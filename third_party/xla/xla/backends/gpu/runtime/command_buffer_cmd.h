/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_BACKENDS_GPU_RUNTIME_COMMAND_BUFFER_CMD_H_
#define XLA_BACKENDS_GPU_RUNTIME_COMMAND_BUFFER_CMD_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "xla/backends/gpu/collectives/gpu_clique_key.h"
#include "xla/backends/gpu/runtime/collective_thunk.h"
#include "xla/backends/gpu/runtime/custom_call_thunk.h"
#include "xla/backends/gpu/runtime/dynamic_slice_thunk.h"
#include "xla/backends/gpu/runtime/gpublas_lt_matmul_thunk.h"
#include "xla/backends/gpu/runtime/thunk.h"
#include "xla/ffi/api/c_api.h"
#include "xla/ffi/call_frame.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/runtime/buffer_use.h"
#include "xla/runtime/execution_graph.h"
#include "xla/runtime/object_pool.h"
#include "xla/runtime/resource_use.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/gpu/buffer_allocations.h"
#include "xla/service/gpu/kernels/custom_kernel.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/matmul_utils.h"
#include "xla/shape.h"
#include "xla/stream_executor/command_buffer.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/dnn.h"
#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/memory_allocation.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream.h"
#include "xla/stream_executor/stream_executor.h"
#include "xla/tsl/lib/gtl/int_type.h"

namespace xla::gpu {

// clang-format off
#define COMMAND_BUFFER_CMD_LIST(V)                       \
  V(kEmptyCmd, "EmptyCmd")                               \
  V(kTracedCommandBufferCmd, "TracedCommandBufferCmd")   \
  V(kComputationIdCmd, "ComputationIdCmd")               \
  V(kLaunchCmd, "LaunchCmd")                             \
  V(kCustomKernelLaunchCmd, "CustomKernelLaunchCmd")     \
  V(kCublasLtCmd, "CublasLtCmd")                         \
  V(kCuDnnCmd, "CuDnnCmd")                               \
  V(kGemmCmd, "GemmCmd")                                 \
  V(kMemcpyDeviceToDeviceCmd, "MemcpyDeviceToDeviceCmd") \
  V(kMemzeroCmd, "MemzeroCmd")                           \
  V(kMemset32Cmd, "Memset32Cmd")                         \
  V(kCaseCmd, "CaseCmd")                                 \
  V(kWhileCmd, "WhileCmd")                               \
  V(kCustomCallCmd, "CustomCallCmd")                     \
  V(kBarrierCmd, "BarrierCmd")                           \
  V(kCollectiveCmd, "CollectiveCmd")                     \
  V(kAllReduceCmd, "AllReduceCmd")                       \
  V(kReduceScatter, "ReduceScatterCmd")                  \
  V(kAllToAll, "AllToAllCmd")                            \
  V(kAllGatherCmd, "AllGatherCmd")                       \
  V(kCollectiveBroadcastCmd, "CollectiveBroadcastCmd")   \
  V(kDynamicSliceFusionCmd, "DynamicSliceFusionCmd")     \
  V(kUnknownCmd, "UnknownCmd") \
  // clang-format on

enum class CommandBufferCmdType : int32_t {
#define DECLARE_ENUM(enum_name, cmd_name, ...) enum_name,
  COMMAND_BUFFER_CMD_LIST(DECLARE_ENUM)
#undef DECLARE_ENUM
};

std::string CommandBufferCmdString(CommandBufferCmdType type);

//===----------------------------------------------------------------------===//
// CommandBufferCmd
//===----------------------------------------------------------------------===//

// Command is a Thunk counterpart that instead of launching operations directly
// on the underlying device records them into command buffers.
//
// Commands have the same execution stages as thunks as they are executed by a
// command buffer thunk: Prepare, Initialize and Record (Execute). See Thunk
// documentation for details.
//
// Commands must be thread safe as they can be recorded into multiple command
// buffers concurrently on different stream executors.

using ResourceUseVector = absl::InlinedVector<ResourceUse, 1>;

class CommandBufferCmd {
 public:
  CommandBufferCmd(CommandBufferCmdType cmd_type,
                   ExecutionStreamId execution_stream_id,
                   ResourceUseVector resources,
                   se::StreamPriority priority = se::StreamPriority::Default)
      : cmd_type_(cmd_type),
        execution_stream_id_(execution_stream_id),
        resources_(std::move(resources)),
        priority_(priority) {}

  virtual ~CommandBufferCmd() = default;

  using BufferUseVector = absl::InlinedVector<BufferUse, 4>;

  // A base class for externally managed command state.
  //
  // Commands can be executed concurrently for many stream executors (underlying
  // devices) and command buffers. Managing per-executor state can become
  // expensive as it requires synchronization. Furthermore the number of command
  // buffers command is recorded into is unbounded as they come and go (command
  // buffers evicted and reconstructed) which makes it hard to manage the
  // lifetime of resources attached to command buffers.
  //
  // Externally managed state (owned and synchronized by CommandBufferThunk)
  // allows commands to attach a piece of information to command buffer in a
  // safe and performant way.
  class State {
   public:
    virtual ~State() = default;
  };

  // An external manager for a state attached to commands recorded into command
  // buffers (same command can be recorded into multiple command buffers).
  class StateManager {
   public:
    virtual ~StateManager() = default;

    template <typename ConcreteState>
    ConcreteState* GetOrNull(const CommandBufferCmd* cmd,
                             const se::CommandBuffer* command_buffer) {
      static_assert(std::is_base_of_v<State, ConcreteState>);
      return static_cast<ConcreteState*>(
          GetOrNull(cmd, command_buffer, GetTypeId<ConcreteState>()));
    }

    template <typename ConcreteState>
    ConcreteState* GetOrCreate(
        const CommandBufferCmd* cmd, const se::CommandBuffer* command_buffer,
        absl::FunctionRef<std::unique_ptr<ConcreteState>()> create) {
      static_assert(std::is_base_of_v<State, ConcreteState>);
      return static_cast<ConcreteState*>(GetOrCreate(cmd, command_buffer,
                                                     GetTypeId<ConcreteState>(),
                                                     [&] { return create(); }));
    }

    template <typename ConcreteState>
    ConcreteState* GetOrCreate(const CommandBufferCmd* cmd,
                               const se::CommandBuffer* command_buffer) {
      return GetOrCreate<ConcreteState>(cmd, command_buffer, [] {
        return std::make_unique<ConcreteState>();
      });
    }

   private:
    // We use TypeId to distinguish between different state types.
    TSL_LIB_GTL_DEFINE_INT_TYPE(TypeId, int64_t);

    template <typename F>
    static TypeId GetTypeId() {
      static const TypeId id = GetNextTypeId();
      return id;
    }

    static TypeId GetNextTypeId();

    State* GetOrNull(const CommandBufferCmd* cmd,
                     const se::CommandBuffer* command_buffer, TypeId type_id);

    State* GetOrCreate(const CommandBufferCmd* cmd,
                       const se::CommandBuffer* command_buffer, TypeId type_id,
                       absl::FunctionRef<std::unique_ptr<State>()> create);

    using Key =
        std::tuple<const CommandBufferCmd*, const se::CommandBuffer*, TypeId>;
    absl::flat_hash_map<Key, std::unique_ptr<State>> state_;
  };

  // Parameters for recording commands into the command buffer.
  struct RecordParams {
    // An external state manager that gives efficient access to per-device state
    // to commands without a need to add expensive synchronization.
    StateManager& state;

    // Buffer allocations that changed since the last call to `Record`. Buffer
    // allocation indices are sorted. CommandBufferCmdExecutor and individual
    // commands rely on this information to skip unnecessary updates.
    std::optional<std::vector<BufferAllocation::Index>> updated_allocs;

    // A flag indicating whether we record comands at command buffer thunk
    // initialization time.
    bool is_initialization = false;
  };

  // Create new commands in the command buffer using the given dependencies.
  struct RecordCreate {
    absl::Span<const se::CommandBuffer::Command* const> dependencies;
  };

  // Update previously recorded commands in the command buffer.
  struct RecordUpdate {
    const se::CommandBuffer::Command* command;
  };

  // When recording a command into the command buffer we can either update
  // previously recorded commands or create new ones. The command DAG structure
  // can be defined only when we record commands the first time, after that we
  // can only update previously recorded commands parameters (i.e. with pointers
  // to new buffer allocations).
  using RecordAction = std::variant<RecordCreate, RecordUpdate>;

  // See Thunk documentation for XLA execution stages (prepare, initialize,
  // execute). Commands mirror thunks as they are executed as CommandBufferThunk
  // that is plugged into the Thunk execution cycle.

  // Prepare command for execution by allowing command to request shared state
  // required for recording (i.e. collective commands request cliques).
  virtual absl::Status Prepare(
      const Thunk::PrepareParams& params,
      Thunk::ResourceRequestsInterface& resource_requests) {
    return absl::OkStatus();
  }

  // Initialize a command for recording on a given executor. We split it into a
  // separate function to allow expensive initialization (e.g. device kernel
  // loading) to happen before a command buffer thunk execution.
  virtual absl::Status Initialize(const Thunk::InitializeParams& params,
                                  StateManager& state) {
    return absl::OkStatus();
  }

  // Records commands into the command buffer. Returned commands will be passed
  // back on the next call to `Record` into the same command buffer, so that it
  // can do efficient command buffer updates.
  virtual absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) = 0;

  // Returns true if command requires initialization (has to be recorded at
  // command buffer thunk initialization).
  //
  // Today this is only true for collective commands that might use NCCL for
  // communication. With NCCL, all participating ranks must record collective
  // commands at the same time, if some ranks will skip command updates (because
  // they got lucky and got the same buffer allocations), it will lead to
  // deadlocks. By forcing the command update at thunk initialization time, we
  // ensure that all ranks execute NCCL command update.
  virtual bool requires_initialization() { return false; }

  // Returns all buffers used by the cmd. These will be used to track cmd
  // updates, thus they need to be consistent across calls to the function.
  virtual BufferUseVector buffers() const = 0;
  ResourceUseVector resources() const { return resources_; }

  // Returns true if command implemented as a nested command buffer.
  virtual bool IsNestedCommandBuffer() const { return false; }

  absl::string_view profile_annotation() const { return profile_annotation_; }
  void set_profile_annotation(absl::string_view profile_annotation) {
    profile_annotation_ = profile_annotation;
  }

  CommandBufferCmdType command_type() const { return cmd_type_; }
  se::StreamPriority priority() const { return priority_; }
  void set_priority(se::StreamPriority priority) { priority_ = priority; }

  virtual std::string ToString() const {
    return CommandBufferCmdString(cmd_type_);
  }

  ExecutionStreamId execution_stream_id() const { return execution_stream_id_; }

 private:
  std::string profile_annotation_;
  CommandBufferCmdType cmd_type_;
  ExecutionStreamId execution_stream_id_;
  ResourceUseVector resources_;

  // Command priority, currently only support default, lowest and highest
  // priority.
  se::StreamPriority priority_ = se::StreamPriority::Default;
};

// A sequence of commands (corresponds to a ThunkSequence from the Thunk API).
class CommandBufferCmdSequence
    : public std::vector<std::unique_ptr<CommandBufferCmd>> {
 public:
  template <typename Command, typename... Args>
  void Emplace(Args&&... args) {
    this->emplace_back(std::make_unique<Command>(std::forward<Args>(args)...));
  }
};

//===----------------------------------------------------------------------===//
// CommandBufferCmdExecutor
//===----------------------------------------------------------------------===//

// Command executor is responsible for recording commands sequence into the
// underlying command buffer and setting up dependencies between commands.
class CommandBufferCmdExecutor {
 public:
  CommandBufferCmdExecutor() = default;
  CommandBufferCmdExecutor(CommandBufferCmdExecutor&&) = default;
  CommandBufferCmdExecutor& operator=(CommandBufferCmdExecutor&&) = default;

  using RecordParams = CommandBufferCmd::RecordParams;

  // Synchronization mode defines how much concurrency is allowed between
  // commands in the sequence.
  enum class SynchronizationMode {
    // Serializes execution of all commands recorded into the command buffer
    // by adding a dependency between them.
    kSerialize,

    // Relies on execution graph to insert dependencies between commands
    // that have buffer of resource conflicts, and building a DAG of commands.
    kAutomatic
  };

  template <typename Sink>
  friend void AbslStringify(Sink& sink, SynchronizationMode mode) {
    switch (mode) {
      case SynchronizationMode::kSerialize:
        sink.Append("serialize");
        break;
      case SynchronizationMode::kAutomatic:
        sink.Append("automatic");
        break;
    }
  }

  // Creates a command executor from a sequence of commands using given
  // synchronization mode.
  static absl::StatusOr<CommandBufferCmdExecutor> Create(
      CommandBufferCmdSequence commands,
      SynchronizationMode synchronization_mode);

  // Prepares all commands added to a sequence.
  absl::Status Prepare(const Thunk::PrepareParams& params,
                       Thunk::ResourceRequestsInterface& resource_requests);

  // Initializes all commands added to a sequence.
  absl::Status Initialize(const Thunk::InitializeParams& params,
                          CommandBufferCmd::StateManager& state);

  // Records commands into the command buffer. This method automatically
  // switches between `RecordCreate` or `RecordUpdate` depending on the command
  // buffer state. This method assumes that no other command buffer sequence is
  // recorded into the same command buffer, and doesn't set up initial
  // dependencies for recorded commands.
  absl::Status Record(const Thunk::ExecuteParams& execute_params,
                      const RecordParams& record_params,
                      se::CommandBuffer* command_buffer);

  // Records command creation into the command buffer. Command buffer must be
  // in create state. The next command sequence recorded into the same command
  // buffer must use returned commands as dependencies, to guarantee that it is
  // correctly ordered after this command sequence.
  absl::StatusOr<std::vector<const se::CommandBuffer::Command*>> RecordCreate(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, se::CommandBuffer* command_buffer,
      absl::Span<const se::CommandBuffer::Command* const> dependencies) const;

  // Records command updates into the command buffer. Command buffer must be
  // in update state.
  absl::Status RecordUpdate(const Thunk::ExecuteParams& execute_params,
                            const RecordParams& record_params,
                            se::CommandBuffer* command_buffer) const;

  // Returns buffers referenced by commands in this sequence.
  const absl::flat_hash_set<BufferUse>& buffers() const;

  // Returns buffer allocations indices referenced by commands in this sequence.
  absl::Span<const BufferAllocation::Index> allocs_indices() const;

  bool empty() const { return commands_.empty(); }
  size_t size() const { return commands_.size(); }

  bool requires_initialization() const {
    return absl::c_any_of(commands_, [](const auto& cmd) {
      return cmd->requires_initialization();
    });
  }

 private:
  // We use index into the `commands_` vector as a command id.
  using CommandId = int64_t;

  // A state associated with commands in the sequence. We rely on this state to
  // efficiently update command recorded into the command buffer.
  struct RecordState : public CommandBufferCmd::State {
    const se::CommandBuffer::Command* command;
  };

  CommandBufferCmdExecutor(SynchronizationMode synchronization_mode,
                           CommandBufferCmdSequence commands,
                           std::optional<ExecutionGraph> execution_graph);

  absl::Status CheckCommandBufferState(
      se::CommandBuffer* command_buffer,
      se::CommandBuffer::State expected_state) const;

  // Returns true if command has no dependencies.
  bool IsSource(CommandId id) const;

  // Returns true if command is not a dependency of any other commands.
  bool IsSink(CommandId id) const;

  // Returns dependencies of the command with the given id.
  std::vector<const se::CommandBuffer::Command*> Dependencies(
      const RecordParams& record_params, se::CommandBuffer* command_buffer,
      CommandId id) const;

  SynchronizationMode synchronization_mode_;
  CommandBufferCmdSequence commands_;

  // In automatic synchronization mode we build an execution graph for the
  // sequence of commands and use it to set up dependencies between commands.
  std::optional<ExecutionGraph> execution_graph_;

  // Buffers referenced by commands in this sequence.
  absl::flat_hash_set<BufferUse> buffers_;

  // Unique buffer allocations indices referenced by all commands in this
  // sequence (sorted by the buffer allocation index).
  std::vector<BufferAllocation::Index> allocs_indices_;

  // A mapping from command id to unique buffer allocations indices referenced
  // by the command (sorted by the buffer allocation index).
  std::vector<std::vector<BufferAllocation::Index>> cmd_allocs_indices_;
};

//===----------------------------------------------------------------------===//
// TracedCommandBuffer
//===----------------------------------------------------------------------===//

// A cache for traced command buffers that will re-trace on change in buffer
// allocations that are relevant for `buffers` passed to constructor. We use a
// very simple most-recently-used cache of traced command buffers as in practice
// subsequent calls to XLA executable tend to reuse the same allocations.
class TracedCommandBuffer : public CommandBufferCmd::State {
 public:
  explicit TracedCommandBuffer(const CommandBufferCmd* trace_cmd,
                               CommandBufferCmd::BufferUseVector buffers,
                               int64_t capacity = 16);

  // Returns cached command buffer traced using the same buffer addresses or
  // traces and caches a new command buffer using user provided callback.
  absl::StatusOr<se::CommandBuffer*> GetOrTraceCommandBuffer(
      const BufferAllocations* buffer_allocation, se::StreamExecutor* executor,
      se::Stream* stream, absl::FunctionRef<absl::Status(se::Stream*)> trace,
      se::StreamPriority priority = se::StreamPriority::Default);

 private:
  std::vector<BufferAllocation::Index> allocs_indices_;

  struct Entry {
    std::vector<se::DeviceMemoryBase> recorded_allocs;
    std::unique_ptr<se::CommandBuffer> command_buffer;
  };
  const CommandBufferCmd* trace_cmd_;
  int64_t capacity_;
  std::vector<Entry> entries_;
};

//===----------------------------------------------------------------------===//
// TracedCommandBufferCmd
//===----------------------------------------------------------------------===//

// A base class for commands implemented as tracing of stream activities.
class TracedCommandBufferCmd : public CommandBufferCmd {
 protected:
  explicit TracedCommandBufferCmd(CommandBufferCmdType cmd_type,
                                  ExecutionStreamId execution_stream_id,
                                  ResourceUseVector resources = {});

  // Creates a command buffer by calling a user-provided `trace` function and
  // adds it as a nested command to `command_buffer`. Traced command buffers
  // cached and reused in an instance of `TracedCommandBuffer` kept in `state`.
  absl::StatusOr<const se::CommandBuffer::Command*> RecordTracedCommand(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer,
      absl::FunctionRef<absl::Status(se::Stream*)> trace);
};

//===----------------------------------------------------------------------===//
// EmptyCmd
//===----------------------------------------------------------------------===//

class EmptyCmd : public CommandBufferCmd {
 public:
  EmptyCmd(ExecutionStreamId execution_stream_id,
           ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override { return {}; }
};

//===----------------------------------------------------------------------===//
// ComputationIdCmd (ReplicaId and PartitionId)
//===----------------------------------------------------------------------===//

class ComputationIdCmd : public CommandBufferCmd {
 public:
  enum class Kind { kReplica, kPartition };

  ComputationIdCmd(ExecutionStreamId execution_stream_id,
                   BufferAllocation::Slice dest, Kind kind,
                   ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

 private:
  BufferAllocation::Slice dest_;
  Kind kind_;
};

//===----------------------------------------------------------------------===//
// LaunchCmd
//===----------------------------------------------------------------------===//

class LaunchCmd : public CommandBufferCmd {
 public:
  LaunchCmd(ExecutionStreamId execution_stream_id, std::string kernel_name,
            absl::Span<const BufferAllocation::Slice> args,
            absl::Span<const BufferUse::MemoryAccess> args_access,
            LaunchDimensions dims, int64_t shmem_bytes,
            ResourceUseVector resources = {});

  absl::Status Initialize(const Thunk::InitializeParams& params,
                          StateManager& state) override;

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

 private:
  std::string kernel_name_;
  std::vector<BufferAllocation::Slice> args_;
  std::vector<BufferUse::MemoryAccess> args_access_;
  LaunchDimensions dims_;
  int64_t shmem_bytes_;

  // Command sequence can be recorded concurrently for multiple command buffers
  // on different stream executors and we need to synchronize mutable state.
  absl::Mutex mutex_;
  absl::flat_hash_map<se::StreamExecutor*, std::unique_ptr<se::Kernel>> kernels_
      ABSL_GUARDED_BY(mutex_);
};

//===----------------------------------------------------------------------===//
// CustomKenelLaunchCmd
//===----------------------------------------------------------------------===//

class CustomKernelLaunchCmd : public CommandBufferCmd {
 public:
  CustomKernelLaunchCmd(ExecutionStreamId execution_stream_id,
                        absl::Span<const BufferAllocation::Slice> args,
                        absl::Span<const BufferUse::MemoryAccess> args_access,
                        CustomKernel custom_kernel,
                        ResourceUseVector resources = {});

  absl::Status Initialize(const Thunk::InitializeParams& params,
                          StateManager& state) override;

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

 private:
  std::vector<BufferAllocation::Slice> args_;
  std::vector<BufferUse::MemoryAccess> args_access_;
  CustomKernel custom_kernel_;

  // Command sequence can be recorded concurrently for multiple command buffers
  // on different stream executors and we need to synchronize mutable state.
  absl::Mutex mutex_;
  absl::flat_hash_map<se::StreamExecutor*, std::unique_ptr<se::Kernel>> kernels_
      ABSL_GUARDED_BY(mutex_);
};

//===----------------------------------------------------------------------===//
// MemcpyDeviceToDeviceCmd
//===----------------------------------------------------------------------===//

class MemcpyDeviceToDeviceCmd : public CommandBufferCmd {
 public:
  MemcpyDeviceToDeviceCmd(ExecutionStreamId execution_stream_id,
                          BufferAllocation::Slice dst,
                          BufferAllocation::Slice src, int64_t num_bytes,
                          ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

 private:
  BufferAllocation::Slice dst_;
  BufferAllocation::Slice src_;
  int64_t num_bytes_;
};

//===----------------------------------------------------------------------===//
// MemzeroCmd
//===----------------------------------------------------------------------===//

class MemzeroCmd : public CommandBufferCmd {
 public:
  MemzeroCmd(ExecutionStreamId execution_stream_id, BufferAllocation::Slice dst,
             ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

 private:
  BufferAllocation::Slice dst_;
};

//===----------------------------------------------------------------------===//
// Memset32Cmd
//===----------------------------------------------------------------------===//

class Memset32Cmd : public CommandBufferCmd {
 public:
  Memset32Cmd(ExecutionStreamId execution_stream_id,
              BufferAllocation::Slice dst, uint32_t bit_pattern,
              ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

 private:
  BufferAllocation::Slice dst_;
  uint32_t bit_pattern_;
};

//===----------------------------------------------------------------------===//
// CaseCmd
//===----------------------------------------------------------------------===//

class CaseCmd : public CommandBufferCmd {
 public:
  CaseCmd(ExecutionStreamId execution_stream_id, BufferAllocation::Slice index,
          bool index_is_bool, std::vector<CommandBufferCmdExecutor> branches,
          ResourceUseVector resources = {});

  absl::Status Initialize(const Thunk::InitializeParams& params,
                          StateManager& state) override;

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  bool requires_initialization() override;

  BufferUseVector buffers() const override;

 private:
  BufferAllocation::Slice index_;
  bool index_is_bool_;
  std::vector<CommandBufferCmdExecutor> branches_;
};

//===----------------------------------------------------------------------===//
// WhileCmd
//===----------------------------------------------------------------------===//

class WhileCmd : public CommandBufferCmd {
 public:
  WhileCmd(ExecutionStreamId execution_stream_id, BufferAllocation::Slice pred,
           CommandBufferCmdExecutor cond_commands,
           CommandBufferCmdExecutor body_commands,
           ResourceUseVector resources = {});

  absl::Status Initialize(const Thunk::InitializeParams& params,
                          StateManager& state) override;

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  bool requires_initialization() override;

  BufferUseVector buffers() const override;

 private:
  BufferAllocation::Slice pred_;
  CommandBufferCmdExecutor cond_commands_;
  CommandBufferCmdExecutor body_commands_;
};

//===----------------------------------------------------------------------===//
// GemmCmd
//===----------------------------------------------------------------------===//

class GemmCmd : public TracedCommandBufferCmd {
 public:
  GemmCmd(ExecutionStreamId execution_stream_id, GemmConfig config,
          const BufferAllocation::Slice& lhs_buffer,
          const BufferAllocation::Slice& rhs_buffer,
          const BufferAllocation::Slice& output_buffer,
          const BufferAllocation::Slice& workspace, bool deterministic,
          ResourceUseVector resources = {});

  absl::Status Initialize(const Thunk::InitializeParams& params,
                          StateManager& state) override;

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

  bool IsNestedCommandBuffer() const final { return true; }

 private:
  const GemmConfig config_;
  const BufferAllocation::Slice lhs_buffer_;
  const BufferAllocation::Slice rhs_buffer_;
  const BufferAllocation::Slice output_buffer_;
  const BufferAllocation::Slice workspace_;
  // Whether to run deterministically.
  const bool deterministic_;
};

//===----------------------------------------------------------------------===//
// CublasLtCmd
//===----------------------------------------------------------------------===//

class CublasLtCmd : public TracedCommandBufferCmd, public CublasLtMatmulThunk {
 public:
  CublasLtCmd(ExecutionStreamId execution_stream_id,
              const CublasLtMatmulThunk& matmul_thunk,
              ResourceUseVector resources = {});

  absl::Status Initialize(const Thunk::InitializeParams& params,
                          StateManager& state) override;

  // This is needed to avoid compile errors about "shadowed" virtual function
  absl::Status Initialize(const InitializeParams& params) override {
    return CublasLtMatmulThunk::Initialize(params);
  }

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

  bool IsNestedCommandBuffer() const final { return true; }
};

//===----------------------------------------------------------------------===//
// CuDnnCmd
//===----------------------------------------------------------------------===//

class CuDnnCmd : public TracedCommandBufferCmd {
 public:
  CuDnnCmd(ExecutionStreamId execution_stream_id,
           absl::Span<const BufferAllocation::Slice> args,
           std::shared_ptr<se::dnn::LazyDnnGraph> graph,
           ResourceUseVector resources = {});

  absl::Status Initialize(const Thunk::InitializeParams& params,
                          StateManager& state) override;

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

  bool IsNestedCommandBuffer() const final { return true; }

 private:
  std::vector<BufferAllocation::Slice> args_;
  const std::shared_ptr<se::dnn::LazyDnnGraph> graph_;
};

//===----------------------------------------------------------------------===//
// CustomCallCmd
//===----------------------------------------------------------------------===//

class CustomCallCmd : public CommandBufferCmd {
 public:
  using Slice = CustomCallThunk::Slice;
  using CustomCallTarget = CustomCallThunk::CustomCallTarget;
  using AttributesMap = CustomCallThunk::AttributesMap;

  // This is a legacy custom call API that is discouraged, and will be
  // deprecated once XLA:FFI mechanism is ready.
  CustomCallCmd(ExecutionStreamId execution_stream_id, std::string target_name,
                CustomCallTarget call_target,
                std::vector<std::optional<Slice>> operands,
                std::vector<std::optional<Slice>> results,
                absl::string_view opaque, ResourceUseVector resources = {})
      : CommandBufferCmd(CommandBufferCmdType::kCustomCallCmd,
                         execution_stream_id, resources),
        target_name_(std::move(target_name)),
        call_target_(std::move(call_target)),
        opaque_(opaque),
        operands_(std::move(operands)),
        results_(std::move(results)) {}

  CustomCallCmd(ExecutionStreamId execution_stream_id, std::string target_name,
                XLA_FFI_Handler* handler,
                std::vector<std::optional<Slice>> operands,
                std::vector<std::optional<Slice>> results,
                ffi::CallFrame call_frame,
                const HloComputation* called_computation,
                ResourceUseVector resources = {})
      : CommandBufferCmd(CommandBufferCmdType::kCustomCallCmd,
                         execution_stream_id, resources),
        target_name_(std::move(target_name)),
        handler_(handler),
        call_frame_(std::move(call_frame)),
        call_frames_([this] { return call_frame_->Copy(); }),
        called_computation_(called_computation),
        operands_(std::move(operands)),
        results_(std::move(results)) {}

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;
  bool IsNestedCommandBuffer() const final { return true; }

 private:
  absl::StatusOr<const se::CommandBuffer::Command*> RecordLegacyCustomCall(
      const Thunk::ExecuteParams& execute_param,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer);

  absl::StatusOr<const se::CommandBuffer::Command*> RecordXlaFfiCall(
      const Thunk::ExecuteParams& execute_param,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer);

  std::string target_name_;

  // This is a legacy custom call API that is discouraged, and will be
  // deprecated once XLA:FFI mechanism is ready.
  CustomCallTarget call_target_;
  std::string opaque_;

  // XLA FFI provides a right type safe mechanism for registering external
  // functions with XLA runtime. It's under construction, and still misses
  // a lot of features. Long term it will replace legacy custom calls.
  XLA_FFI_Handler* handler_ = nullptr;

  // Reference call frame pre-initialized at construction time.
  std::optional<ffi::CallFrame> call_frame_;

  // A pool of call frames used at run time. Newly created call frames are
  // copied from the reference call frame and updated with buffer addresses.
  std::optional<ObjectPool<ffi::CallFrame>> call_frames_;

  const HloComputation* called_computation_;

  std::vector<std::optional<Slice>> operands_;
  std::vector<std::optional<Slice>> results_;
};

//===----------------------------------------------------------------------===//
// CollectiveCmd
//===----------------------------------------------------------------------===//

class CollectiveCmd : public CommandBufferCmd {
 public:
  CollectiveCmd(CommandBufferCmdType cmd_type,
                ExecutionStreamId execution_stream_id,
                ExecutionStreamId async_from_stream_id, CollectiveConfig config,
                ResourceUseVector resources = {});

  absl::Status Prepare(
      const Thunk::PrepareParams& params,
      Thunk::ResourceRequestsInterface& resource_requests) final;

  bool requires_initialization() override { return true; }

  bool IsNestedCommandBuffer() const final { return true; }

  absl::StatusOr<const se::CommandBuffer::Command*> RecordTracedCommand(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer,
      absl::FunctionRef<absl::Status(se::Stream*)> trace);

  virtual AsyncStreamKind GetAsyncStreamKind() = 0;

  bool IsAsync() const {
    return async_from_stream_id_ != execution_stream_id();
  }

  CollectiveStreamId nccl_stream_id() {
    return xla::gpu::GetCollectiveStreamId(IsAsync(), GetAsyncStreamKind());
  }

  ExecutionStreamId async_from_stream_id() const {
    return async_from_stream_id_;
  }

 protected:
  const CollectiveConfig& config() const { return config_; }

 private:
  ExecutionStreamId async_from_stream_id_;
  CollectiveConfig config_;
};

//===----------------------------------------------------------------------===//
// AllReduceCmd
//===----------------------------------------------------------------------===//

class AllReduceCmd : public CollectiveCmd {
 public:
  AllReduceCmd(ExecutionStreamId execution_stream_id,
               ExecutionStreamId async_from_stream_id, CollectiveConfig config,
               ReductionKind reduction_kind,
               absl::Span<const CollectiveThunk::Buffer> buffers,
               ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

  AsyncStreamKind GetAsyncStreamKind() override {
    return AsyncStreamKind::kCollective;
  };

 private:
  ReductionKind reduction_kind_;
  std::vector<CollectiveThunk::Buffer> buffers_;
};

//===----------------------------------------------------------------------===//
// ReduceScatterCmd
//===----------------------------------------------------------------------===//

class ReduceScatterCmd : public CollectiveCmd {
 public:
  ReduceScatterCmd(ExecutionStreamId execution_stream_id,
                   ExecutionStreamId async_from_stream_id,
                   CollectiveConfig config, ReductionKind reduction_kind,
                   absl::Span<const CollectiveThunk::Buffer> buffers,
                   ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

  AsyncStreamKind GetAsyncStreamKind() override {
    return AsyncStreamKind::kCollective;
  };

 private:
  ReductionKind reduction_kind_;
  std::vector<CollectiveThunk::Buffer> buffers_;
};

//===----------------------------------------------------------------------===//
// AllToAllCmd
//===----------------------------------------------------------------------===//

class AllToAllCmd : public CollectiveCmd {
 public:
  AllToAllCmd(ExecutionStreamId execution_stream_id,
              ExecutionStreamId async_from_stream_id, CollectiveConfig config,
              bool has_split_dimension,
              absl::Span<const CollectiveThunk::Buffer> buffers,
              ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

  AsyncStreamKind GetAsyncStreamKind() override {
    return AsyncStreamKind::kCollective;
  };

 private:
  bool has_split_dimension_;
  std::vector<CollectiveThunk::Buffer> buffers_;
};

//===----------------------------------------------------------------------===//
// AllGatherCmd
//===----------------------------------------------------------------------===//

class AllGatherCmd : public CollectiveCmd {
 public:
  AllGatherCmd(ExecutionStreamId execution_stream_id,
               ExecutionStreamId async_from_stream_id, CollectiveConfig config,
               absl::Span<const CollectiveThunk::Buffer> buffers,
               ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

  AsyncStreamKind GetAsyncStreamKind() override {
    return AsyncStreamKind::kCollective;
  };

 private:
  std::vector<CollectiveThunk::Buffer> buffers_;
};

//===----------------------------------------------------------------------===//
// CollectiveBroadcastCmd
//===----------------------------------------------------------------------===//

class CollectiveBroadcastCmd : public CollectiveCmd {
 public:
  CollectiveBroadcastCmd(ExecutionStreamId execution_stream_id,
                         ExecutionStreamId async_from_stream_id,
                         CollectiveConfig config,
                         absl::Span<const CollectiveThunk::Buffer> buffers,
                         ResourceUseVector resources = {});

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

 private:
  std::vector<CollectiveThunk::Buffer> buffers_;
};

//===----------------------------------------------------------------------===//
// DynamicSliceFusionCmd
//===----------------------------------------------------------------------===//

class DynamicSliceFusionCmd : public CommandBufferCmd {
 public:
  DynamicSliceFusionCmd(
      ExecutionStreamId execution_stream_id,
      CommandBufferCmdExecutor embedded_commands,
      std::vector<std::optional<BufferAllocation::Slice>> arguments,
      std::vector<std::unique_ptr<BufferAllocation>> fake_allocations_,
      std::vector<std::optional<std::vector<DynamicSliceThunk::Offset>>>
          offsets,
      std::vector<std::optional<Shape>> orig_shapes,
      std::vector<std::optional<Shape>> sliced_shapes,
      std::vector<std::optional<uint64_t>> offset_byte_sizes,
      ResourceUseVector resources = {});

  absl::Status Initialize(const Thunk::InitializeParams& params,
                          StateManager& state) override;

  absl::Status Prepare(
      const Thunk::PrepareParams& params,
      Thunk::ResourceRequestsInterface& resource_requests) final;

  absl::StatusOr<const se::CommandBuffer::Command*> Record(
      const Thunk::ExecuteParams& execute_params,
      const RecordParams& record_params, RecordAction record_action,
      se::CommandBuffer* command_buffer) override;

  BufferUseVector buffers() const override;

  bool requires_initialization() override;

  bool IsNestedCommandBuffer() const final { return true; }

 private:
  CommandBufferCmdExecutor embedded_commands_;
  std::vector<DynamicSliceThunk::SliceDef> slices_;
  std::vector<std::unique_ptr<BufferAllocation>> fake_allocations_;

  // Pinned host memory for transferring offset values from device to host.
  absl::Mutex mutex_;
  absl::flat_hash_map<se::StreamExecutor*,
                      std::unique_ptr<se::MemoryAllocation>>
      offsets_allocs_ ABSL_GUARDED_BY(mutex_);

  // Pre-computed size requirement for `offsets_allocs_`.
  int64_t offsets_allocs_size_ = 0;

  // A mapping from argument index to the base offset in the `offsets_allocs_`.
  std::vector<int64_t> offsets_allocs_base_;

  // mapping from original allocation index to allocation index of embedded
  // command sequences.
  absl::flat_hash_map<int64_t, std::optional<BufferAllocation::Slice>>
      embeded_to_origin_slice_map_;
};

}  // namespace xla::gpu

#endif  // XLA_BACKENDS_GPU_RUNTIME_COMMAND_BUFFER_CMD_H_

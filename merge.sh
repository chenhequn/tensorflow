#!/bin/bash
ar -M <<EOM
CREATE libtensorflow-all.a
ADDLIB ./build/libtensorflow-lite.a
ADDLIB ./build/cpuinfo/libcpuinfo.a
ADDLIB ./build/pthreadpool/libpthreadpool.a
ADDLIB ./build/_deps/farmhash-build/libfarmhash.a
ADDLIB ./build/_deps/xnnpack-build/libXNNPACK.a
ADDLIB ./build/_deps/fft2d-build/libfft2d_fftsg.a
ADDLIB ./build/_deps/fft2d-build/libfft2d_fftsg2d.a
ADDLIB ./build/_deps/flatbuffers-build/libflatbuffers.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/synchronization/libabsl_synchronization.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/synchronization/libabsl_graphcycles_internal.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/types/libabsl_bad_variant_access.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/types/libabsl_bad_optional_access.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/strings/libabsl_cord.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/strings/libabsl_strings_internal.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/strings/libabsl_strings.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/strings/libabsl_str_format_internal.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/flags/libabsl_flags.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/flags/libabsl_flags_marshalling.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/flags/libabsl_flags_registry.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/flags/libabsl_flags_internal.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/flags/libabsl_flags_config.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/flags/libabsl_flags_program_name.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/base/libabsl_throw_delegate.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/base/libabsl_spinlock_wait.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/base/libabsl_log_severity.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/base/libabsl_raw_logging_internal.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/base/libabsl_dynamic_annotations.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/base/libabsl_base.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/base/libabsl_malloc_internal.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/status/libabsl_status.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/time/libabsl_time.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/time/libabsl_time_zone.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/time/libabsl_civil_time.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/numeric/libabsl_int128.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/debugging/libabsl_stacktrace.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/debugging/libabsl_debugging_internal.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/debugging/libabsl_demangle_internal.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/debugging/libabsl_symbolize.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/hash/libabsl_hash.a
ADDLIB ./build/_deps/abseil-cpp-build/absl/hash/libabsl_city.a
ADDLIB ./build/_deps/ruy-build/libruy.a
ADDLIB ./build/clog/libclog.a
SAVE
END
EOM
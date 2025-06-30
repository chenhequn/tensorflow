#!/bin/bash
#注意：
#提前安装cmake flatbuffers-compiler python
#参考：https://ai.google.dev/edge/litert/build/cmake_arm
#这里的编译脚本基于分支：v2.5.0-rc3

#工具在这里下：
#curl -LO https://storage.googleapis.com/mirror.tensorflow.org/developer.arm.com/media/Files/downloads/gnu-a/8.3-2019.03/binrel/gcc-arm-8.3-2019.03-x86_64-aarch64-linux-gnu.tar.xz
#mkdir -p ${HOME}/toolchains
#tar xvf gcc-arm-8.3-2019.03-x86_64-aarch64-linux-gnu.tar.xz -C ${HOME}/toolchains

#v2.5.0-rc3对应的flatbuffers v1.12.0
# git clone https://github.com/google/flatbuffers.git
# cd flatbuffers
# git checkout v1.12.0

# 使用 CMake 安装：
# cmake -B build .
# cmake --build build -j$(nproc)
# sudo cmake --install build


# 编译器前缀（注意路径必须真实存在）
CROSS_PREFIX="/home/chenhequn/01_project/rk3588_linux_tve1206r/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu"

# 编译器参数
ARMCC_FLAGS="-funsafe-math-optimizations"

# 清理 build 目录
mkdir -p build && cd build

# 执行 CMake 配置
cmake ../tensorflow/lite \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=${CROSS_PREFIX}-gcc \
  -DCMAKE_CXX_COMPILER=${CROSS_PREFIX}-g++ \
  -DCMAKE_C_FLAGS="${ARMCC_FLAGS}" \
  -DCMAKE_CXX_FLAGS="${ARMCC_FLAGS}" \
  -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON \
  -DTFLITE_HOST_TOOLS_DIR=/usr/bin

#后续进入build目录，make编译
#!/bin/sh
# End-to-end SDK smoke test, including CMake discovery and the hot-cache path.
set -eu
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cat > "$scratch/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(elf2_sdk_check LANGUAGES C CXX)
find_package(Vulkan REQUIRED)
add_executable(sdk-check main.cpp)
target_link_libraries(sdk-check PRIVATE Vulkan::Vulkan)
EOF
cat > "$scratch/main.cpp" <<'EOF'
#include <iostream>
#include <vulkan/vulkan.h>
int main() {
    uint32_t count = 0;
    std::cout << "ELF2 SDK";
    return vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
}
EOF
elf2-cmake -S "$scratch" -B "$scratch/build"
# With a valid cache, this command must succeed even without a reachable daemon.
DOCKER_HOST=unix:///nonexistent-sdk-test.sock elf2-sdk run cmake --build "$scratch/build"
file "$scratch/build/sdk-check" | grep 'ARM aarch64'
echo 'SDK_CMAKE_AND_OFFLINE_CACHE_VERIFIED'

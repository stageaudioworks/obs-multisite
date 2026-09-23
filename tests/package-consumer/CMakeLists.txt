# A project outside the core that builds against the installed package — what
# the headless encoder does. If this configures, compiles and links, the
# package is usable from outside this repository.
#
#   cmake -S . -B build -DMULTISITE_INSTALL_CORE=ON && cmake --build build
#   cmake --install build --prefix /tmp/ms
#   cmake -S tests/package-consumer -B /tmp/consumer -DCMAKE_PREFIX_PATH=/tmp/ms
#   cmake --build /tmp/consumer && /tmp/consumer/consumer
cmake_minimum_required(VERSION 3.16)
project(multisite_core_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(multisite_core REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE multisite::core multisite::s3 multisite::cmaf)

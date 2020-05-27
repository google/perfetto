# Copyright (C) 2020 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.13)
project(perfetto-sdk-example)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads)

# Define a static library for Perfetto. In this example, we expect the SDK
# (perfetto.cc and perfetto.h) to live in the top level sdk/ directory.
include_directories(../../sdk)
add_library(perfetto STATIC ../../sdk/perfetto.cc)

# Link the library to the main executables.
add_executable(example example.cc trace_categories.cc)
add_executable(example_system_wide example_system_wide.cc
               trace_categories.cc)
add_executable(example_custom_data_source example_custom_data_source.cc)

target_link_libraries(example perfetto
                      ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(example_system_wide perfetto
                      ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(example_custom_data_source perfetto
                      ${CMAKE_THREAD_LIBS_INIT})

# On Android we also need the logging library.
if (ANDROID)
  target_link_libraries(example log)
  target_link_libraries(example_system_wide log)
  target_link_libraries(example_custom_data_source log)
endif (ANDROID)
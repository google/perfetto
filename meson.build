# Copyright (C) 2021 The Android Open Source Project
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

# This meson project only supports the Perfetto SDK
# available from release branches.
# https://perfetto.dev/docs/instrumentation/tracing-sdk

project(
    'perfetto',
    ['c','cpp'],
    default_options: ['c_std=c99', 'cpp_std=c++11']
)

fs = import('fs')

if not fs.is_dir('sdk')
    error('sdk dir not found, please checkout a release tag, e.g. v14.0')
endif

dep_threads = dependency('threads')

lib_perfetto = static_library(
    'perfetto',
    sources: 'sdk/perfetto.cc',
    dependencies: dep_threads,
)

inc_perfetto = include_directories('sdk')

dep_perfetto = declare_dependency(
    link_with: lib_perfetto,
    include_directories: inc_perfetto,
)

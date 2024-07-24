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
    ['cpp'],
)

fs = import('fs')

if not fs.is_dir('sdk')
    error('sdk dir not found, please checkout a release tag, e.g. v14.0')
endif

cpp = meson.get_compiler('cpp')

deps_perfetto = [dependency('threads')]

if host_machine.system() == 'android'
  deps_perfetto += cpp.find_library('log')
endif

lib_perfetto = static_library(
    'perfetto',
    sources: 'sdk/perfetto.cc',
    dependencies: deps_perfetto,
    install: true,
    cpp_args: '-std=c++17',
)

inc_perfetto = include_directories('sdk')

dir_perfetto_trace = join_paths(meson.current_source_dir(),
                                'protos/perfetto/trace')

install_data(dir_perfetto_trace / 'perfetto_trace.proto')

dep_perfetto = declare_dependency(
    link_with: lib_perfetto,
    include_directories: inc_perfetto,
    variables: {
        'pkgdatadir': dir_perfetto_trace,
    }
)

#!/usr/bin/env python
# Copyright (C) 2017 The Android Open Source Project
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

import os
import subprocess
import sys

PROTOS = (
  'perfetto/config/chrome/chrome_config.proto',
  'perfetto/config/data_source_config.proto',
  'perfetto/config/inode_file/inode_file_config.proto',
  'perfetto/config/process_stats/process_stats_config.proto',
  'perfetto/config/data_source_descriptor.proto',
  'perfetto/config/ftrace/ftrace_config.proto',
  'perfetto/config/trace_config.proto',
  'perfetto/config/test_config.proto',
  'perfetto/common/commit_data_request.proto',
)

HEADER_PATH = 'include/perfetto/tracing/core'
CPP_PATH = 'src/tracing/core'
INCLUDE_PATH = 'perfetto/tracing/core'


def run(cmd):
  print('\nRunning ' + ' '.join(cmd))
  subprocess.check_call(cmd)


def main():
  if not os.path.exists('.gn'):
    print('This script must be executed from the perfetto root directory')
    return 1
  if len(sys.argv) < 2:
    print('Usage: %s out/xxx' % sys.argv[0])
    return 1
  out_dir = sys.argv[1]
  clang_format = ['clang-format', '-i', '--sort-includes']
  tool = os.path.join(out_dir, 'proto_to_cpp')
  if not os.path.exists(tool):
    print('Could not find %s, run ninja -C %s proto_to_cpp' % (tool, out_dir))
  for proto in PROTOS:
    run([tool, proto] + [HEADER_PATH, CPP_PATH, INCLUDE_PATH])
    fname = os.path.basename(proto).replace('.proto', '')
    run(clang_format + [os.path.join(HEADER_PATH, fname + '.h')])
    run(clang_format + [os.path.join(CPP_PATH, fname + '.cc')])


if __name__ == '__main__':
  sys.exit(main())

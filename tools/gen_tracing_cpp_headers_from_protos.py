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
  'protos/tracing_service/trace_config.proto',
  'protos/tracing_service/data_source_config.proto',
  'protos/tracing_service/data_source_descriptor.proto',
)

HEADER_PATH = 'include/perfetto/tracing/core'
CPP_PATH = 'src/tracing/core'


def run(cmd):
  print('\nRunning ' + ' '.join(cmd))
  subprocess.check_call(cmd)


def main():
  if not os.path.exists('.gn'):
    print('This script mast be executed from the perfetto root directory')
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
    run([tool, proto] + [HEADER_PATH, CPP_PATH])
    fname = os.path.basename(proto).replace('.proto', '')
    run(clang_format + [os.path.join(HEADER_PATH, fname + '.h')])
    run(clang_format + [os.path.join(CPP_PATH, fname + '.cc')])


if __name__ == '__main__':
  sys.exit(main())

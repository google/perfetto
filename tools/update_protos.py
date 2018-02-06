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

# Generates proto definitions from ftrace format files.
# Arguments:
#   path to ftrace_proto_gen
#   path to input directory full of format files
#   path to output directory for proto definitions files
#   Either:
#    --whitelist PATH : path to a list of events to generate
#    --event EVENT : a single event to generate
#
# Example:
# ./tools/update_protos.py
#    out/linux/ftrace_proto_gen
#   libftrace/test/data/android_seed_N2F62_3.10.49
#   protos/ftrace
#   --whitelist tools/ftrace_proto_gen/event_whitelist


from __future__ import print_function
import argparse
import os
import subprocess
import sys
import tempfile


HEADER = """# Copyright (C) 2018 The Android Open Source Project
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

"""


def command(*args):
  subprocess.check_call(args, stdout=sys.stdout, stderr=sys.stderr)

def main():
  parser = argparse.ArgumentParser(description='Generate protos.')
  parser.add_argument('ftrace_proto_gen',
                      help='an ftrace_proto_gen binary')
  parser.add_argument('input_dir',
                      help='input directory')
  parser.add_argument('output_dir',
                      help='output directory')
  parser.add_argument('--whitelist', dest='whitelist_path',
                      default=None,
                      help='path to whitelist of events')
  parser.add_argument('--event', dest='event',
                      default=None,
                      help='output directory')
  parser.add_argument('--update-build-files', dest='update_build_files',
                      action='store_true',
                      help='should update metadata')
  args = parser.parse_args()

  gen_path = args.ftrace_proto_gen
  input_dir = args.input_dir
  output_dir = args.output_dir
  whitelist_path = args.whitelist_path
  update_build_files = args.update_build_files

  if whitelist_path is not None and not os.path.isfile(whitelist_path):
    parser.error('Whitelist file {} does not exist.'.format(whitelist_path))
  if not os.path.isdir(input_dir):
    parser.error('Input directory {} does not exist.'.format(input_dir))
  if not os.path.isdir(output_dir):
    parser.error('Output directory {} does not exist.'.format(output_dir))

  # Run ftrace_proto_gen
  command(gen_path, whitelist_path, input_dir, output_dir)

  if update_build_files:
    names = sorted(os.listdir(output_dir))
    names = [name for name in names if name.endswith('.proto')]
    names = ''.join(['  "{}",\n'.format(name) for name in names])
    body = 'ftrace_proto_names = [\n{}]'.format(names)
    with open(os.path.join(output_dir, 'all_protos.gni'), 'wb') as f:
      f.write(HEADER)
      f.write(body)

  return 0


if __name__ == '__main__':
  sys.exit(main())

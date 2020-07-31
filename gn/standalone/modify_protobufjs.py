#!/usr/bin/env python3
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

import os
import re
import sys
import argparse
import fileinput

PERFETTO_ROOT = os.path.dirname(os.path.dirname(__file__))


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      'protobufjs_path', type=str, help='path to prebundled protobufjs file')
  args = parser.parse_args()

  TARGET_LINE_RE = r'^(\s*)(protobuf.util.global.protobuf = protobuf)'

  inplace_file = fileinput.input(args.protobufjs_path, inplace=True)
  for line in inplace_file:
    if re.search(TARGET_LINE_RE, line):
      print("// NOTE: Local modification by %s" %
            os.path.relpath(__file__, PERFETTO_ROOT))
      print("// NOTE: Commenting out this line to prevent the protobuf variable"
            " from leaking into the global scope.")
      sys.stdout.write(re.sub(TARGET_LINE_RE, r'\1// \2', line))
    else:
      sys.stdout.write(line)


if __name__ == '__main__':
  sys.exit(main())

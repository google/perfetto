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

# Usage: ./generate_full_protos.py path/to/input.proto path/to/output.proto
# Copies the input to the output removing 'option optimize_for = LITE_RUNTIME'
# if present.

import argparse
import sys

def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('input')
  parser.add_argument('output')
  args = parser.parse_args()

  with open(args.input, 'r') as fin:
    s = fin.read()

  s = s.replace('option optimize_for = LITE_RUNTIME;\n', '')

  with open(args.output, 'w') as fout:
    fout.write(s)

  return 0

if __name__ == '__main__':
  sys.exit(main())

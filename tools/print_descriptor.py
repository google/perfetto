#!/usr/bin/env python3
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

import argparse
from google.protobuf import descriptor_pb2


# Take a path to file with binary protobuf descriptor as CLI argument and print
# it in textproto format.
#
# Example usage:
#   tools/print_descriptor.py path/to/file.descriptor
def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      'input_file',
      type=str,
      help='File name with binary proto descriptor to print')
  args = parser.parse_args()

  descriptor = descriptor_pb2.FileDescriptorSet()
  with open(args.input_file, 'rb') as f:
    contents = f.read()
    descriptor.MergeFromString(contents)

  print(descriptor)


if __name__ == "__main__":
  main()

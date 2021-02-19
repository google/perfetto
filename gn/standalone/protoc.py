# Copyright (C) 2019 The Android Open Source Project
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
"""Script to wrap protoc execution.

This script exists to work-around the bad depfile generation by protoc when
generating descriptors."""

from __future__ import print_function
import argparse
import os
import sys
import subprocess
import tempfile
import uuid

from codecs import open


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--descriptor_set_out', default=None)
  parser.add_argument('--dependency_out', default=None)
  parser.add_argument('protoc')
  args, remaining = parser.parse_known_args()

  if args.dependency_out and args.descriptor_set_out:
    tmp_path = os.path.join(tempfile.gettempdir(), str(uuid.uuid4()))
    custom = [
        '--descriptor_set_out', args.descriptor_set_out, '--dependency_out',
        tmp_path
    ]
    try:
      cmd = [args.protoc] + custom + remaining
      subprocess.check_call(cmd)
      with open(tmp_path, 'rb') as tmp_rd:
        dependency_data = tmp_rd.read().decode('utf-8')
    finally:
      if os.path.exists(tmp_path):
        os.unlink(tmp_path)

    with open(args.dependency_out, 'w', encoding='utf-8') as f:
      f.write(args.descriptor_set_out + ":")
      f.write(dependency_data)
  else:
    subprocess.check_call(sys.argv[1:])


if __name__ == '__main__':
  sys.exit(main())

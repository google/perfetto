#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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
"""Turns a shared library into a loadable .duckdb_extension.

DuckDB requires a 512 byte metadata footer (plus a WebAssembly-style custom
section header) at the end of every extension binary. This mirrors
duckdb/extension-ci-tools/scripts/append_extension_metadata.py.
"""

import argparse
import shutil


def field(s):
  b = s.encode('ascii')
  assert len(b) <= 32
  return b + b'\x00' * (32 - len(b))


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--library', required=True)
  parser.add_argument('--out', required=True)
  parser.add_argument('--platform', required=True)  # e.g. osx_arm64
  # For --abi-type=CPP this is the exact DuckDB version; for C_STRUCT, the
  # minimum C API version.
  parser.add_argument('--duckdb-version', required=True)  # e.g. v1.5.6
  parser.add_argument('--abi-type', default='CPP', choices=['CPP', 'C_STRUCT'])
  parser.add_argument('--extension-version', default='v0.0.1')
  args = parser.parse_args()

  shutil.copyfile(args.library, args.out)
  with open(args.out, 'ab') as f:
    # Custom section header: id 0, LEB128 size 531, 16-char name, LEB128 512.
    f.write(bytes([0, 147, 4, 16]) + b'duckdb_signature' + bytes([128, 4]))
    f.write(field(''))  # FIELD8 (unused)
    f.write(field(''))  # FIELD7 (unused)
    f.write(field(''))  # FIELD6 (unused)
    f.write(field(args.abi_type))  # FIELD5: ABI type.
    f.write(field(args.extension_version))  # FIELD4
    f.write(field(args.duckdb_version))  # FIELD3
    f.write(field(args.platform))  # FIELD2
    f.write(field('4'))  # FIELD1: magic.
    f.write(b'\x00' * 256)  # Signature (unsigned).


if __name__ == '__main__':
  main()

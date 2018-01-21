#!/usr/bin/env python
# Copyright (C) 2018 The Android Open Source Project
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
import os
import subprocess
import sys


def run(encode_or_decode, protoc_path, proto, root, input, output):
  cmd = [
      protoc_path,
      '--{}=perfetto.protos.{}'.format(encode_or_decode, proto),
      os.path.join(root, 'protos/perfetto/config/trace_config.proto'),
      os.path.join(root, 'protos/perfetto/trace/trace.proto'),
      '--proto_path={}/protos'.format(root),
  ]
  subprocess.check_call(cmd, stdin=input, stdout=output, stderr=sys.stderr)


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      'encode_or_decode',
      choices=['encode', 'decode'],
      help='encode into binary format or decode to text.'
  )
  parser.add_argument('--proto_name', default='TraceConfig',
      help='name of proto to encode/decode (default: TraceConfig).')
  parser.add_argument('--protoc_path', default=None,
      help='path to protoc')
  parser.add_argument('--root', default='.',
      help='root directory (default: "protos")')
  parser.add_argument('--input', default='-',
      help='input file, or "-" for stdin (default: "-")')
  parser.add_argument('--output', default='-',
      help='output file, or "-" for stdout (default: "-")')
  args = parser.parse_args()

  encode_or_decode = args.encode_or_decode
  proto_name = args.proto_name
  protoc_path =  args.protoc_path
  root =  args.root
  input = sys.stdin if args.input == '-' else open(args.input, 'rb')
  output = sys.stdout if args.output == '-' else open(args.output, 'wb')
  if not protoc_path:
    directory = os.path.dirname(__file__)
    protoc_path = os.path.join(directory, 'gcc_like_host', 'protoc')
  run(encode_or_decode, protoc_path, proto_name, root, input, output)
  return 0


if __name__ == '__main__':
  sys.exit(main())

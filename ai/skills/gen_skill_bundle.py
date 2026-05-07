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
"""Pack the SKILL.md files under ai/skills/ into a gzip-compressed blob
embedded as a `constexpr std::array<uint8_t, N>` C++ header.

The blob, after decompression with util::GzipDecompressor::DecompressFully,
is an SqlBundle wire stream:

  uint32_t count;
  for (count) {
    uint32_t path_size;       // excluding the trailing NUL.
    char     path[path_size];
    char     nul;             // 0x00.
    uint32_t content_size;
    char     content[content_size];
  }

The path is the file's location relative to ai/skills/ (e.g.
'perfetto-infra-querying-traces/SKILL.md'); the directory portion is
the skill slug used by `trace_processor ai install-skills` when
writing the file out under the agent's discovery root.

Inputs are positional file paths. As a convenience, any positional that
ends with `.txt` is read as a newline-separated list of further input
paths (the GN driver materialises this list at build time via
`generated_file`).
"""

import argparse
import gzip
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(SCRIPT_DIR, '..', '..')))

# pylint: disable=wrong-import-position
from python.tools import cpp_blob_emitter  # noqa: E402
# pylint: enable=wrong-import-position


def expand_inputs(inputs):
  out = []
  for path in inputs:
    if path.endswith('.txt'):
      with open(path, 'r', encoding='utf-8') as f:
        out.extend(line for line in f.read().splitlines() if line)
    else:
      out.append(path)
  return out


def pack(file_to_bytes):
  """Pack {relpath: content_bytes} into the SqlBundle wire format."""
  buf = bytearray()
  buf += struct.pack('<I', len(file_to_bytes))
  for path, content in file_to_bytes.items():
    path_bytes = path.encode('utf-8')
    buf += struct.pack('<I', len(path_bytes))
    buf += path_bytes
    buf += b'\0'
    buf += struct.pack('<I', len(content))
    buf += content
  return bytes(buf)


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--output', required=True)
  parser.add_argument('--namespace', required=True)
  parser.add_argument('--gen-dir', default='')
  parser.add_argument('--root-dir', default=None)
  parser.add_argument('inputs', nargs='+')
  args = parser.parse_args()

  files = expand_inputs(args.inputs)

  # Soong cannot pass a path to the source root, so without --root-dir we
  # fall back to the longest common path. This relies on every input
  # living under ai/skills/ which the GN driver guarantees.
  root_dir = args.root_dir if args.root_dir else os.path.commonpath(files)

  packed_files = {}
  for path in files:
    with open(path, 'rb') as f:
      relpath = os.path.relpath(path, root_dir)
      assert '..' not in relpath.split(os.sep), relpath
      relpath = relpath.replace('\\', '/')
      packed_files[relpath] = f.read()

  blob = gzip.compress(pack(packed_files), compresslevel=9, mtime=0)
  cpp_blob_emitter.emit_array(
      blob,
      args.output,
      symbol=cpp_blob_emitter.derive_symbol(args.output),
      namespace=args.namespace,
      include_guard=cpp_blob_emitter.derive_include_guard(
          args.output, args.gen_dir))
  return 0


if __name__ == '__main__':
  sys.exit(main())

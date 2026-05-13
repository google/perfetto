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
"""Emit a C++ header containing a constexpr std::array<uint8_t, N> blob.

Importable from other build-time codegen tools, or runnable as a CLI:
  python3 cpp_blob_emitter.py \\
      --input data.bin --output out.h \\
      --namespace foo [--symbol kFoo] [--include-guard FOO_H_] \\
      [--symbol-suffix Descriptor] [--gen-dir path/to/gen] [--compress]

If --symbol is omitted, it is derived from the basename of --output: the
substring before the first '.' is title-cased with underscores stripped, then
--symbol-suffix is appended (e.g. test_messages.descriptor.h with suffix
'Descriptor' produces TestMessagesDescriptor).

If --include-guard is omitted, it is derived from --output: when --gen-dir is
given the path is taken relative to it, and the result has separators/dots
replaced with underscores, is uppercased, and gets a trailing '_'.
"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import os
import sys
import textwrap
import zlib

_HEADER_TEMPLATE = """/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef {include_guard}
#define {include_guard}

#include <stddef.h>
#include <stdint.h>
#include <array>

namespace {namespace} {{

inline constexpr std::array<uint8_t, {size}> k{symbol}{{
{binary}}};

}}  // namespace {namespace}

#endif  // {include_guard}
"""


def _format_byte_literal(data):
  # Match the historical output of gen_cc_proto_descriptor.py exactly so
  # consumers see byte-identical headers after the extraction.
  try:
    ord(data[0])
    ordinal = ord
  except TypeError:
    ordinal = lambda x: x
  binary = '{' + ', '.join('{0:#04x}'.format(ordinal(c)) for c in data) + '}'
  return textwrap.fill(
      binary, width=80, initial_indent='    ', subsequent_indent='     ')


def derive_symbol(output_path, suffix=''):
  """Title-case the part of basename(output_path) before the first '.'.

  e.g. 'test_messages.descriptor.h' with suffix 'Descriptor' →
  'TestMessagesDescriptor'.
  """
  base = os.path.basename(output_path).split('.', 1)[0]
  return base.title().replace('_', '') + suffix


def derive_include_guard(output_path, gen_dir=''):
  """Compute a C++ include guard token from output_path.

  When gen_dir is provided, the output is taken relative to it first. The
  resulting path has '/', '\\\\' and '.' replaced with '_', is uppercased,
  and gets a trailing '_'.
  """
  rel = os.path.relpath(output_path,
                        gen_dir) if gen_dir else os.path.basename(output_path)
  return rel.replace('\\', '_').replace('/', '_').replace('.',
                                                          '_').upper() + '_'


def emit_array(data, output_path, *, symbol, namespace, include_guard):
  """Write `data` to `output_path` as a constexpr std::array<uint8_t, N>."""
  binary = _format_byte_literal(data)
  with open(output_path, 'wb') as f:
    f.write(
        _HEADER_TEMPLATE.format(
            include_guard=include_guard,
            namespace=namespace,
            symbol=symbol,
            size=len(data),
            binary=binary,
        ).encode())


def emit_compressed_array(data,
                          output_path,
                          *,
                          symbol,
                          namespace,
                          include_guard,
                          level=9):
  """Like emit_array, but zlib-compresses `data` first.

  Consumers must inflate the resulting array at runtime. The uncompressed
  size is not encoded — pass `level=zlib.Z_BEST_COMPRESSION` for max ratio
  (the default).
  """
  emit_array(
      zlib.compress(data, level),
      output_path,
      symbol=symbol,
      namespace=namespace,
      include_guard=include_guard)


def _main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--output', required=True, help='Path to .h to emit.')
  parser.add_argument(
      '--namespace', required=True, help='C++ namespace for the array.')
  parser.add_argument(
      '--symbol',
      default=None,
      help='Array symbol name (sans leading "k"). '
      'Default: derived from --output.')
  parser.add_argument(
      '--symbol-suffix',
      default='',
      help='Suffix appended when --symbol is derived from --output.')
  parser.add_argument(
      '--include-guard',
      default=None,
      help='Include guard token. Default: derived from --output.')
  parser.add_argument(
      '--gen-dir',
      default='',
      help='Build gen dir; used to make --output relative when '
      'deriving --include-guard.')
  parser.add_argument(
      '--compress',
      action='store_true',
      help='zlib-compress the bytes before embedding.')
  parser.add_argument('input', help='Path to bytes file.')
  args = parser.parse_args()

  symbol = args.symbol if args.symbol is not None else derive_symbol(
      args.output, args.symbol_suffix)
  include_guard = (
      args.include_guard if args.include_guard is not None else
      derive_include_guard(args.output, args.gen_dir))

  with open(args.input, 'rb') as f:
    data = f.read()
  emit = emit_compressed_array if args.compress else emit_array
  emit(
      data,
      args.output,
      symbol=symbol,
      namespace=args.namespace,
      include_guard=include_guard)
  return 0


if __name__ == '__main__':
  sys.exit(_main())

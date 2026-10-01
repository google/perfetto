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
"""Emit a C++ header containing a blob packed into uint64_t words.

The generated view preserves byte-oriented data(), size(), begin(), and end()
accessors over constant-initialized storage. Each blob has its own view type.
The header selects little- or big-endian uint64_t initializers at preprocessing
time so the underlying bytes are identical on either architecture.
This reduces C++ parsing work without long string literals, runtime conversion,
or static constructors. Encoded blobs instead expose Decode(), which returns
an owning byte buffer and releases its allocation when the caller is done.

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
import re
import sys
import struct
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

#include "perfetto/base/compiler.h"
{decoder_include}
namespace {namespace} {{

inline constexpr std::array<uint64_t, {word_count}> k{symbol}Words{{{{
#if PERFETTO_IS_LITTLE_ENDIAN()
{little_binary}
#else
{big_binary}
#endif
}}}};
struct {symbol}View {{
  const uint64_t* words;

{accessors}
}};
inline constexpr {symbol}View k{symbol}{{k{symbol}Words.data()}};

}}  // namespace {namespace}

#endif  // {include_guard}
"""

_VIEW_ACCESSORS = """  const uint8_t* data() const {{
    return reinterpret_cast<const uint8_t*>(words);
  }}
  constexpr size_t size() const {{ return {size}; }}
  const uint8_t* begin() const {{ return data(); }}
  const uint8_t* end() const {{ return data() + size(); }}"""

_DECODE_ACCESSORS = """  auto Decode() const {{
    return ::perfetto::base::DecodedBlob::Decode(
        reinterpret_cast<const uint8_t*>(words), {size});
  }}"""


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
  return re.sub(r'[^A-Z0-9_]', '_', rel.upper()) + '_'


def emit_array(data,
               output_path,
               *,
               symbol,
               namespace,
               include_guard,
               decoder=False):
  """Write a header with native-endian words and a byte-view accessor."""
  # Padding is storage, not payload. Keep an addressable word for empty blobs.
  padded = data + b'\0' * (-len(data) % 8) if data else b'\0' * 8

  def words(order):
    return '\n'.join(f'    0x{word:016x}ULL,'
                     for (word,) in struct.iter_unpack(order + 'Q', padded))

  with open(output_path, 'w') as f:
    f.write(
        _HEADER_TEMPLATE.format(
            include_guard=include_guard,
            namespace=namespace,
            symbol=symbol,
            size=len(data),
            word_count=len(padded) // 8,
            little_binary=words('<'),
            big_binary=words('>'),
            decoder_include=('#include "src/base/embedded_blob.h"\n'
                             if decoder else ''),
            accessors=(_DECODE_ACCESSORS
                       if decoder else _VIEW_ACCESSORS).format(size=len(data))))


def emit_encoded_array(data,
                       output_path,
                       *,
                       symbol,
                       namespace,
                       include_guard,
                       compression='zlib',
                       decoder=False):
  """Emit a self-describing blob that the generic embedded-blob decoder reads."""
  # Keep codec IDs in sync with DecodedBlob::Decode: none=0, zlib=1, zstd=2.
  assert compression in ('none', 'zlib')
  codec = 1 if compression == 'zlib' else 0
  payload = zlib.compress(data, level=9) if codec else data
  encoded = struct.pack('<BI', codec, len(data)) + payload
  emit_array(
      encoded,
      output_path,
      symbol=symbol,
      namespace=namespace,
      include_guard=include_guard,
      decoder=decoder)


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
  parser.add_argument(
      '--encoded',
      action='store_true',
      help='Emit a codec/size envelope and an owning Decode() accessor.')
  parser.add_argument('input', help='Path to bytes file.')
  args = parser.parse_args()

  symbol = args.symbol if args.symbol is not None else derive_symbol(
      args.output, args.symbol_suffix)
  include_guard = (
      args.include_guard if args.include_guard is not None else
      derive_include_guard(args.output, args.gen_dir))

  with open(args.input, 'rb') as f:
    data = f.read()
  if args.encoded:
    emit_encoded_array(
        data,
        args.output,
        symbol=symbol,
        namespace=args.namespace,
        include_guard=include_guard,
        decoder=True,
        compression='zlib' if args.compress else 'none')
    return 0
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

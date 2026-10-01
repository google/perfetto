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

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
import zlib

from python.tools import cpp_blob_emitter

ROOT_DIR = Path(__file__).resolve().parents[2]


class TestCppBlobEmitter(unittest.TestCase):

  def test_compiled_bytes(self):
    """Exercise the generated C++ accessors, padding, and both endian paths."""
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    if not shutil.which(compiler[0]):
      self.skipTest('A C++ compiler is required to test generated headers')

    # Every tail length, empty input, embedded NULs, high bits, and whole words.
    payloads = [bytes(range(n)) for n in range(18)]
    payloads += [bytes(range(256)), b'\xff' * 17, b'abc\0\0']
    # Larger than the string-literal limits this representation must avoid.
    payloads.append(bytes(range(256)) * 257)
    compressed_input = bytes(range(256)) * 4
    payloads.append(zlib.compress(compressed_input, 9))

    with tempfile.TemporaryDirectory() as tmp:
      tmp = Path(tmp)
      includes = []
      checks = []
      for i, payload in enumerate(payloads):
        header = tmp / f'blob_{i}.h'
        args = dict(
            symbol=f'Blob{i}',
            namespace='test',
            include_guard=f'TEST_BLOB_{i}_H_')
        if i == len(payloads) - 1:
          cpp_blob_emitter.emit_compressed_array(compressed_input, header,
                                                 **args)
        else:
          cpp_blob_emitter.emit_array(payload, header, **args)
        includes.append(f'#include "{header.name}"')
        checks.append(
            f'static_assert(test::kBlob{i}.size() == {len(payload)});\n'
            f'Emit(test::kBlob{i});')

      for force_big_endian in [False, True]:
        with self.subTest(force_big_endian=force_big_endian):
          # Test the big-endian initializer path even on a little-endian host.
          # Emit the simulated target's bytes using shifts instead of reading
          # this machine's object representation in that case.
          setup = ''
          emit = '''for (uint8_t byte : blob)
    std::putchar(byte);'''
          if force_big_endian:
            setup = '''
#include "perfetto/base/compiler.h"
#undef PERFETTO_IS_LITTLE_ENDIAN
#define PERFETTO_IS_LITTLE_ENDIAN() 0
'''
            emit = '''for (size_t i = 0; i < blob.size(); ++i)
    std::putchar(static_cast<unsigned char>(
        blob.words[i / 8] >> ((7 - i % 8) * 8)));'''
          source = tmp / 'test.cc'
          source.write_text(setup + '\n'.join(includes) + f'''
#include <cstdio>
#include <cassert>
#ifdef PERFETTO_INTERNAL_BLOB_WORD
#error Generated headers must not leak the word conversion macro.
#endif
static_assert(perfetto::base::ByteSwap64(0x0123456789abcdefULL) ==
              0xefcdab8967452301ULL);
static_assert(perfetto::base::ByteSwap64(0) == 0);
static_assert(perfetto::base::ByteSwap64(~uint64_t{{0}}) == ~uint64_t{{0}});
template <typename T>
void Emit(const T& blob) {{
  assert(blob.begin() == blob.data());
  assert(blob.end() == (blob.size() ? blob.data() + blob.size() : blob.data()));
  {emit}
}}
int main() {{
{''.join(checks)}
}}
''')
          executable = tmp / 'test'
          subprocess.run(
              compiler + [
                  '-std=c++17', '-Wall', '-Wextra', '-Werror',
                  '-pedantic-errors', '-I' + str(ROOT_DIR / 'include'), '-I' +
                  str(ROOT_DIR / 'include/perfetto/base/build_configs/bazel'),
                  str(source), '-o',
                  str(executable)
              ],
              check=True,
              capture_output=True)
          self.assertEqual(
              subprocess.check_output([str(executable)]), b''.join(payloads))


if __name__ == '__main__':
  unittest.main()

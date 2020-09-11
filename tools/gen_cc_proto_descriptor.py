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

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
import os
import sys
import argparse
import tempfile
import subprocess
import textwrap


def write_cpp_header(gendir, target, descriptor_bytes):
  _, target_name = os.path.split(target)

  proto_name = target_name[:-len('.descriptor.h')].title().replace("_", "")
  try:
    ord(descriptor_bytes[0])
    ordinal = ord
  except TypeError:
    ordinal = lambda x: x
  binary = '{' + ', '.join(
      '{0:#04x}'.format(ordinal(c)) for c in descriptor_bytes) + '}'
  binary = textwrap.fill(
      binary, width=80, initial_indent='    ', subsequent_indent='     ')

  relative_target = os.path.relpath(target, gendir)
  include_guard = relative_target.replace('/', '_').replace('.',
                                                            '_').upper() + '_'

  with open(target, 'wb') as f:
    f.write("""/*
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

namespace perfetto {{

constexpr std::array<uint8_t, {size}> k{proto_name}Descriptor{{
{binary}}};

}}  // namespace perfetto

#endif  // {include_guard}
""".format(
        proto_name=proto_name,
        size=len(descriptor_bytes),
        binary=binary,
        include_guard=include_guard,
    ).encode())


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--cpp_out', required=True)
  parser.add_argument('--gen_dir', default='')
  parser.add_argument('descriptor')
  args = parser.parse_args()

  with open(args.descriptor, 'rb') as fdescriptor:
    s = fdescriptor.read()
    write_cpp_header(args.gen_dir, args.cpp_out, s)

  return 0


if __name__ == '__main__':
  sys.exit(main())

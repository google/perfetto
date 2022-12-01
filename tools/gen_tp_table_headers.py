#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
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
from dataclasses import dataclass
import os
import runpy
import sys
from typing import List

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.trace_processor_table.public import Table  #pylint: disable=wrong-import-position)


@dataclass
class GeneratorArg:
  """Represents a Python module to be converted to a header."""
  in_path: str
  out_path: str
  tables: List[Table]


def main():
  """Main function."""
  parser = argparse.ArgumentParser()
  parser.add_argument('--gen-dir', required=True)
  parser.add_argument('--inputs', required=True, nargs='*')
  parser.add_argument('--outputs', required=True, nargs='*')
  args = parser.parse_args()

  if len(args.inputs) != len(args.outputs):
    raise Exception('Number of inputs must match number of outputs')

  gen_args = []
  for (in_path, out_path) in zip(args.inputs, args.outputs):
    tables = runpy.run_path(in_path)['ALL_TABLES']
    gen_args.append(GeneratorArg(in_path, out_path, tables))

  for arg in gen_args:
    # TODO(lalitm): fill this header with useful content.
    with open(arg.out_path, 'w', encoding='utf8') as out:
      out.write('\n')


if __name__ == '__main__':
  sys.exit(main())

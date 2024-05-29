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

import argparse
import os
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(ROOT_DIR)

from python.generators.diff_tests.utils import serialize_textproto_trace, serialize_python_trace


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '--out', type=str, help='out directory to search for trace descriptor')
  parser.add_argument(
      '--descriptor', type=str, help='path to the trace descriptor')
  parser.add_argument('trace_path', type=str, help='path of trace to serialize')
  args = parser.parse_args()

  if args.out and not args.descriptor:
    trace_protos_path = os.path.join(args.out, 'gen', 'protos', 'perfetto',
                                     'trace')
    chrome_extension_descriptor_path = os.path.join(
        args.out, 'gen', 'protos', 'third_party', 'chromium',
        'chrome_track_event.descriptor')
    trace_descriptor_path = os.path.join(trace_protos_path, 'trace.descriptor')
    test_extensions_descriptor_path = os.path.join(
        trace_protos_path, 'test_extensions.descriptor')
    winscope_extensions_descriptor_path = os.path.join(trace_protos_path,
                                                       'android',
                                                       'winscope.descriptor')
    extension_descriptors = [
        chrome_extension_descriptor_path, test_extensions_descriptor_path,
        winscope_extensions_descriptor_path
    ]
  elif args.descriptor and not args.out:
    trace_descriptor_path = args.descriptor
    extension_descriptors = []
  else:
    raise RuntimeError(
        'Exactly one of --out and --descriptor should be provided')

  trace_path = args.trace_path

  if trace_path.endswith('.py'):
    serialize_python_trace(ROOT_DIR, trace_descriptor_path, trace_path,
                           sys.stdout.buffer)
  elif trace_path.endswith('.textproto'):
    serialize_textproto_trace(trace_descriptor_path, extension_descriptors,
                              trace_path, sys.stdout.buffer)
  else:
    raise RuntimeError('Invalid extension for unserialized trace file')

  return 0


if __name__ == '__main__':
  sys.exit(main())

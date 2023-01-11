#!/usr/bin/env python3
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

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import datetime
import json
import os
import re
import signal
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.diff_tests.testing import DiffTest
from python.generators.diff_tests.utils import red, green, end_color
from python.generators.diff_tests.utils import ctrl_c_handler, find_trace_descriptor
from python.generators.diff_tests.runner import run_all_tests, read_all_tests


def main():
  signal.signal(signal.SIGINT, ctrl_c_handler)
  parser = argparse.ArgumentParser()
  parser.add_argument('--test-type', type=str, default='all')
  parser.add_argument('--trace-descriptor', type=str)
  parser.add_argument('--metrics-descriptor', type=str)
  parser.add_argument('--perf-file', type=str)
  parser.add_argument(
      '--query-metric-filter',
      default='.*',
      type=str,
      help='Filter the name of query files or metrics to test (regex syntax)')
  parser.add_argument(
      '--trace-filter',
      default='.*',
      type=str,
      help='Filter the name of trace files to test (regex syntax)')
  parser.add_argument(
      '--keep-input',
      action='store_true',
      help='Save the (generated) input pb file for debugging')
  parser.add_argument(
      '--rebase',
      action='store_true',
      help='Update the expected output file with the actual result')
  parser.add_argument(
      '--no-colors', action='store_true', help='Print without coloring')
  parser.add_argument(
      'trace_processor', type=str, help='location of trace processor binary')
  args = parser.parse_args()

  query_metric_pattern = re.compile(args.query_metric_filter)
  trace_pattern = re.compile(args.trace_filter)

  tests = read_all_tests(query_metric_pattern, trace_pattern)
  sys.stderr.write(f"[==========] Running {len(tests)} tests.\n")

  out_path = os.path.dirname(args.trace_processor)
  if args.trace_descriptor:
    trace_descriptor_path = args.trace_descriptor
  else:

    trace_descriptor_path = find_trace_descriptor(out_path)
    if not os.path.exists(trace_descriptor_path):
      trace_descriptor_path = find_trace_descriptor(
          os.path.join(out_path, 'gcc_like_host'))

  chrome_extensions = os.path.join(out_path, 'gen', 'protos', 'third_party',
                                   'chromium', 'chrome_track_event.descriptor')
  test_extensions = os.path.join(out_path, 'gen', 'protos', 'perfetto', 'trace',
                                 'test_extensions.descriptor')

  test_run_start = datetime.datetime.now()
  test_failures, perf_data, rebased = run_all_tests(
      trace_descriptor_path, [chrome_extensions, test_extensions], args, tests)
  test_run_end = datetime.datetime.now()
  test_time_ms = int((test_run_end - test_run_start).total_seconds() * 1000)

  sys.stderr.write(
      f"[==========] {len(tests)} tests ran. ({test_time_ms} ms total)\n")
  sys.stderr.write(
      f"{green(args.no_colors)}[  PASSED  ]{end_color(args.no_colors)} "
      f"{len(tests) - len(test_failures)} tests.\n")
  if len(test_failures) > 0:
    sys.stderr.write(
        f"{red(args.no_colors)}[  FAILED  ]{end_color(args.no_colors)} "
        f"{len(test_failures)} tests.\n")
    for failure in test_failures:
      sys.stderr.write(
          f"{red(args.no_colors)}[  FAILED  ]{end_color(args.no_colors)} "
          f"{failure}\n")

  if args.rebase:
    sys.stderr.write('\n')
    sys.stderr.write(f"{rebased} tests rebased.\n")
    for name in rebased:
      sys.stderr.write(f"[  REBASED  ] {name}\n")

  if len(test_failures) > 0:
    return 1

  if args.perf_file:
    test_dir = os.path.join(ROOT_DIR, 'test')
    trace_processor_dir = os.path.join(test_dir, 'trace_processor')

    metrics = []
    sorted_data = sorted(
        perf_data,
        key=lambda x: (x.test_type.name, x.trace_path, x.query_path_or_metric))
    for perf_args in sorted_data:
      trace_short_path = os.path.relpath(perf_args.trace_path, test_dir)

      query_short_path_or_metric = perf_args.query_path_or_metric
      if perf_args.test_type == DiffTest.TestType.QUERY:
        query_short_path_or_metric = os.path.relpath(
            perf_args.query_path_or_metric, trace_processor_dir)

      metrics.append({
          'metric': 'tp_perf_test_ingest_time',
          'value': float(perf_args.ingest_time_ns) / 1.0e9,
          'unit': 's',
          'tags': {
              'test_name': f"{trace_short_path}-{query_short_path_or_metric}",
              'test_type': perf_args.test_type.name,
          },
          'labels': {},
      })
      metrics.append({
          'metric': 'perf_test_real_time',
          'value': float(perf_args.real_time_ns) / 1.0e9,
          'unit': 's',
          'tags': {
              'test_name': f"{trace_short_path}-{query_short_path_or_metric}",
              'test_type': perf_args.test_type.name,
          },
          'labels': {},
      })

    output_data = {'metrics': metrics}
    with open(args.perf_file, 'w+') as perf_file:
      perf_file.write(json.dumps(output_data, indent=2))
  return 0


if __name__ == '__main__':
  sys.exit(main())

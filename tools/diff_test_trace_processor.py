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

from python.generators.diff_tests.testing import TestType
from python.generators.diff_tests.utils import ctrl_c_handler
from python.generators.diff_tests.runner import DiffTestSuiteRunner


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

  test_runner = DiffTestSuiteRunner(args.query_metric_filter, args.trace_filter,
                                    args.trace_processor, args.trace_descriptor,
                                    args.no_colors)
  sys.stderr.write(f"[==========] Running {len(test_runner.tests)} tests.\n")

  results = test_runner.run_all_tests(args.metrics_descriptor, args.keep_input,
                                      args.rebase)
  sys.stderr.write(results.str(args.no_colors, len(test_runner.tests)))

  if args.rebase:
    sys.stderr.write(results.rebase_str())

  if len(results.test_failures) > 0:
    return 1

  if args.perf_file:
    test_dir = os.path.join(ROOT_DIR, 'test')
    trace_processor_dir = os.path.join(test_dir, 'trace_processor')

    metrics = []
    sorted_data = sorted(
        results.perf_data,
        key=lambda x: (x.test_type.name, x.trace_path, x.query_path_or_metric))
    for perf_args in sorted_data:
      trace_short_path = os.path.relpath(perf_args.trace_path, test_dir)

      query_short_path_or_metric = perf_args.query_path_or_metric
      if perf_args.test_type == TestType.QUERY:
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

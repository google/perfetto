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
import json
import os
import signal
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.diff_tests.utils import ctrl_c_handler
from python.generators.diff_tests.runner import DiffTestsRunner
from python.generators.diff_tests.models import Config


def main():
  signal.signal(signal.SIGINT, ctrl_c_handler)
  parser = argparse.ArgumentParser()
  parser.add_argument('--test-type', type=str, default='all')
  parser.add_argument('--trace-descriptor', type=str)
  parser.add_argument('--summary-descriptor', type=str)
  parser.add_argument('--metrics-descriptor', nargs='+', type=str)
  parser.add_argument('--chrome-track-event-descriptor', type=str, default=None)
  parser.add_argument('--test-extensions', type=str, default=None)
  parser.add_argument('--winscope-extensions', type=str, default=None)
  parser.add_argument('--simpleperf-descriptor', type=str, default=None)
  parser.add_argument('--perf-file', type=str)
  parser.add_argument(
      '--override-sql-package', type=str, action='append', default=[])
  parser.add_argument('--test-dir', type=str, default=ROOT_DIR)
  parser.add_argument(
      '--name-filter',
      default='.*',
      type=str,
      help='Filter the name of the tests to run (regex syntax)')
  parser.add_argument(
      '--keep-input',
      action='store_true',
      help='Save the (generated) input pb file for debugging')
  parser.add_argument(
      '--quiet', action='store_true', help='Only print if the test failed.')
  parser.add_argument(
      '--no-colors', action='store_true', help='Print without coloring')
  parser.add_argument(
      '--print-slowest-tests',
      action='store_true',
      help='Print the slowest tests')
  parser.add_argument(
      'trace_processor', type=str, help='location of trace processor binary')
  args = parser.parse_args()

  out_path = os.path.dirname(args.trace_processor)
  protos_path = os.path.join(out_path, 'gen', 'protos')
  if args.chrome_track_event_descriptor is None:
    args.chrome_track_event_descriptor = os.path.join(
        protos_path, 'third_party', 'chromium', 'chrome_track_event.descriptor')
  if args.test_extensions is None:
    args.test_extensions = os.path.join(protos_path, 'perfetto', 'trace',
                                        'test_extensions.descriptor')
  if args.winscope_extensions is None:
    args.winscope_extensions = os.path.join(protos_path, 'perfetto', 'trace',
                                            'android', 'winscope.descriptor')
  if args.simpleperf_descriptor is None:
    args.simpleperf_descriptor = os.path.join(protos_path, 'third_party',
                                              'simpleperf',
                                              'simpleperf.descriptor')
  if args.summary_descriptor is None:
    args.summary_descriptor = os.path.join(protos_path, 'perfetto',
                                           'trace_summary',
                                           'trace_summary.descriptor')

  config = Config(
      name_filter=args.name_filter,
      trace_processor_path=args.trace_processor,
      trace_descriptor=args.trace_descriptor,
      no_colors=args.no_colors,
      override_sql_package_paths=args.override_sql_package,
      test_dir=args.test_dir,
      quiet=args.quiet,
      summary_descriptor=args.summary_descriptor,
      metrics_descriptor_paths=args.metrics_descriptor,
      chrome_extensions=args.chrome_track_event_descriptor,
      test_extensions=args.test_extensions,
      winscope_extensions=args.winscope_extensions,
      simpleperf_descriptor=args.simpleperf_descriptor,
      keep_input=args.keep_input,
      print_slowest_tests=args.print_slowest_tests)
  test_runner = DiffTestsRunner(config)
  results = test_runner.run()
  sys.stderr.write(results.str(args.no_colors))

  if args.print_slowest_tests:
    sys.stderr.write('\n--- Slowest tests ---\n')
    slowest_total = sorted(
        results.perf_data,
        key=lambda p: p.ingest_time_ns + p.real_time_ns,
        reverse=True)[:5]
    slowest_query = sorted(
        results.perf_data, key=lambda p: p.real_time_ns, reverse=True)[:5]
    slowest_ingest = sorted(
        results.perf_data, key=lambda p: p.ingest_time_ns, reverse=True)[:5]

    sys.stderr.write('Top 5 by total time:\n')
    for p in slowest_total:
      total_ms = (p.ingest_time_ns + p.real_time_ns) / 1000000
      sys.stderr.write(f'  {p.test.name}: {total_ms:.2f}ms\n')

    sys.stderr.write('Top 5 by query time:\n')
    for p in slowest_query:
      query_ms = p.real_time_ns / 1000000
      sys.stderr.write(f'  {p.test.name}: {query_ms:.2f}ms\n')

    sys.stderr.write('Top 5 by ingest time:\n')
    for p in slowest_ingest:
      ingest_ms = p.ingest_time_ns / 1000000
      sys.stderr.write(f'  {p.test.name}: {ingest_ms:.2f}ms\n')

  if len(results.test_failures) > 0:
    return 1

  if args.perf_file:
    metrics = []
    sorted_data = sorted(
        results.perf_data, key=lambda x: (x.test.type.name, x.test.name))
    for perf_args in sorted_data:
      metrics.append({
          'metric': 'tp_perf_test_ingest_time',
          'value': float(perf_args.ingest_time_ns) / 1.0e9,
          'unit': 's',
          'tags': {
              'test_name': perf_args.test.name,
              'test_type': perf_args.test.type.name,
          },
          'labels': {},
      })
      metrics.append({
          'metric': 'perf_test_real_time',
          'value': float(perf_args.real_time_ns) / 1.0e9,
          'unit': 's',
          'tags': {
              'test_name': perf_args.test.name,
              'test_type': perf_args.test.type.name,
          },
          'labels': {},
      })

    output_data = {'metrics': metrics}
    with open(args.perf_file, 'w+') as perf_file:
      perf_file.write(json.dumps(output_data, indent=2))
  return 0


if __name__ == '__main__':
  sys.exit(main())

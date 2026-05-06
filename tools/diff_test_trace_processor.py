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
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

# Check that the python venv is active by trying to import protobuf.
try:
  import google.protobuf  # noqa: F401
except ImportError:
  sys.stderr.write('''
Error: google.protobuf module not found.

The Perfetto python virtual environment is not active. To set it up, run:

  tools/install-build-deps

Then activate it with:

  source .venv/bin/activate  (Linux/macOS)
  .venv\\Scripts\\activate   (Windows)

Alternatively, run this script using the venv python directly:

  .venv/bin/python3 tools/diff_test_trace_processor.py ...

''')
  sys.exit(1)

from python.generators.diff_tests.utils import setup_ctrl_c_handler
from python.generators.diff_tests.runner import DiffTestsRunner
from python.generators.diff_tests.models import Config


def main():
  setup_ctrl_c_handler()
  parser = argparse.ArgumentParser()
  parser.add_argument('--test-type', type=str, default='all')
  parser.add_argument('--trace-descriptor', type=str)
  parser.add_argument('--summary-descriptor', type=str)
  parser.add_argument('--metrics-descriptor', nargs='+', type=str)
  parser.add_argument('--chrome-track-event-descriptor', type=str, default=None)
  parser.add_argument('--test-extensions', type=str, default=None)
  parser.add_argument('--winscope-extensions', type=str, default=None)
  parser.add_argument('--gpu-extensions', type=str, default=None)
  parser.add_argument('--gpu-interned-data-extensions', type=str, default=None)
  parser.add_argument('--v8-profile-extensions', type=str, default=None)
  parser.add_argument('--simpleperf-descriptor', type=str, default=None)
  parser.add_argument('--perf-file', type=str)
  parser.add_argument(
      '--compare-perf',
      type=str,
      help='Compare current performance against a saved performance JSON file')

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
      '-j',
      '--jobs',
      type=int,
      default=0,
      help='Number of parallel jobs (default: 0 = use all CPUs)')
  parser.add_argument(
      'trace_processor', type=str, help='location of trace processor binary')
  args = parser.parse_args()

  baseline = {}
  if args.compare_perf:
    if not os.path.exists(args.compare_perf):
      sys.stderr.write(f'Error: Baseline file {args.compare_perf} not found\n')
      return 1

    with open(args.compare_perf, 'r') as f:
      try:
        baseline_data = json.load(f)
      except json.JSONDecodeError:
        sys.stderr.write(
            f'Error: Failed to parse baseline file {args.compare_perf}\n')
        return 1

    for m in baseline_data.get('metrics', []):
      tags = m.get('tags', {})
      test_name = tags.get('test_name')
      if not test_name:
        continue
      metric = m.get('metric')
      val = m.get('value')
      if test_name not in baseline:
        baseline[test_name] = {}
      if metric == 'tp_perf_test_ingest_time':
        baseline[test_name]['ingest'] = val
      elif metric == 'perf_test_real_time':
        baseline[test_name]['query'] = val

  out_path = os.path.dirname(args.trace_processor)
  protos_path = os.path.join(out_path, 'gen', 'protos')
  if args.chrome_track_event_descriptor is None:
    args.chrome_track_event_descriptor = os.path.join(
        protos_path, 'third_party', 'chromium', 'chrome_track_event.descriptor')
  if args.test_extensions is None:
    args.test_extensions = os.path.join(protos_path, 'perfetto', 'trace',
                                        'test_extensions.descriptor')
  if args.winscope_extensions is None:
    args.winscope_extensions = os.path.join(protos_path, 'third_party',
                                            'android',
                                            'android_extension.descriptor')
  if args.gpu_extensions is None:
    args.gpu_extensions = os.path.join(protos_path, 'perfetto', 'trace', 'gpu',
                                       'gpu_track_event.descriptor')
  if args.gpu_interned_data_extensions is None:
    args.gpu_interned_data_extensions = os.path.join(
        protos_path, 'perfetto', 'trace', 'gpu', 'gpu_interned_data.descriptor')
  if args.v8_profile_extensions is None:
    args.v8_profile_extensions = os.path.join(
        protos_path, 'perfetto', 'trace', 'v8',
        'v8_profile_extensions.descriptor')
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
      gpu_extensions=args.gpu_extensions,
      gpu_interned_data_extensions=args.gpu_interned_data_extensions,
      v8_profile_extensions=args.v8_profile_extensions,
      simpleperf_descriptor=args.simpleperf_descriptor,
      keep_input=args.keep_input,
      print_slowest_tests=args.print_slowest_tests,
      jobs=args.jobs)
  test_runner = DiffTestsRunner(config)
  results = test_runner.run()
  sys.stderr.write(results.str(args.no_colors))

  if args.compare_perf:
    sys.stderr.write('\n--- Performance Comparison ---\n\n')

    sys.stderr.write(
        f'{"Test Name":<60} | {"Ingest Diff":<16} | {"Query Diff":<16} | {"Total Diff":<16}\n'
    )
    sys.stderr.write('-' * 117 + '\n')

    improved_cnt = 0
    regressed_cnt = 0

    CLR_RED = '' if args.no_colors else '\033[91m'
    CLR_GRN = '' if args.no_colors else '\033[92m'
    CLR_RST = '' if args.no_colors else '\033[0m'
    CLR_BLD = '' if args.no_colors else '\033[1m'

    import re
    ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')

    for curr in results.perf_data:
      name = curr.test.name
      if name not in baseline:
        continue

      curr_ingest = float(curr.ingest_time_ns) / 1e9
      curr_query = float(curr.real_time_ns) / 1e9
      curr_total = curr_ingest + curr_query

      base_ingest = baseline[name].get('ingest', 0.0)
      base_query = baseline[name].get('query', 0.0)
      base_total = base_ingest + base_query

      def get_diff_str(curr_val, base_val):
        if base_val == 0:
          return "N/A"
        diff_pct = (curr_val - base_val) / base_val * 100
        diff_val = curr_val - base_val
        val_str = f"{diff_pct:+.1f}% ({diff_val:+.3f}s)"
        if diff_pct < -5:
          return f"{CLR_GRN}{val_str}{CLR_RST}"
        elif diff_pct > 5:
          return f"{CLR_RED}{val_str}{CLR_RST}"
        return val_str

      ingest_diff_str = get_diff_str(curr_ingest, base_ingest)
      query_diff_str = get_diff_str(curr_query, base_query)
      total_diff_str = get_diff_str(curr_total, base_total)

      def pad_ansi(s, width, align='left'):
        raw_len = len(ansi_escape.sub('', s))
        padding = max(0, width - raw_len)
        if align == 'left':
          return s + ' ' * padding
        else:
          return ' ' * padding + s

      ingest_padded = pad_ansi(ingest_diff_str, 16, 'right')
      query_padded = pad_ansi(query_diff_str, 16, 'right')
      total_padded = pad_ansi(total_diff_str, 16, 'right')

      sys.stderr.write(
          f'{name:<60} | {ingest_padded} | {query_padded} | {total_padded}\n')

      if base_total > 0:
        pct = (curr_total - base_total) / base_total * 100
        if pct < -5:
          improved_cnt += 1
        elif pct > 5:
          regressed_cnt += 1

    sys.stderr.write(f'\nCompared {len(results.perf_data)} tests.\n')
    sys.stderr.write(
        f'{CLR_BLD}Improved (>5% faster):{CLR_RST} {CLR_GRN}{improved_cnt}{CLR_RST}\n'
    )
    sys.stderr.write(
        f'{CLR_BLD}Regressed (>5% slower):{CLR_RST} {CLR_RED}{regressed_cnt}{CLR_RST}\n'
    )

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

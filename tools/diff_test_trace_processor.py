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
import difflib
import glob
import importlib
import json
import os
import re
import subprocess
import sys
import tempfile

from itertools import chain
from google.protobuf import reflection, text_format

from proto_utils import create_message_factory, serialize_textproto_trace, serialize_python_trace

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Test(object):

  def __init__(self, type, trace_path, query_path_or_metric, expected_path):
    self.type = type
    self.trace_path = trace_path
    self.query_path_or_metric = query_path_or_metric
    self.expected_path = expected_path


class PerfResult(object):

  def __init__(self, test_type, trace_path, query_path_or_metric,
               ingest_time_ns_str, real_time_ns_str):
    self.test_type = test_type
    self.trace_path = trace_path
    self.query_path_or_metric = query_path_or_metric
    self.ingest_time_ns = int(ingest_time_ns_str)
    self.real_time_ns = int(real_time_ns_str)


class TestResult(object):

  def __init__(self, test_type, input_name, trace, cmd, expected, actual,
               stderr, exit_code):
    self.test_type = test_type
    self.input_name = input_name
    self.trace = trace
    self.cmd = cmd
    self.expected = expected
    self.actual = actual
    self.stderr = stderr
    self.exit_code = exit_code


def create_metrics_message_factory(metrics_descriptor_path):
  return create_message_factory(metrics_descriptor_path,
                                'perfetto.protos.TraceMetrics')


def write_diff(expected, actual):
  expected_lines = expected.splitlines(True)
  actual_lines = actual.splitlines(True)
  diff = difflib.unified_diff(
      expected_lines, actual_lines, fromfile='expected', tofile='actual')
  for line in diff:
    sys.stderr.write(line)


def run_metrics_test(trace_processor_path, gen_trace_path, metric,
                     expected_path, perf_path, metrics_message_factory):
  with open(expected_path, 'r') as expected_file:
    expected = expected_file.read()

  json_output = os.path.basename(expected_path).endswith('.json.out')
  cmd = [
      trace_processor_path,
      '--run-metrics',
      metric,
      '--metrics-output=%s' % ('json' if json_output else 'binary'),
      gen_trace_path,
      '--perf-file',
      perf_path,
  ]
  tp = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
  (stdout, stderr) = tp.communicate()

  if json_output:
    expected_text = expected
    actual_text = stdout.decode('utf8')
  else:
    # Expected will be in text proto format and we'll need to parse it to a real
    # proto.
    expected_message = metrics_message_factory()
    text_format.Merge(expected, expected_message)

    # Actual will be the raw bytes of the proto and we'll need to parse it into
    # a message.
    actual_message = metrics_message_factory()
    actual_message.ParseFromString(stdout)

    # Convert both back to text format.
    expected_text = text_format.MessageToString(expected_message)
    actual_text = text_format.MessageToString(actual_message)

  return TestResult('metric', metric, gen_trace_path, cmd, expected_text,
                    actual_text, stderr.decode('utf8'), tp.returncode)


def run_query_test(trace_processor_path, gen_trace_path, query_path,
                   expected_path, perf_path):
  with open(expected_path, 'r') as expected_file:
    expected = expected_file.read()

  cmd = [
      trace_processor_path,
      '-q',
      query_path,
      gen_trace_path,
      '--perf-file',
      perf_path,
  ]

  tp = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
  (stdout, stderr) = tp.communicate()
  return TestResult('query', query_path, gen_trace_path, cmd, expected,
                    stdout.decode('utf8'), stderr.decode('utf8'), tp.returncode)


def run_all_tests(trace_processor, trace_descriptor_path,
                  metrics_message_factory, tests, keep_input):
  perf_data = []
  test_failure = 0
  for test in tests:
    trace_path = test.trace_path
    expected_path = test.expected_path
    if not os.path.exists(trace_path):
      sys.stderr.write('Trace file not found {}\n'.format(trace_path))
      test_failure += 1
      continue
    elif not os.path.exists(expected_path):
      sys.stderr.write('Expected file not found {}\n'.format(expected_path))
      test_failure += 1
      continue

    is_generated_trace = trace_path.endswith('.py') or trace_path.endswith(
        '.textproto')
    if trace_path.endswith('.py'):
      gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
      serialize_python_trace(trace_descriptor_path, trace_path, gen_trace_file)
      gen_trace_path = os.path.realpath(gen_trace_file.name)
    elif trace_path.endswith('.textproto'):
      gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
      serialize_textproto_trace(trace_descriptor_path, trace_path,
                                gen_trace_file)
      gen_trace_path = os.path.realpath(gen_trace_file.name)
    else:
      gen_trace_file = None
      gen_trace_path = trace_path

    with tempfile.NamedTemporaryFile() as tmp_perf_file:
      sys.stderr.write('[ RUN      ] {} {}\n'.format(
          os.path.basename(test.query_path_or_metric),
          os.path.basename(trace_path)))

      tmp_perf_path = tmp_perf_file.name
      if test.type == 'queries':
        query_path = test.query_path_or_metric

        if not os.path.exists(test.query_path_or_metric):
          print('Query file not found {}'.format(query_path))
          test_failure += 1
          continue

        result = run_query_test(trace_processor, gen_trace_path, query_path,
                                expected_path, tmp_perf_path)
      elif test.type == 'metrics':
        result = run_metrics_test(trace_processor, gen_trace_path,
                                  test.query_path_or_metric, expected_path,
                                  tmp_perf_path, metrics_message_factory)
      else:
        assert False

      perf_lines = [line.decode('utf8') for line in tmp_perf_file.readlines()]

    if gen_trace_file:
      if keep_input:
        sys.stderr.write(
            "Saving generated input trace: {}\n".format(gen_trace_path))
      else:
        gen_trace_file.close()
        os.remove(gen_trace_path)

    def write_cmdlines():
      if is_generated_trace:
        sys.stderr.write(
            'Command to generate trace:\n'
            'tools/serialize_test_trace.py --descriptor {} {} > {}\n'.format(
                os.path.relpath(trace_descriptor_path, ROOT_DIR),
                os.path.relpath(trace_path, ROOT_DIR),
                os.path.relpath(gen_trace_path, ROOT_DIR)))
      sys.stderr.write('Command line:\n{}\n'.format(' '.join(result.cmd)))

    if result.exit_code != 0 or result.expected != result.actual:
      sys.stderr.write(result.stderr)

      if result.exit_code == 0:
        sys.stderr.write(
            'Expected did not match actual for trace {} and {} {}\n'.format(
                trace_path, result.test_type, result.input_name))
        sys.stderr.write('Expected file: {}\n'.format(expected_path))
        write_cmdlines()
        write_diff(result.expected, result.actual)
      else:
        write_cmdlines()

      sys.stderr.write('[     FAIL ] {} {}\n'.format(
          os.path.basename(test.query_path_or_metric),
          os.path.basename(trace_path)))

      test_failure += 1
    else:
      assert len(perf_lines) == 1
      perf_numbers = perf_lines[0].split(',')

      assert len(perf_numbers) == 2
      perf_result = PerfResult(test.type, trace_path, test.query_path_or_metric,
                               perf_numbers[0], perf_numbers[1])
      perf_data.append(perf_result)

      sys.stderr.write(
          '[       OK ] {} {} (ingest: {} ms, query: {} ms)\n'.format(
              os.path.basename(test.query_path_or_metric),
              os.path.basename(trace_path),
              perf_result.ingest_time_ns / 1000000,
              perf_result.real_time_ns / 1000000))

  return test_failure, perf_data


def read_all_tests_from_index(index_path, query_metric_pattern, trace_pattern):
  index_dir = os.path.dirname(index_path)

  with open(index_path, 'r') as index_file:
    index_lines = index_file.readlines()

  tests = []
  for line in index_lines:
    stripped = line.strip()
    if stripped.startswith('#'):
      continue
    elif not stripped:
      continue

    [trace_fname, query_fname_or_metric, expected_fname] = stripped.split(' ')
    if not query_metric_pattern.match(os.path.basename(query_fname_or_metric)):
      continue

    if not trace_pattern.match(os.path.basename(trace_fname)):
      continue

    trace_path = os.path.abspath(os.path.join(index_dir, trace_fname))
    expected_path = os.path.abspath(os.path.join(index_dir, expected_fname))

    if query_fname_or_metric.endswith('.sql'):
      test_type = 'queries'
      query_path_or_metric = os.path.abspath(
          os.path.join(index_dir, query_fname_or_metric))
    else:
      test_type = 'metrics'
      query_path_or_metric = query_fname_or_metric

    tests.append(
        Test(test_type, trace_path, query_path_or_metric, expected_path))
  return tests


def read_all_tests(query_metric_pattern, trace_pattern):
  include_index_dir = os.path.join(ROOT_DIR, 'test', 'trace_processor')
  include_index = os.path.join(include_index_dir, 'include_index')
  tests = []
  with open(include_index, 'r') as include_file:
    for index_relpath in include_file.readlines():
      index_path = os.path.join(include_index_dir, index_relpath.strip())
      tests.extend(
          read_all_tests_from_index(index_path, query_metric_pattern,
                                    trace_pattern))
  return tests


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--test-type', type=str, default='all')
  parser.add_argument('--trace-descriptor', type=str)
  parser.add_argument('--metrics-descriptor', type=str)
  parser.add_argument('--perf-file', type=str)
  parser.add_argument(
      '--query-metric-filter',
      default='.*',
      type=str,
      help=
      'Filter the name of query files or metrics to diff test (regex syntax)')
  parser.add_argument(
      '--trace-filter',
      default='.*',
      type=str,
      help='Filter the name of trace files to diff test (regex syntax)')
  parser.add_argument(
      '--keep-input',
      action='store_true',
      help='Save the (generated) input pb file for debugging')
  parser.add_argument(
      'trace_processor', type=str, help='location of trace processor binary')
  args = parser.parse_args()

  query_metric_pattern = re.compile(args.query_metric_filter)
  trace_pattern = re.compile(args.trace_filter)

  tests = read_all_tests(query_metric_pattern, trace_pattern)
  sys.stderr.write('[==========] Running {} tests.\n'.format(len(tests)))

  if args.trace_descriptor:
    trace_descriptor_path = args.trace_descriptor
  else:
    out_path = os.path.dirname(args.trace_processor)

    def find_trace_descriptor(parent):
      trace_protos_path = os.path.join(parent, 'gen', 'protos', 'perfetto',
                                       'trace')
      return os.path.join(trace_protos_path, 'trace.descriptor')

    trace_descriptor_path = find_trace_descriptor(out_path)
    if not os.path.exists(trace_descriptor_path):
      trace_descriptor_path = find_trace_descriptor(
          os.path.join(out_path, 'gcc_like_host'))

  if args.metrics_descriptor:
    metrics_descriptor_path = args.metrics_descriptor
  else:
    out_path = os.path.dirname(args.trace_processor)
    metrics_protos_path = os.path.join(out_path, 'gen', 'protos', 'perfetto',
                                       'metrics')
    metrics_descriptor_path = os.path.join(metrics_protos_path,
                                           'metrics.descriptor')

  metrics_message_factory = create_metrics_message_factory(
      metrics_descriptor_path)

  test_run_start = datetime.datetime.now()
  test_failure, perf_data = run_all_tests(
      args.trace_processor, trace_descriptor_path, metrics_message_factory,
      tests, args.keep_input)
  test_run_end = datetime.datetime.now()

  sys.stderr.write('[==========] {} tests ran. ({} ms total)\n'.format(
      len(tests), int((test_run_end - test_run_start).total_seconds() * 1000)))
  sys.stderr.write('[  PASSED  ] {} tests.\n'.format(len(tests) - test_failure))

  if test_failure == 0:
    if args.perf_file:
      test_dir = os.path.join(ROOT_DIR, 'test')
      trace_processor_dir = os.path.join(test_dir, 'trace_processor')

      metrics = []
      sorted_data = sorted(
          perf_data,
          key=lambda x: (x.test_type, x.trace_path, x.query_path_or_metric))
      for perf_args in sorted_data:
        trace_short_path = os.path.relpath(perf_args.trace_path, test_dir)

        query_short_path_or_metric = perf_args.query_path_or_metric
        if perf_args.test_type == 'queries':
          query_short_path_or_metric = os.path.relpath(
              perf_args.query_path_or_metric, trace_processor_dir)

        metrics.append({
            'metric': 'tp_perf_test_ingest_time',
            'value': float(perf_args.ingest_time_ns) / 1.0e9,
            'unit': 's',
            'tags': {
                'test_name':
                    '{}-{}'.format(trace_short_path,
                                   query_short_path_or_metric),
                'test_type':
                    perf_args.test_type,
            },
            'labels': {},
        })
        metrics.append({
            'metric': 'perf_test_real_time',
            'value': float(perf_args.real_time_ns) / 1.0e9,
            'unit': 's',
            'tags': {
                'test_name':
                    '{}-{}'.format(
                        os.path.relpath(perf_args.trace_path, test_dir),
                        query_short_path_or_metric),
                'test_type':
                    perf_args.test_type,
            },
            'labels': {},
        })

      output_data = {'metrics': metrics}
      with open(args.perf_file, 'w+') as perf_file:
        perf_file.write(json.dumps(output_data, indent=2))
    return 0
  else:
    sys.stderr.write('[  FAILED  ] {} tests.\n'.format(test_failure))
    return 1


if __name__ == '__main__':
  sys.exit(main())

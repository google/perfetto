#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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
import difflib
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import List, Tuple
import concurrent.futures

from google.protobuf import text_format
from python.generators.diff_tests.testing import DiffTest
from python.generators.diff_tests.utils import (create_message_factory,
                                                end_color, get_env, green, red,
                                                yellow)
from tools.proto_utils import serialize_python_trace, serialize_textproto_trace

ROOT_DIR = os.path.dirname(
    os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


# Performance result of running the test.
@dataclass
class PerfResult:
  test_type: DiffTest.TestType
  trace_path: str
  query_path_or_metric: str
  ingest_time_ns: int
  real_time_ns: int

  def __init__(self, test: DiffTest, perf_lines: List[str]):
    self.test_type = test.type
    self.trace_path = test.trace_path
    self.query_path_or_metric = test.query_path

    assert len(perf_lines) == 1
    perf_numbers = perf_lines[0].split(',')

    assert len(perf_numbers) == 2
    self.ingest_time_ns = int(perf_numbers[0])
    self.real_time_ns = int(perf_numbers[1])


# Data gathered from running the test.
@dataclass
class TestResult:
  test_type: DiffTest.TestType
  input_name: str
  trace: str
  cmd: List[str]
  expected: str
  actual: str
  passed: bool
  stderr: str
  exit_code: int
  perf_lines: List[str]

  def __init__(self, type: DiffTest.TestType, query: str, gen_trace_path: str,
               cmd: List[str], expected_text: str, actual_text: str,
               stderr: str, exit_code: int, perf_lines: List[str]) -> None:
    self.test_type = type
    self.input_name = query
    self.trace = gen_trace_path
    self.cmd = cmd
    self.stderr = stderr
    self.exit_code = exit_code
    self.perf_lines = perf_lines
    self.expected = expected_text
    self.actual = actual_text

    expected_content = expected_text.replace('\r\n', '\n')
    actual_content = actual_text.replace('\r\n', '\n')
    self.passed = (expected_content == actual_content)

  def write_diff(self):
    expected_lines = self.expected.splitlines(True)
    actual_lines = self.actual.splitlines(True)
    diff = difflib.unified_diff(
        expected_lines, actual_lines, fromfile='expected', tofile='actual')
    return "".join(list(diff))


# Run a metrics based DiffTest.
def run_metrics_test(test: DiffTest, trace_processor_path: str,
                     gen_trace_path: str,
                     metrics_message_factory) -> TestResult:
  if test.expected_path:
    with open(test.expected_path, 'r') as expected_file:
      expected = expected_file.read()
  else:
    expected = test.blueprint.out
  tmp_perf_file = tempfile.NamedTemporaryFile(delete=False)
  json_output = os.path.basename(test.expected_path).endswith('.json.out')
  cmd = [
      trace_processor_path,
      '--analyze-trace-proto-content',
      '--crop-track-events',
      '--run-metrics',
      test.query_path,
      '--metrics-output=%s' % ('json' if json_output else 'binary'),
      '--perf-file',
      tmp_perf_file.name,
      gen_trace_path,
  ]
  tp = subprocess.Popen(
      cmd,
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      env=get_env(ROOT_DIR))
  (stdout, stderr) = tp.communicate()

  if json_output:
    expected_text = expected
    actual_text = stdout.decode('utf8')
  else:
    # Expected will be in text proto format and we'll need to parse it to
    # a real proto.
    expected_message = metrics_message_factory()
    text_format.Merge(expected, expected_message)

    # Actual will be the raw bytes of the proto and we'll need to parse it
    # into a message.
    actual_message = metrics_message_factory()
    actual_message.ParseFromString(stdout)

    # Convert both back to text format.
    expected_text = text_format.MessageToString(expected_message)
    actual_text = text_format.MessageToString(actual_message)

  perf_lines = [line.decode('utf8') for line in tmp_perf_file.readlines()]
  tmp_perf_file.close()
  os.remove(tmp_perf_file.name)
  return TestResult(test.type, test.query_path,
                    gen_trace_path, cmd, expected_text, actual_text,
                    stderr.decode('utf8'), tp.returncode, perf_lines)


# Run a query based Diff Test.
def run_query_test(test: DiffTest, trace_processor_path: str,
                   gen_trace_path: str) -> TestResult:
  with open(test.expected_path, 'r') as expected_file:
    expected = expected_file.read()
  tmp_perf_file = tempfile.NamedTemporaryFile(delete=False)
  cmd = [
      trace_processor_path,
      '--analyze-trace-proto-content',
      '--crop-track-events',
      '-q',
      test.query_path if test.query_path else test.blueprint.query,
      '--perf-file',
      tmp_perf_file.name,
      gen_trace_path,
  ]
  tp = subprocess.Popen(
      cmd,
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      env=get_env(ROOT_DIR))
  (stdout, stderr) = tp.communicate()

  perf_lines = [line.decode('utf8') for line in tmp_perf_file.readlines()]
  tmp_perf_file.close()
  os.remove(tmp_perf_file.name)

  return TestResult(test.type, test.query_path, gen_trace_path, cmd, expected,
                    stdout.decode('utf8'), stderr.decode('utf8'), tp.returncode,
                    perf_lines)


# Run a DiffTest
def run_test(trace_descriptor_path: str, extension_descriptor_paths: List[str],
             args: argparse.Namespace,
             test: DiffTest) -> Tuple[str, bool, str, PerfResult]:
  out_path = os.path.dirname(args.trace_processor)
  if args.metrics_descriptor:
    metrics_descriptor_paths = [args.metrics_descriptor]
  else:
    metrics_protos_path = os.path.join(out_path, 'gen', 'protos', 'perfetto',
                                       'metrics')
    metrics_descriptor_paths = [
        os.path.join(metrics_protos_path, 'metrics.descriptor'),
        os.path.join(metrics_protos_path, 'chrome',
                     'all_chrome_metrics.descriptor')
    ]
  metrics_message_factory = create_message_factory(
      metrics_descriptor_paths, 'perfetto.protos.TraceMetrics')
  result_str = ""
  red_str = red(args.no_colors)
  green_str = green(args.no_colors)
  end_color_str = end_color(args.no_colors)
  expected_path = test.expected_path
  test_name = f"{test.name}"

  if not os.path.exists(test.trace_path):
    result_str += f"Trace file not found {test.trace_path}\n"
    return test_name, False, result_str, None
  elif not os.path.exists(expected_path):
    result_str = f"Expected file not found {expected_path}"
    return test_name, False, result_str, None

  is_generated_trace = test.trace_path.endswith(
      '.py') or test.trace_path.endswith('.textproto')
  if test.trace_path.endswith('.py'):
    gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
    serialize_python_trace(trace_descriptor_path, test.trace_path,
                           gen_trace_file)
    gen_trace_path = os.path.realpath(gen_trace_file.name)
  elif test.trace_path.endswith('.textproto'):
    gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
    serialize_textproto_trace(trace_descriptor_path, extension_descriptor_paths,
                              test.trace_path, gen_trace_file)
    gen_trace_path = os.path.realpath(gen_trace_file.name)
  else:
    gen_trace_file = None
    gen_trace_path = test.trace_path

  result_str += f"{yellow(args.no_colors)}[ RUN      ]{end_color_str} "
  result_str += f"{test_name}\n"

  # We can't use delete=True here. When using that on Windows, the
  # resulting file is opened in exclusive mode (in turn that's a subtle
  # side-effect of the underlying CreateFile(FILE_ATTRIBUTE_TEMPORARY))
  # and TP fails to open the passed path.
  if test.type == DiffTest.TestType.QUERY:

    if not os.path.exists(test.query_path):
      result_str += f"Query file not found {test.query_path}"
      return test_name, False, result_str, None

    result = run_query_test(test, args.trace_processor, gen_trace_path)
  elif test.type == DiffTest.TestType.METRIC:
    result = run_metrics_test(test, args.trace_processor, gen_trace_path,
                              metrics_message_factory)
  else:
    assert False

  if gen_trace_file:
    if args.keep_input:
      result_str += f"Saving generated input trace: {gen_trace_path}\n"
    else:
      gen_trace_file.close()
      os.remove(gen_trace_path)

  def write_cmdlines():
    res = ""
    if is_generated_trace:
      res += 'Command to generate trace:\n'
      res += 'tools/serialize_test_trace.py '
      res += '--descriptor {} {} > {}\n'.format(
          os.path.relpath(trace_descriptor_path, ROOT_DIR),
          os.path.relpath(test.trace_path, ROOT_DIR),
          os.path.relpath(gen_trace_path, ROOT_DIR))
    res += f"Command line:\n{' '.join(result.cmd)}\n"
    return res

  if result.exit_code != 0 or not result.passed:
    result_str += result.stderr

    if result.exit_code == 0:
      result_str += (
          f"Expected did not match actual for trace "
          f"{test.trace_path} and {result.test_type} {result.input_name}\n"
          f"Expected file: {expected_path}\n")
      result_str += write_cmdlines()
      result_str += result.write_diff()
    else:
      result_str += write_cmdlines()

    result_str += f"{red_str}[  FAILED  ]{end_color_str} {test_name} "
    result_str += f"{os.path.basename(test.trace_path)}\n"

    if args.rebase:
      if result.exit_code == 0:
        result_str += f"Rebasing {expected_path}\n"
        with open(expected_path, 'w') as f:
          f.write(result.actual)
      else:
        result_str += f"Rebase failed for {expected_path} as query failed\n"

    return test_name, False, result_str, None
  else:
    perf_result = PerfResult(test, result.perf_lines)

    result_str += (f"{green_str}[       OK ]{end_color_str} {test.name} "
                   f"(ingest: {perf_result.ingest_time_ns / 1000000:.2f} ms "
                   f"query: {perf_result.real_time_ns / 1000000:.2f} ms)\n")
  return test_name, True, result_str, perf_result


# Run all DiffTests.
def run_all_tests(trace_descriptor_path: str,
                  extension_descriptor_paths: List['str'],
                  args: argparse.Namespace, tests: List[DiffTest]
                 ) -> Tuple[List[str], List[PerfResult], List[str]]:
  perf_data = []
  test_failure = []
  rebased = []
  with concurrent.futures.ProcessPoolExecutor() as e:
    fut = [
        e.submit(run_test, trace_descriptor_path, extension_descriptor_paths,
                 args, test) for test in tests
    ]
    for res in concurrent.futures.as_completed(fut):
      test_name, test_passed, res_str, perf_result = res.result()
      sys.stderr.write(res_str)
      if test_passed:
        perf_data.append(perf_result)
      else:
        if args.rebase:
          rebased.append(test_name)
        test_failure.append(test_name)

  return test_failure, perf_data, rebased


# Load all DiffTests matching the patterns.
def read_all_tests(query_metric_pattern, trace_pattern):
  include_index_dir = os.path.join(ROOT_DIR, 'test', 'trace_processor')
  tests = []

  INCLUDE_PATH = os.path.join(ROOT_DIR, 'test', 'trace_processor')
  sys.path.append(INCLUDE_PATH)
  from include_index import fetch_all_diff_tests
  sys.path.pop()
  diff_tests = fetch_all_diff_tests(include_index_dir)

  for test in diff_tests:
    # Temporary assertion until string passing is supported.
    if not (test.blueprint.is_out_file() and test.blueprint.is_query_file() and
            test.blueprint.is_trace_file()):
      raise AssertionError("Test parameters should be passed as files.")
    if test.query_path and not query_metric_pattern.match(
        os.path.basename(test.name)):
      continue

    if test.trace_path and not trace_pattern.match(
        os.path.basename(test.trace_path)):
      continue

    tests.append(test)
  return tests

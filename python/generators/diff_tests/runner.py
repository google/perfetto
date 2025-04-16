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

import concurrent.futures
import datetime
import difflib
import os
import subprocess
import sys
import tempfile
from binascii import unhexlify
from dataclasses import dataclass
from typing import List, Tuple, Optional

from google.protobuf import text_format
from python.generators.diff_tests.testing import Metric, MetricV2SpecTextproto, Path, TestCase, TestType, BinaryProto, TextProto
from python.generators.diff_tests.utils import (
    ColorFormatter, create_message_factory, get_env, get_trace_descriptor_path,
    read_all_tests, serialize_python_trace, serialize_textproto_trace,
    modify_trace)

ROOT_DIR = os.path.dirname(
    os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


# Performance result of running the test.
@dataclass
class PerfResult:
  test: TestCase
  ingest_time_ns: int
  real_time_ns: int

  def __init__(self, test: TestCase, perf_lines: List[str]):
    self.test = test

    assert len(perf_lines) == 1
    perf_numbers = perf_lines[0].split(',')

    assert len(perf_numbers) == 2
    self.ingest_time_ns = int(perf_numbers[0])
    self.real_time_ns = int(perf_numbers[1])


# Data gathered from running the test.
@dataclass
class TestResult:
  test: TestCase
  trace: str
  cmd: List[str]
  expected: str
  actual: str
  passed: bool
  stderr: str
  exit_code: int
  perf_result: Optional[PerfResult]

  def __init__(
      self,
      test: TestCase,
      gen_trace_path: str,
      cmd: List[str],
      expected_text: str,
      actual_text: str,
      stderr: str,
      exit_code: int,
      perf_lines: List[str],
  ) -> None:
    self.test = test
    self.trace = gen_trace_path
    self.cmd = cmd
    self.stderr = stderr
    self.exit_code = exit_code

    # For better string formatting we often add whitespaces, which has to now
    # be removed.
    def strip_whitespaces(text: str):
      no_front_new_line_text = text.lstrip('\n')
      return '\n'.join(s.strip() for s in no_front_new_line_text.split('\n'))

    self.expected = strip_whitespaces(expected_text)
    self.actual = strip_whitespaces(actual_text)

    expected_content = self.expected.replace('\r\n', '\n')

    actual_content = self.actual.replace('\r\n', '\n')
    self.passed = (expected_content == actual_content)

    if self.exit_code == 0:
      self.perf_result = PerfResult(self.test, perf_lines)
    else:
      self.perf_result = None

  def write_diff(self):
    expected_lines = self.expected.splitlines(True)
    actual_lines = self.actual.splitlines(True)
    diff = difflib.unified_diff(
        expected_lines, actual_lines, fromfile='expected', tofile='actual')
    return "".join(list(diff))


# Results of running the test suite. Mostly used for printing aggregated
# results.
@dataclass
class TestResults:
  test_failures: List[str]
  perf_data: List[PerfResult]
  test_time_ms: int

  def str(self, no_colors: bool, tests_no: int):
    c = ColorFormatter(no_colors)
    res = (
        f"[==========] {tests_no} tests ran. ({self.test_time_ms} ms total)\n"
        f"{c.green('[  PASSED  ]')} "
        f"{tests_no - len(self.test_failures)} tests.\n")
    if len(self.test_failures) > 0:
      res += (f"{c.red('[  FAILED  ]')} "
              f"{len(self.test_failures)} tests.\n")
      for failure in self.test_failures:
        res += f"{c.red('[  FAILED  ]')} {failure}\n"
    return res


# Responsible for executing singular diff test.
@dataclass
class TestCaseRunner:
  test: TestCase
  trace_processor_path: str
  trace_descriptor_path: str
  colors: ColorFormatter
  override_sql_package_paths: List[str]

  def __output_to_text_proto(self, actual: str, out: BinaryProto) -> str:
    """Deserializes a binary proto and returns its text representation.

    Args:
      actual: (string) HEX encoded serialized proto message
      message_type: (string) Message type

    Returns:
      Text proto
    """
    try:
      protos_dir = os.path.join(
          ROOT_DIR,
          os.path.dirname(self.trace_processor_path),
          'gen',
          'protos',
      )
      raw_data = unhexlify(actual.splitlines()[-1][1:-1])
      descriptor_paths = [
          f.path
          for f in os.scandir(
              os.path.join(protos_dir, 'perfetto', 'trace_processor'))
          if f.is_file() and os.path.splitext(f.name)[1] == '.descriptor'
      ]
      descriptor_paths.append(
          os.path.join(protos_dir, 'third_party', 'pprof',
                       'profile.descriptor'))
      proto = create_message_factory(descriptor_paths, out.message_type)()
      proto.ParseFromString(raw_data)
      try:
        return out.post_processing(proto)
      except:
        return '<Proto post processing failed>'
    except:
      return '<Invalid input for proto deserializaiton>'

  def __run_metrics_test(self, trace_path: str,
                         metrics_message_factory) -> TestResult:
    with tempfile.NamedTemporaryFile(delete=False) as tmp_perf_file:
      assert isinstance(self.test.blueprint.query, Metric)

      is_json_output_file = self.test.blueprint.is_out_file(
      ) and os.path.basename(self.test.expected_path).endswith('.json.out')
      is_json_output = is_json_output_file or self.test.blueprint.is_out_json()
      cmd = [
          self.trace_processor_path,
          '--analyze-trace-proto-content',
          '--crop-track-events',
          '--extra-checks',
          '--run-metrics',
          self.test.blueprint.query.name,
          '--metrics-output=%s' % ('json' if is_json_output else 'binary'),
          '--perf-file',
          tmp_perf_file.name,
          trace_path,
      ]
      if self.test.register_files_dir:
        cmd += ['--register-files-dir', self.test.register_files_dir]
      for sql_package_path in self.override_sql_package_paths:
        cmd += ['--override-sql-package', sql_package_path]
      tp = subprocess.Popen(
          cmd,
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
          env=get_env(ROOT_DIR))
      (stdout, stderr) = tp.communicate()

      if is_json_output:
        expected_text = self.test.expected_str
        actual_text = stdout.decode('utf8')
      else:
        # Expected will be in text proto format and we'll need to parse it to
        # a real proto.
        expected_message = metrics_message_factory()
        text_format.Merge(self.test.expected_str, expected_message)

        # Actual will be the raw bytes of the proto and we'll need to parse it
        # into a message.
        actual_message = metrics_message_factory()
        actual_message.ParseFromString(stdout)

        # Convert both back to text format.
        expected_text = text_format.MessageToString(expected_message)
        actual_text = text_format.MessageToString(actual_message)

      os.remove(tmp_perf_file.name)

      return TestResult(
          self.test,
          trace_path,
          cmd,
          expected_text,
          actual_text,
          stderr.decode('utf8'),
          tp.returncode,
          [line.decode('utf8') for line in tmp_perf_file.readlines()],
      )

  def __run_metrics_v2_test(
      self,
      trace_path: str,
      keep_input: bool,
      summary_spec_message_factory,
      summary_message_factory,
  ) -> TestResult:
    with tempfile.NamedTemporaryFile(delete=False) as tmp_perf_file, \
         tempfile.NamedTemporaryFile(delete=False) as tmp_spec_file:
      assert isinstance(self.test.blueprint.query, MetricV2SpecTextproto)

      spec_message = summary_spec_message_factory()
      text_format.Merge(self.test.blueprint.query.contents,
                        spec_message.metric_spec.add())

      tmp_spec_file.write(spec_message.SerializeToString())
      tmp_spec_file.flush()

      cmd = [
          self.trace_processor_path,
          '--analyze-trace-proto-content',
          '--crop-track-events',
          '--extra-checks',
          '--perf-file',
          tmp_perf_file.name,
          '--summary',
          '--summary-spec',
          tmp_spec_file.name,
          '--summary-metrics-v2',
          spec_message.metric_spec[0].id,
          '--summary-format',
          'binary',
          trace_path,
      ]
      for sql_package_path in self.override_sql_package_paths:
        cmd += ['--override-sql-package', sql_package_path]
      tp = subprocess.Popen(
          cmd,
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
          env=get_env(ROOT_DIR),
      )
      (stdout, stderr) = tp.communicate()

      # Expected will be in text proto format and we'll need to parse it to
      # a real proto.
      expected_summary = summary_message_factory()
      text_format.Merge(self.test.expected_str, expected_summary.metric.add())

      # Actual will be the raw bytes of the proto and we'll need to parse it
      # into a message.
      actual_summary = summary_message_factory()
      actual_summary.ParseFromString(stdout)

      os.remove(tmp_perf_file.name)
      if not keep_input:
        os.remove(tmp_spec_file.name)

      return TestResult(
          self.test,
          trace_path,
          cmd,
          text_format.MessageToString(expected_summary.metric[0]),
          text_format.MessageToString(actual_summary.metric[0]),
          stderr.decode('utf8'),
          tp.returncode,
          [line.decode('utf8') for line in tmp_perf_file.readlines()],
      )

  # Run a query based Diff Test.
  def __run_query_test(self, trace_path: str) -> TestResult:
    with tempfile.NamedTemporaryFile(delete=False) as tmp_perf_file:
      cmd = [
          self.trace_processor_path,
          '--analyze-trace-proto-content',
          '--crop-track-events',
          '--extra-checks',
          '--perf-file',
          tmp_perf_file.name,
          trace_path,
      ]
      if self.test.blueprint.is_query_file():
        cmd += ['-q', self.test.query_path]
      else:
        assert isinstance(self.test.blueprint.query, str)
        cmd += ['-Q', self.test.blueprint.query]
      if self.test.register_files_dir:
        cmd += ['--register-files-dir', self.test.register_files_dir]
      for sql_package_path in self.override_sql_package_paths:
        cmd += ['--override-sql-package', sql_package_path]
      tp = subprocess.Popen(
          cmd,
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
          env=get_env(ROOT_DIR))
      (stdout, stderr) = tp.communicate()

      actual = stdout.decode('utf8')
      if self.test.blueprint.is_out_binaryproto():
        assert isinstance(self.test.blueprint.out, BinaryProto)
        actual = self.__output_to_text_proto(actual, self.test.blueprint.out)

      os.remove(tmp_perf_file.name)

      return TestResult(
          self.test,
          trace_path,
          cmd,
          self.test.expected_str,
          actual,
          stderr.decode('utf8'),
          tp.returncode,
          [line.decode('utf8') for line in tmp_perf_file.readlines()],
      )

  def __run(
      self,
      summary_descriptor_path: str,
      metrics_descriptor_paths: List[str],
      extension_descriptor_paths: List[str],
      keep_input,
  ) -> Tuple[TestResult, str]:
    # We can't use delete=True here. When using that on Windows, the
    # resulting file is opened in exclusive mode (in turn that's a subtle
    # side-effect of the underlying CreateFile(FILE_ATTRIBUTE_TEMPORARY))
    # and TP fails to open the passed path.
    gen_trace_file = None
    if self.test.blueprint.is_trace_file():
      assert self.test.trace_path
      if self.test.trace_path.endswith('.py'):
        gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
        serialize_python_trace(ROOT_DIR, self.trace_descriptor_path,
                               self.test.trace_path, gen_trace_file)

      elif self.test.trace_path.endswith('.textproto'):
        gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
        serialize_textproto_trace(self.trace_descriptor_path,
                                  extension_descriptor_paths,
                                  self.test.trace_path, gen_trace_file)

    elif self.test.blueprint.is_trace_textproto():
      gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
      proto = create_message_factory([self.trace_descriptor_path] +
                                     extension_descriptor_paths,
                                     'perfetto.protos.Trace')()
      assert isinstance(self.test.blueprint.trace, TextProto)
      text_format.Merge(self.test.blueprint.trace.contents, proto)
      gen_trace_file.write(proto.SerializeToString())
      gen_trace_file.flush()

    else:
      gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
      with open(gen_trace_file.name, 'w') as trace_file:
        trace_file.write(self.test.blueprint.trace.contents)

    if self.test.blueprint.trace_modifier is not None:
      if gen_trace_file:
        # Overwrite |gen_trace_file|.
        modify_trace(self.trace_descriptor_path, extension_descriptor_paths,
                     gen_trace_file.name, gen_trace_file.name,
                     self.test.blueprint.trace_modifier)
      else:
        # Create |gen_trace_file| to save the modified trace.
        gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
        modify_trace(self.trace_descriptor_path, extension_descriptor_paths,
                     self.test.trace_path, gen_trace_file.name,
                     self.test.blueprint.trace_modifier)

    if gen_trace_file:
      trace_path = os.path.realpath(gen_trace_file.name)
    else:
      trace_path = self.test.trace_path
    assert trace_path

    str = f"{self.colors.yellow('[ RUN      ]')} {self.test.name}\n"

    if self.test.type == TestType.QUERY:
      result = self.__run_query_test(trace_path)
    elif self.test.type == TestType.METRIC:
      result = self.__run_metrics_test(
          trace_path,
          create_message_factory(metrics_descriptor_paths,
                                 'perfetto.protos.TraceMetrics'),
      )
    elif self.test.type == TestType.METRIC_V2:
      result = self.__run_metrics_v2_test(
          trace_path,
          keep_input,
          create_message_factory([summary_descriptor_path],
                                 'perfetto.protos.TraceSummarySpec'),
          create_message_factory([summary_descriptor_path],
                                 'perfetto.protos.TraceSummary'),
      )
    else:
      assert False

    if gen_trace_file:
      if not keep_input:
        gen_trace_file.close()
        os.remove(trace_path)

    def write_cmdlines():
      res = ""
      if self.test.trace_path and (self.test.trace_path.endswith('.textproto')
                                   or self.test.trace_path.endswith('.py')):
        res += 'Command to generate trace:\n'
        res += 'tools/serialize_test_trace.py '
        res += '--descriptor {} {} > {}\n'.format(
            os.path.relpath(self.trace_descriptor_path, ROOT_DIR),
            os.path.relpath(self.test.trace_path, ROOT_DIR),
            os.path.relpath(trace_path, ROOT_DIR))
      res += f"Command line:\n{' '.join(result.cmd)}\n"
      return res

    if result.exit_code != 0 or not result.passed:
      result.passed = False
      str += result.stderr

      if result.exit_code == 0:
        str += f"Expected did not match actual for test {self.test.name}.\n"
        str += write_cmdlines()
        str += result.write_diff()
      else:
        str += write_cmdlines()

      str += (f"{self.colors.red('[  FAILED  ]')} {self.test.name}\n")

      return result, str
    else:
      assert result.perf_result
      str += (f"{self.colors.green('[       OK ]')} {self.test.name} "
              f"(ingest: {result.perf_result.ingest_time_ns / 1000000:.2f} ms "
              f"query: {result.perf_result.real_time_ns / 1000000:.2f} ms)\n")
    return result, str

  # Run a TestCase.
  def execute(
      self,
      summary_descriptor_path: str,
      metrics_descriptor_paths: List[str],
      extension_descriptor_paths: List[str],
      keep_input: bool,
  ) -> Tuple[str, str, TestResult]:
    if not metrics_descriptor_paths:
      out_path = os.path.dirname(self.trace_processor_path)
      metrics_protos_path = os.path.join(
          out_path,
          'gen',
          'protos',
          'perfetto',
          'metrics',
      )
      metrics_descriptor_paths = [
          os.path.join(metrics_protos_path, 'metrics.descriptor'),
          os.path.join(metrics_protos_path, 'chrome',
                       'all_chrome_metrics.descriptor'),
          os.path.join(metrics_protos_path, 'webview',
                       'all_webview_metrics.descriptor')
      ]
    result, run_str = self.__run(
        summary_descriptor_path,
        metrics_descriptor_paths,
        extension_descriptor_paths,
        keep_input,
    )
    return self.test.name, run_str, result


# Fetches and executes all diff viable tests.
@dataclass
class DiffTestsRunner:
  tests: List[TestCase]
  trace_processor_path: str
  trace_descriptor_path: str
  test_runners: List[TestCaseRunner]
  quiet: bool

  def __init__(
      self,
      name_filter: str,
      trace_processor_path: str,
      trace_descriptor: str,
      no_colors: bool,
      override_sql_package_paths: List[str],
      test_dir: str,
      quiet: bool,
  ):
    self.tests = read_all_tests(name_filter, test_dir)
    self.trace_processor_path = trace_processor_path
    self.quiet = quiet

    out_path = os.path.dirname(self.trace_processor_path)
    self.trace_descriptor_path = get_trace_descriptor_path(
        out_path,
        trace_descriptor,
    )
    self.test_runners = []
    color_formatter = ColorFormatter(no_colors)
    for test in self.tests:
      self.test_runners.append(
          TestCaseRunner(
              test,
              self.trace_processor_path,
              self.trace_descriptor_path,
              color_formatter,
              override_sql_package_paths,
          ))

  def run_all_tests(
      self,
      summary_descriptor: str,
      metrics_descriptor_paths: List[str],
      chrome_extensions: str,
      test_extensions: str,
      winscope_extensions: str,
      keep_input: bool,
  ) -> TestResults:
    perf_results = []
    failures = []
    test_run_start = datetime.datetime.now()
    completed_tests = 0

    with concurrent.futures.ProcessPoolExecutor() as e:
      fut = [
          e.submit(
              test.execute,
              summary_descriptor,
              metrics_descriptor_paths,
              [chrome_extensions, test_extensions, winscope_extensions],
              keep_input,
          ) for test in self.test_runners
      ]
      for res in concurrent.futures.as_completed(fut):
        test_name, res_str, result = res.result()

        if self.quiet:
          completed_tests += 1
          sys.stderr.write(f"\rRan {completed_tests} tests")
          if not result.passed:
            sys.stderr.write(f"\r")
            sys.stderr.write(res_str)
        else:
          sys.stderr.write(res_str)

        if not result or not result.passed:
          failures.append(test_name)
        else:
          perf_results.append(result.perf_result)
    test_time_ms = int(
        (datetime.datetime.now() - test_run_start).total_seconds() * 1000)
    if self.quiet:
      sys.stderr.write(f"\r")
    return TestResults(failures, perf_results, test_time_ms)

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

import abc
import os
import subprocess
import tempfile
from binascii import unhexlify
from typing import Any, List

from google.protobuf import text_format

from python.generators.diff_tests.testing import (BinaryProto, Metric,
                                                  MetricV2SpecTextproto)
from python.generators.diff_tests.models import TestCase, TestResult
from python.generators.diff_tests.utils import ProtoManager, get_env

ROOT_DIR = os.path.dirname(
    os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


class TestExecutor(abc.ABC):
  """Abstract base class for test executors."""

  def __init__(self, trace_processor_path: str,
               override_sql_package_paths: List[str]):
    self.trace_processor_path = trace_processor_path
    self.override_sql_package_paths = override_sql_package_paths

  def _execute_trace_processor(self, cmd: List[str]):
    tp = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=get_env(ROOT_DIR))
    return tp.communicate(), tp.returncode

  @abc.abstractmethod
  def run(self, test: TestCase, trace_path: str) -> TestResult:
    pass


def _output_to_text_proto(trace_processor_path: str, actual: str,
                          out: BinaryProto) -> str:
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
        os.path.dirname(trace_processor_path),
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
        os.path.join(protos_dir, 'third_party', 'pprof', 'profile.descriptor'))
    proto = ProtoManager(descriptor_paths).create_message(out.message_type)()
    proto.ParseFromString(raw_data)
    try:
      return out.post_processing(proto)
    except:
      return '<Proto post processing failed>'
  except:
    return '<Invalid input for proto deserializaiton>'


class QueryTestExecutor(TestExecutor):
  """Executor for query-based tests."""

  def _execute_and_analyze(self, test: TestCase, trace_path: str,
                           cmd: List[str], perf_file_path: str):
    (stdout, stderr), returncode = self._execute_trace_processor(cmd)
    actual = stdout.decode('utf8')
    if test.blueprint.is_out_binaryproto():
      assert isinstance(test.blueprint.out, BinaryProto)
      actual = _output_to_text_proto(self.trace_processor_path, actual,
                                     test.blueprint.out)
    with open(perf_file_path, 'r') as f:
      return TestResult(test, trace_path, cmd, test.expected_str, actual,
                        stderr.decode('utf8'), returncode,
                        [line for line in f.readlines()])

  def run(self, test: TestCase, trace_path: str) -> TestResult:
    with tempfile.NamedTemporaryFile(mode='w+', delete=False) as tmp_perf_file:
      cmd = [
          self.trace_processor_path,
          '--analyze-trace-proto-content',
          '--crop-track-events',
          '--extra-checks',
          '--perf-file',
          tmp_perf_file.name,
          trace_path,
      ]
      if test.blueprint.is_query_file():
        cmd += ['-q', test.query_path]
      else:
        assert isinstance(test.blueprint.query, str)
        cmd += ['-Q', test.blueprint.query]
      if test.register_files_dir:
        cmd += ['--register-files-dir', test.register_files_dir]
      for sql_package_path in self.override_sql_package_paths:
        cmd += ['--override-sql-package', sql_package_path]

      result = self._execute_and_analyze(test, trace_path, cmd,
                                         tmp_perf_file.name)
    os.remove(tmp_perf_file.name)
    return result


class MetricTestExecutor(TestExecutor):
  """Executor for metric-based tests."""

  def __init__(self, trace_processor_path: str,
               override_sql_package_paths: List[str],
               metrics_message_factory: Any):
    super().__init__(trace_processor_path, override_sql_package_paths)
    self.metrics_message_factory = metrics_message_factory

  def _execute_and_analyze(self, test: TestCase, trace_path: str,
                           cmd: List[str], is_json_output: bool,
                           perf_file_path: str):
    (stdout, stderr), returncode = self._execute_trace_processor(cmd)

    if is_json_output:
      expected_text = test.expected_str
      actual_text = stdout.decode('utf8')
    else:
      # Expected will be in text proto format and we'll need to parse it to
      # a real proto.
      expected_message = self.metrics_message_factory(
          'perfetto.protos.TraceMetrics')()
      text_format.Merge(test.expected_str, expected_message)

      # Actual will be the raw bytes of the proto and we'll need to parse it
      # into a message.
      actual_message = self.metrics_message_factory(
          'perfetto.protos.TraceMetrics')()
      actual_message.ParseFromString(stdout)

      # Convert both back to text format.
      expected_text = text_format.MessageToString(expected_message)
      actual_text = text_format.MessageToString(actual_message)

    with open(perf_file_path, 'r') as f:
      return TestResult(test, trace_path, cmd, expected_text, actual_text,
                        stderr.decode('utf8'), returncode,
                        [line for line in f.readlines()])

  def run(self, test: TestCase, trace_path: str) -> TestResult:
    with tempfile.NamedTemporaryFile(mode='w+', delete=False) as tmp_perf_file:
      assert isinstance(test.blueprint.query, Metric)

      is_json_output_file = test.blueprint.is_out_file() and os.path.basename(
          test.expected_path or '').endswith('.json.out')
      is_json_output = is_json_output_file or test.blueprint.is_out_json()
      cmd = [
          self.trace_processor_path,
          '--analyze-trace-proto-content',
          '--crop-track-events',
          '--extra-checks',
          '--run-metrics',
          test.blueprint.query.name,
          '--metrics-output=%s' % ('json' if is_json_output else 'binary'),
          '--perf-file',
          tmp_perf_file.name,
          trace_path,
      ]
      if test.register_files_dir:
        cmd += ['--register-files-dir', test.register_files_dir]
      for sql_package_path in self.override_sql_package_paths:
        cmd += ['--override-sql-package', sql_package_path]

      result = self._execute_and_analyze(test, trace_path, cmd, is_json_output,
                                         tmp_perf_file.name)
    os.remove(tmp_perf_file.name)
    return result


class MetricV2TestExecutor(TestExecutor):
  """Executor for Metric v2 tests."""

  def __init__(self, trace_processor_path: str,
               override_sql_package_paths: List[str], keep_input: bool,
               summary_spec_message_factory: Any, summary_message_factory: Any):
    super().__init__(trace_processor_path, override_sql_package_paths)
    self.keep_input = keep_input
    self.summary_spec_message_factory = summary_spec_message_factory
    self.summary_message_factory = summary_message_factory

  def _execute_and_analyze(self, test: TestCase, trace_path: str,
                           cmd: List[str], perf_file_path: str):
    (stdout, stderr), returncode = self._execute_trace_processor(cmd)

    # Expected will be in text proto format and we'll need to parse it to
    # a real proto.
    expected_summary = self.summary_message_factory(
        'perfetto.protos.TraceSummary')()
    text_format.Merge(test.expected_str, expected_summary.metric_bundles.add())

    # Actual will be the raw bytes of the proto and we'll need to parse it
    # into a message.
    actual_summary = self.summary_message_factory(
        'perfetto.protos.TraceSummary')()
    actual_summary.ParseFromString(stdout)

    actual = text_format.MessageToString(
        actual_summary.metric_bundles[0]) if len(
            actual_summary.metric_bundles) > 0 else ''

    with open(perf_file_path, 'r') as f:
      return TestResult(
          test, trace_path, cmd,
          text_format.MessageToString(expected_summary.metric_bundles[0]),
          actual, stderr.decode('utf8'), returncode,
          [line for line in f.readlines()])

  def run(self, test: TestCase, trace_path: str) -> TestResult:
    with tempfile.NamedTemporaryFile(mode='w+', delete=False) as tmp_perf_file, \
         tempfile.NamedTemporaryFile(delete=False) as tmp_spec_file:
      assert isinstance(test.blueprint.query, MetricV2SpecTextproto)

      spec_message = self.summary_spec_message_factory(
          'perfetto.protos.TraceSummarySpec')()
      text_format.Merge(test.blueprint.query.contents,
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

      result = self._execute_and_analyze(test, trace_path, cmd,
                                         tmp_perf_file.name)

      if not self.keep_input:
        os.remove(tmp_spec_file.name)
    os.remove(tmp_perf_file.name)
    return result

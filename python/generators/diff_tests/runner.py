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
import os
import sys
from dataclasses import dataclass
from typing import List, Tuple

from python.generators.diff_tests.models import (TestCase, TestResult, TestType,
                                                 PerfResult, Config)
from python.generators.diff_tests.utils import (ColorFormatter, ProtoManager,
                                                get_trace_descriptor_path,
                                                write_diff)
from python.generators.diff_tests.trace_generator import generate_trace_file
from python.generators.diff_tests.test_executor import (QueryTestExecutor,
                                                        MetricTestExecutor,
                                                        MetricV2TestExecutor)
from python.generators.diff_tests.test_loader import TestLoader


@dataclass
class TestResults:
  """Results of running the test suite.

  Mostly used for printing aggregated results.
  """
  test_failures: List[str]
  perf_data: List[PerfResult]
  test_time_ms: int

  def str(self, no_colors: bool, tests_no: int) -> str:
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


@dataclass
class DiffTestsRunner:
  """Fetches and executes all diff tests."""

  def __init__(self, config: Config):
    self.config = config
    self.test_loader = TestLoader(os.path.abspath(self.config.test_dir))

  def run(self) -> TestResults:
    tests = self.test_loader.discover_and_load_tests(self.config.name_filter)

    trace_descriptor_path = get_trace_descriptor_path(
        os.path.dirname(self.config.trace_processor_path),
        self.config.trace_descriptor)

    if not self.config.metrics_descriptor_paths:
      out_path = os.path.dirname(self.config.trace_processor_path)
      metrics_protos_path = os.path.join(out_path, 'gen', 'protos', 'perfetto',
                                         'metrics')
      self.config.metrics_descriptor_paths = [
          os.path.join(metrics_protos_path, 'metrics.descriptor'),
          os.path.join(metrics_protos_path, 'chrome',
                       'all_chrome_metrics.descriptor'),
          os.path.join(metrics_protos_path, 'webview',
                       'all_webview_metrics.descriptor')
      ]

    perf_results = []
    failures = []
    test_run_start = datetime.datetime.now()
    completed_tests = 0

    with concurrent.futures.ProcessPoolExecutor() as e:
      fut = [
          e.submit(self._run_test, test, trace_descriptor_path)
          for test in tests
      ]
      for res in concurrent.futures.as_completed(fut):
        test_name, res_str, result = res.result()

        if self.config.quiet:
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
    if self.config.quiet:
      sys.stderr.write(f"\r")
    return TestResults(failures, perf_results, test_time_ms)

  def _run_test(self, test: TestCase,
                trace_descriptor_path: str) -> Tuple[str, str, TestResult]:
    extension_descriptor_paths = [
        self.config.chrome_extensions, self.config.test_extensions,
        self.config.winscope_extensions
    ]
    gen_trace_file = generate_trace_file(test, trace_descriptor_path,
                                         extension_descriptor_paths)

    if gen_trace_file:
      trace_path = os.path.realpath(gen_trace_file.name)
    else:
      trace_path = test.trace_path
    assert trace_path

    if test.type == TestType.QUERY:
      executor = QueryTestExecutor(self.config.trace_processor_path,
                                   self.config.override_sql_package_paths)
      result = executor.run(test, trace_path)
    elif test.type == TestType.METRIC:
      executor = MetricTestExecutor(
          self.config.trace_processor_path,
          self.config.override_sql_package_paths,
          ProtoManager(self.config.metrics_descriptor_paths).create_message)
      result = executor.run(test, trace_path)
    elif test.type == TestType.METRIC_V2:
      executor = MetricV2TestExecutor(
          self.config.trace_processor_path,
          self.config.override_sql_package_paths, self.config.keep_input,
          ProtoManager([self.config.summary_descriptor]).create_message,
          ProtoManager([self.config.summary_descriptor]).create_message)
      result = executor.run(test, trace_path)
    else:
      assert False

    if gen_trace_file:
      if not self.config.keep_input:
        gen_trace_file.close()
        os.remove(trace_path)

    run_str = self._process_test_result(result, trace_path,
                                        extension_descriptor_paths,
                                        trace_descriptor_path)
    return test.name, run_str, result

  def _process_test_result(
      self,
      result: TestResult,
      trace_path: str,
      extension_descriptor_paths: List[str],
      trace_descriptor_path: str,
  ) -> str:
    colors = ColorFormatter(self.config.no_colors)

    def write_cmdlines() -> str:
      res = ""
      if result.test.trace_path and (
          result.test.trace_path.endswith('.textproto') or
          result.test.trace_path.endswith('.py')):
        res += 'Command to generate trace:\n'
        res += 'tools/serialize_test_trace.py '
        assert result.test.trace_path
        res += '--descriptor {} {} {} > {}\n'.format(
            os.path.relpath(trace_descriptor_path,
                            self.config.test_dir), " ".join([
                                "--extension-descriptor {}".format(
                                    os.path.relpath(p, self.config.test_dir))
                                for p in extension_descriptor_paths
                            ]),
            os.path.relpath(result.test.trace_path, self.config.test_dir),
            os.path.relpath(trace_path, self.config.test_dir),
            extension_descriptor_paths)
      res += f"Command line:\n{' '.join(result.cmd)}\n"
      return res

    run_str = f"{colors.yellow('[ RUN      ]')} {result.test.name}\n"
    if result.exit_code != 0 or not result.passed:
      result.passed = False
      run_str += result.stderr

      if result.exit_code == 0:
        run_str += f"Expected did not match actual for test {result.test.name}.\n"
        run_str += write_cmdlines()
        run_str += write_diff(result.expected, result.actual)
      else:
        run_str += write_cmdlines()

      run_str += (f"{colors.red('[  FAILED  ]')} {result.test.name}\n")
    else:
      assert result.perf_result
      run_str += (
          f"{colors.green('[       OK ]')} {result.test.name} "
          f"(ingest: {result.perf_result.ingest_time_ns / 1000000:.2f} ms "
          f"query: {result.perf_result.real_time_ns / 1000000:.2f} ms)\n")
    return run_str

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

import os
import re
from dataclasses import dataclass
from enum import Enum
from typing import List, Optional, Tuple

from .testing import DiffTestBlueprint

TestName = str


@dataclass
class DiscoveredTests:
  """In-memory database of all discovered tests."""
  # All tests which match the given name filter and module constraints.
  runnable: List['TestCase']

  # All tests which are skipped because they don't match the name filter.
  skipped_name_filter: List[str]

  # All tests which are skipped due to a missing module.
  skipped_module_missing: List[Tuple[str, str]]


@dataclass
class Config:
  """Configuration for the diff test runner."""
  name_filter: str
  trace_processor_path: str
  trace_descriptor: str
  no_colors: bool
  override_sql_package_paths: List[str]
  test_dir: str
  quiet: bool
  summary_descriptor: str
  metrics_descriptor_paths: List[str]
  chrome_extensions: str
  test_extensions: str
  winscope_extensions: str
  simpleperf_descriptor: str
  keep_input: bool
  print_slowest_tests: bool


class TestType(Enum):
  """The type of the diff test."""
  QUERY = 1
  METRIC = 2
  METRIC_V2 = 3


@dataclass
class TestCase:
  """Description of a diff test."""
  name: str
  blueprint: DiffTestBlueprint
  query_path: Optional[str]
  trace_path: Optional[str]
  expected_path: Optional[str]
  expected_str: str
  register_files_dir: Optional[str]
  type: TestType

  def validate(self, name_filter: str):
    query_metric_pattern = re.compile(name_filter)
    return bool(query_metric_pattern.match(os.path.basename(self.name)))


@dataclass
class PerfResult:
  """Performance result of running the test."""
  test: 'TestCase'
  ingest_time_ns: int
  real_time_ns: int

  def __init__(self, test: 'TestCase', perf_lines: List[str]):
    self.test = test

    assert len(perf_lines) == 1
    perf_numbers = perf_lines[0].split(',')

    assert len(perf_numbers) == 2
    self.ingest_time_ns = int(perf_numbers[0])
    self.real_time_ns = int(perf_numbers[1])


@dataclass
class TestResult:
  """Data gathered from running the test."""
  test: 'TestCase'
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
      test: 'TestCase',
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

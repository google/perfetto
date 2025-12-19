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
import sys
from typing import TYPE_CHECKING, Dict, List, Optional, Set, Tuple, Union

from python.generators.diff_tests.testing import (BinaryProto, Csv, DataPath,
                                                  DiffTestBlueprint, Json, Path,
                                                  Systrace, TextProto)
from python.generators.diff_tests.models import DiscoveredTests, TestCase, TestType

if TYPE_CHECKING:
  from python.generators.diff_tests.testing import TestSuite


class TestLoader:
  """Discovers and loads all tests."""

  def __init__(self, root_dir: os.PathLike):
    self.root_dir = root_dir

  def discover_and_load_tests(self, name_filter: str,
                              enabled_modules: Set[str]) -> DiscoveredTests:
    # Import the index file to discover all the tests.
    include_path = os.path.join(self.root_dir, 'test', 'trace_processor',
                                'diff_tests')
    sys.path.append(include_path)
    from include_index import fetch_all_diff_tests
    sys.path.pop()

    all_tests_data = fetch_all_diff_tests(include_path)

    runnable: List[TestCase] = []
    skipped_name_filter: List[str] = []
    skipped_module_missing: List[Tuple[str, str]] = []

    query_metric_pattern = re.compile(name_filter)
    for name, blueprint in all_tests_data:
      if not query_metric_pattern.match(os.path.basename(name)):
        skipped_name_filter.append(name)
        continue

      should_run, reason = self._validate_test(blueprint, enabled_modules)
      if not should_run:
        if reason:
          skipped_module_missing.append((name, reason))
        continue

      query_path = self._get_query_path(name, blueprint)
      trace_path = self._get_trace_path(name, blueprint)
      expected_path = self._get_expected_path(name, blueprint)
      expected_str = self._get_expected_str(name, blueprint, expected_path)
      register_files_dir = self._get_register_files_dir(name, blueprint)
      test_type = self._get_test_type(blueprint)

      runnable.append(
          TestCase(name, blueprint, query_path, trace_path, expected_path,
                   expected_str, register_files_dir, test_type))
    return DiscoveredTests(runnable, skipped_name_filter,
                           skipped_module_missing)

  # Returns a bool that is true if and only if the test should run, and a string
  # describing the reason the test did not run
  def _validate_test(self, blueprint: DiffTestBlueprint,
                     enabled_modules: Set[str]) -> Tuple[bool, Optional[str]]:
    if blueprint.module_dependencies:
      for module in blueprint.module_dependencies:
        if module not in enabled_modules:
          return False, f"module '{module}' not found"
    return True, None

  def _get_test_type(self, blueprint: DiffTestBlueprint) -> TestType:
    if blueprint.is_metric():
      return TestType.METRIC
    elif blueprint.is_metric_v2():
      return TestType.METRIC_V2
    else:
      return TestType.QUERY

  def _get_path(self, name: str, file_path: Union[Path, DataPath],
                index_dir: str, test_data_dir: str) -> str:
    if isinstance(file_path, DataPath):
      path = os.path.join(test_data_dir, file_path.filename)
    else:
      path = os.path.abspath(os.path.join(index_dir, file_path.filename))

    if not os.path.exists(path):
      raise AssertionError(f"File ({path}) for test '{name}' does not exist.")
    return path

  def _get_query_path(self, name: str,
                      blueprint: DiffTestBlueprint) -> Optional[str]:
    if not blueprint.is_query_file():
      return None
    assert isinstance(blueprint.query, (Path, DataPath))
    return self._get_path(name, blueprint.query, blueprint.index_dir,
                          blueprint.test_data_dir)

  def _get_trace_path(self, name: str,
                      blueprint: DiffTestBlueprint) -> Optional[str]:
    if not blueprint.is_trace_file():
      return None
    assert isinstance(blueprint.trace, (Path, DataPath))
    return self._get_path(name, blueprint.trace, blueprint.index_dir,
                          blueprint.test_data_dir)

  def _get_expected_path(self, name: str,
                         blueprint: DiffTestBlueprint) -> Optional[str]:
    if not blueprint.is_out_file():
      return None
    assert isinstance(blueprint.out, (Path, DataPath))
    return self._get_path(name, blueprint.out, blueprint.index_dir,
                          blueprint.test_data_dir)

  def _get_register_files_dir(self, name: str,
                              blueprint: DiffTestBlueprint) -> Optional[str]:
    if not blueprint.register_files_dir:
      return None
    return self._get_path(name, blueprint.register_files_dir,
                          blueprint.index_dir, blueprint.test_data_dir)

  def _get_expected_str(self, name: str, blueprint: DiffTestBlueprint,
                        expected_path: Optional[str]) -> str:
    if blueprint.is_out_file():
      assert expected_path
      with open(expected_path, 'r') as expected_file:
        return expected_file.read()
    assert isinstance(blueprint.out, (
        TextProto,
        Json,
        Csv,
        BinaryProto,
        Systrace,
    ))
    return blueprint.out.contents

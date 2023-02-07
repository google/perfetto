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

import inspect
import os
from dataclasses import dataclass
from typing import List, Union
from enum import Enum
import re

TestName = str


@dataclass
class Path:
  filename: str


@dataclass
class DataPath(Path):
  filename: str


@dataclass
class Metric:
  name: str


@dataclass
class Json:
  contents: str


@dataclass
class Csv:
  contents: str


@dataclass
class TextProto:
  contents: str


@dataclass
class Systrace:
  contents: str


class TestType(Enum):
  QUERY = 1
  METRIC = 2


# Blueprint for running the diff test. 'query' is being run over data from the
# 'trace 'and result will be compared to the 'out. Each test (function in class
# inheriting from TestSuite) returns a DiffTestBlueprint.
@dataclass
class DiffTestBlueprint:

  trace: Union[Path, DataPath, Json, Systrace, TextProto]
  query: Union[str, Path, DataPath, Metric]
  out: Union[Path, DataPath, Json, Csv, TextProto]

  def is_trace_file(self):
    return isinstance(self.trace, Path)

  def is_trace_textproto(self):
    return isinstance(self.trace, TextProto)

  def is_trace_json(self):
    return isinstance(self.trace, Json)

  def is_trace_systrace(self):
    return isinstance(self.trace, Systrace)

  def is_query_file(self):
    return isinstance(self.query, Path)

  def is_metric(self):
    return isinstance(self.query, Metric)

  def is_out_file(self):
    return isinstance(self.out, Path)

  def is_out_json(self):
    return isinstance(self.out, Json)

  def is_out_texproto(self):
    return isinstance(self.out, TextProto)

  def is_out_csv(self):
    return isinstance(self.out, Csv)


# Description of a diff test. Created in `fetch_diff_tests()` in
# TestSuite: each test (function starting with `test_`) returns
# DiffTestBlueprint and function name is a TestCase name. Used by diff test
# script.
class TestCase:

  def __get_query_path(self) -> str:
    if not self.blueprint.is_query_file():
      return None

    if isinstance(self.blueprint.query, DataPath):
      path = os.path.join(self.test_dir, 'data', self.blueprint.query.filename)
    else:
      path = os.path.abspath(
          os.path.join(self.index_dir, self.blueprint.query.filename))

    if not os.path.exists(path):
      raise AssertionError(
          f"Query file ({path}) for test '{self.name}' does not exist.")
    return path

  def __get_trace_path(self) -> str:
    if not self.blueprint.is_trace_file():
      return None

    if isinstance(self.blueprint.trace, DataPath):
      path = os.path.join(self.test_dir, 'data', self.blueprint.trace.filename)
    else:
      path = os.path.abspath(
          os.path.join(self.index_dir, self.blueprint.trace.filename))

    if not os.path.exists(path):
      raise AssertionError(
          f"Trace file ({path}) for test '{self.name}' does not exist.")
    return path

  def __get_out_path(self) -> str:
    if not self.blueprint.is_out_file():
      return None

    if isinstance(self.blueprint.out, DataPath):
      path = os.path.join(self.test_dir, 'data', self.blueprint.out.filename)
    else:
      path = os.path.abspath(
          os.path.join(self.index_dir, self.blueprint.out.filename))

    if not os.path.exists(path):
      raise AssertionError(
          f"Out file ({path}) for test '{self.name}' does not exist.")
    return path

  def __init__(self, name: str, blueprint: DiffTestBlueprint,
               index_dir: str) -> None:
    self.name = name
    self.blueprint = blueprint
    self.index_dir = index_dir
    self.test_dir = os.path.dirname(os.path.dirname(os.path.dirname(index_dir)))

    if blueprint.is_metric():
      self.type = TestType.METRIC
    else:
      self.type = TestType.QUERY

    self.query_path = self.__get_query_path()
    self.trace_path = self.__get_trace_path()
    self.expected_path = self.__get_out_path()

  # Verifies that the test should be in test suite. If False, test will not be
  # executed.
  def validate(self, name_filter: str):
    query_metric_pattern = re.compile(name_filter)
    return bool(query_metric_pattern.match(os.path.basename(self.name)))


# Virtual class responsible for fetching diff tests.
# All functions with name starting with `test_` have to return
# DiffTestBlueprint and function name is a test name. All DiffTestModules have
# to be included in `test/diff_tests/trace_processor/include_index.py`.
# `fetch_diff_test` function should not be overwritten.
class TestSuite:

  def __init__(self, include_index_dir: str, dir_name: str,
               class_name: str) -> None:
    self.dir_name = dir_name
    self.index_dir = os.path.join(include_index_dir, dir_name)
    self.class_name = class_name

  def __test_name(self, method_name):
    return f"{self.class_name}:{method_name.split('test_',1)[1]}"

  def fetch(self) -> List['TestCase']:
    attrs = (getattr(self, name) for name in dir(self))
    methods = [attr for attr in attrs if inspect.ismethod(attr)]
    return [
        TestCase(self.__test_name(method.__name__), method(), self.index_dir)
        for method in methods
        if method.__name__.startswith('test_')
    ]

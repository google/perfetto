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
from typing import Dict, List, Union
from enum import Enum


@dataclass
class Path:
  filename: str


# Blueprint for running the diff test. 'query' is being run over data from the
# 'trace 'and result will be compared to the 'out. Each test (function in class
# inheriting from DiffTestModule) returns a DiffTestBlueprint.
@dataclass
class DiffTestBlueprint:
  trace: Union[str, 'Path']
  query: Union[str, 'Path']
  out: Union[str, 'Path']

  def is_trace_file(self):
    return isinstance(self.trace, Path)

  def is_query_file(self):
    return isinstance(self.query, Path)

  def is_out_file(self):
    return isinstance(self.out, Path)


# Description of a diff test. Created in `fetch_diff_tests()` in
# DiffTestModule: each test (function starting with `test_`) returns
# DiffTestBlueprint and function name is a DiffTest name. Used by diff test
# script.
class DiffTest:

  class TestType(Enum):
    QUERY = 1
    METRIC = 2

  def __init__(self, name: str, blueprint: DiffTestBlueprint,
               index_dir: str) -> None:
    self.name = name
    self.blueprint = blueprint

    if blueprint.is_query_file():
      if blueprint.query.filename.endswith('.sql'):
        self.type = DiffTest.TestType.QUERY
        self.query_path = os.path.abspath(
            os.path.join(index_dir, blueprint.query.filename))
      else:
        self.type = DiffTest.TestType.METRIC
        self.query_path = blueprint.query.filename
    else:
      self.type = DiffTest.TestType.METRIC
      self.query_path = None

    if blueprint.is_trace_file():
      self.trace_path = os.path.abspath(
          os.path.join(index_dir, blueprint.trace.filename))
    else:
      self.trace_path = None

    if blueprint.is_out_file():
      self.expected_path = os.path.abspath(
          os.path.join(index_dir, blueprint.out.filename))
    else:
      self.expected_path = None


# Virtual class responsible for fetching diff tests.
# All functions with name starting with `test_` have to return
# DiffTestBlueprint and function name is a test name. All DiffTestModules have
# to be included in `test/trace_processor/include_index.py`.
# `fetch_diff_test` function should not be overwritten.
class DiffTestModule:

  def __init__(
      self,
      include_index_dir,
      dir_name,
  ) -> None:
    self.dir_name = dir_name
    self.index_dir = os.path.join(include_index_dir, dir_name)

  def fetch_diff_tests(self) -> List['DiffTest']:
    attrs = (getattr(self, name) for name in dir(self))
    methods = [attr for attr in attrs if inspect.ismethod(attr)]
    return [
        DiffTest(f"{self.dir_name}:{method.__name__}", method(), self.index_dir)
        for method in methods
        if method.__name__.startswith('test_')
    ]

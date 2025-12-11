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
import re
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Tuple, Union

from google.protobuf import text_format


@dataclass
class Path:
  """Represents a path to a file."""
  filename: str


@dataclass
class DataPath(Path):
  """Represents a path to a file in the test data directory."""
  filename: str


@dataclass
class Metric:
  """Represents a metric to be run."""
  name: str


@dataclass
class MetricV2SpecTextproto:
  """Represents a Metric v2 specification in textproto format."""
  contents: str


@dataclass
class Json:
  """Represents a JSON string."""
  contents: str


@dataclass
class Csv:
  """Represents a CSV string."""
  contents: str


@dataclass
class RawText:
  """Represents a raw text string."""
  contents: str


@dataclass
class TextProto:
  """Represents a textproto string."""
  contents: str


@dataclass
class BinaryProto:
  """Represents a binary proto message."""
  message_type: str
  contents: str
  # Comparing protos is tricky. For example, repeated fields might be written in
  # any order. To help with that you can specify a `post_processing` function
  # that will be called with the actual proto message object before converting
  # it to text representation and doing the comparison with `contents`. This
  # gives us a chance to e.g. sort messages in a repeated field.
  post_processing: Callable = text_format.MessageToString


@dataclass
class SimpleperfProto:
  """Represents a simpleperf_proto binary file with inline generation."""
  records: List[str]  # List of textproto strings for Record messages


@dataclass
class PprofTextproto:
  """Represents a pprof file in textproto format."""
  contents: str


@dataclass
class Systrace:
  """Represents a systrace file in string format."""
  contents: str


class TraceInjector:
  '''Injects fields into trace packets before test starts.

  TraceInjector can be used within a DiffTestBlueprint to selectively inject
  fields to trace packets containing specific data types. For example:

    DiffTestBlueprint(
        trace=...,
        trace_modifier=TraceInjector('ftrace_events',
                                     'sys_stats',
                                     'process_tree',
                                     {'machine_id': 1001},
                                     trusted_uid=123)
        query=...,
        out=...)

  packet_data_types: Data types to target for injection ('ftrace_events',
  'sys_stats', 'process_tree')
  injected_fields: Fields and their values to inject into matching packets
  ({'machine_id': 1001}, trusted_uid=123).
  '''

  def __init__(self, packet_data_types: List[str], injected_fields: Dict[str,
                                                                         Any]):
    self.packet_data_types = packet_data_types
    self.injected_fields = injected_fields

  def inject(self, proto: Any):
    for p in proto.packet:
      for f in self.packet_data_types:
        if p.HasField(f):
          for k, v, in self.injected_fields.items():
            setattr(p, k, v)
          continue


@dataclass
class DiffTestBlueprint:
  """Blueprint for running the diff test.

  'query' is being run over data from the 'trace 'and result will be compared
  to the 'out. Each test (function in class inheriting from TestSuite) returns
  a DiffTestBlueprint.
  """

  trace: Union[Path, DataPath, Json, Systrace, TextProto, RawText]
  query: Union[str, Path, DataPath, Metric, MetricV2SpecTextproto]
  out: Union[Path, DataPath, Json, Csv, TextProto, BinaryProto]
  trace_modifier: Union[TraceInjector, None] = None
  register_files_dir: Optional[DataPath] = None
  # If set, this test will only be run if all of these module_dependencies are enabled.
  module_dependencies: Optional[List[str]] = None
  index_dir: str = ''
  test_data_dir: str = ''

  def is_trace_file(self):
    return isinstance(self.trace, Path)

  def is_trace_textproto(self):
    return isinstance(self.trace, TextProto)

  def is_trace_json(self):
    return isinstance(self.trace, Json)

  def is_trace_systrace(self):
    return isinstance(self.trace, Systrace)

  def is_trace_rawtext(self):
    return isinstance(self.trace, RawText)

  def is_trace_simpleperf_proto(self):
    return isinstance(self.trace, SimpleperfProto)

  def is_query_file(self):
    return isinstance(self.query, Path)

  def is_metric(self):
    return isinstance(self.query, Metric)

  def is_metric_v2(self):
    return isinstance(self.query, MetricV2SpecTextproto)

  def is_out_file(self):
    return isinstance(self.out, Path)

  def is_out_json(self):
    return isinstance(self.out, Json)

  def is_out_texproto(self):
    return isinstance(self.out, TextProto)

  def is_out_binaryproto(self):
    return isinstance(self.out, BinaryProto)

  def is_out_csv(self):
    return isinstance(self.out, Csv)


def removeprefix(s: str, prefix: str):
  """str.removeprefix is available in Python 3.9+, but the Perfetto CI runs on
  older versions."""
  if s.startswith(prefix):
    return s[len(prefix):]
  return s


class TestSuite:
  """Virtual class responsible for fetching diff tests.

  All functions with name starting with `test_` have to return
  DiffTestBlueprint and function name is a test name. All DiffTestModules have
  to be included in `test/diff_tests/trace_processor/include_index.py`. `fetch`
  function should not be overwritten.
  """

  def __init__(
      self,
      include_index_dir: str,
      test_data_dir: str = os.path.abspath(
          os.path.join(__file__, '../../../../test/data'))
  ) -> None:
    # The last path in the module is the module name itself, which is not a part
    # of the directory. The first part is "diff_tests.", but it is not present
    # when running difftests from Chrome, so we strip it conditionally.
    self.dir_name = '/'.join(
        removeprefix(self.__class__.__module__, 'diff_tests.').split('.')[:-1])
    self.index_dir = os.path.join(include_index_dir, self.dir_name)
    self.class_name = self.__class__.__name__
    self.test_data_dir = test_data_dir

  def __test_name(self, method_name: str) -> str:
    return f"{self.class_name}:{method_name.split('test_',1)[1]}"

  def fetch(self) -> List[Tuple[str, 'DiffTestBlueprint']]:
    attrs = (getattr(self, name) for name in dir(self))
    methods = [attr for attr in attrs if inspect.ismethod(attr)]
    tests = []
    for method in methods:
      if method.__name__.startswith('test_'):
        blueprint = method()
        blueprint.index_dir = self.index_dir
        blueprint.test_data_dir = self.test_data_dir
        tests.append((self.__test_name(method.__name__), blueprint))
    return tests


def PrintProfileProto(profile: Any) -> str:
  """Post processing function for pprof profiles."""
  locations = {l.id: l for l in profile.location}
  functions = {f.id: f for f in profile.function}
  samples = []
  # Strips trailing annotations like (.__uniq.1657) from the function name.
  filter_fname = lambda x: re.sub(r' [(\[].*?uniq.*?[)\]]$', '', x)
  for s in profile.sample:
    stack = []
    for location in [locations[id] for id in s.location_id]:
      for function in [functions[l.function_id] for l in location.line]:
        stack.append("{name} ({address})".format(
            name=filter_fname(profile.string_table[function.name]),
            address=hex(location.address)))
      if len(location.line) == 0:
        stack.append("({address})".format(address=hex(location.address)))
    samples.append('Sample:\nValues: {values}\nStack:\n{stack}'.format(
        values=', '.join(map(str, s.value)), stack='\n'.join(stack)))
  return '\n\n'.join(sorted(samples)) + '\n'

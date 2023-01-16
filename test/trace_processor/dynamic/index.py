#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Dynamic(DiffTestModule):

  def test_ancestor_slice(self):
    return DiffTestBlueprint(
        trace=Path('relationship_tables.textproto'),
        query=Path('ancestor_slice_test.sql'),
        out=Path('ancestor_slice.out'))

  def test_descendant_slice(self):
    return DiffTestBlueprint(
        trace=Path('relationship_tables.textproto'),
        query=Path('descendant_slice_test.sql'),
        out=Path('descendant_slice.out'))

  def test_ancestor_slice_by_stack(self):
    return DiffTestBlueprint(
        trace=Path('slice_stacks.textproto'),
        query=Path('ancestor_slice_by_stack_test.sql'),
        out=Csv("""
"ts","name"
1000,"event_depth_0"
2000,"event_depth_1"
8000,"event_depth_0"
9000,"event_depth_1"
"""))

  def test_descendant_slice_by_stack(self):
    return DiffTestBlueprint(
        trace=Path('slice_stacks.textproto'),
        query=Path('descendant_slice_by_stack_test.sql'),
        out=Csv("""
"ts","name"
2000,"event_depth_1"
3000,"event_depth_2"
9000,"event_depth_1"
10000,"event_depth_2"
"""))

  def test_connected_flow(self):
    return DiffTestBlueprint(
        trace=Path('connected_flow_data.json'),
        query=Path('connected_flow_test.sql'),
        out=Path('connected_flow.out'))

  def test_perf_sample_sc_annotated_callstack(self):
    return DiffTestBlueprint(
        trace=Path('../../data/perf_sample_sc.pb'),
        query=Path('annotated_callstack_test.sql'),
        out=Path('perf_sample_sc_annotated_callstack.out'))

  def test_various_clocks_abs_time_str(self):
    return DiffTestBlueprint(
        trace=Path('various_clocks.textproto'),
        query=Path('abs_time_str_test.sql'),
        out=Path('various_clocks_abs_time_str.out'))

  def test_empty_abs_time_str(self):
    return DiffTestBlueprint(
        trace=Path('../common/empty.textproto'),
        query=Path('abs_time_str_test.sql'),
        out=Csv("""
"t15","t25","t35"
"[NULL]","[NULL]","[NULL]"
"""))

  def test_various_clocks_to_monotonic(self):
    return DiffTestBlueprint(
        trace=Path('various_clocks.textproto'),
        query=Path('to_monotonic_test.sql'),
        out=Csv("""
"t15","t20","t25"
15,20,25
"""))

  def test_empty_to_monotonic(self):
    return DiffTestBlueprint(
        trace=Path('../common/empty.textproto'),
        query=Path('to_monotonic_test.sql'),
        out=Csv("""
"t15","t20","t25"
"[NULL]","[NULL]","[NULL]"
"""))

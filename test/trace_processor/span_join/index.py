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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Span_join(DiffTestModule):

  def test_span_join_unordered_cols_synth_1(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_join_unordered_cols_test.sql'),
        out=Path('span_join_unordered_cols_synth_1.out'))

  def test_span_join_unordered_cols_synth_1_2(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_join_unordered_cols_reverse_test.sql'),
        out=Path('span_join_unordered_cols_synth_1.out'))

  def test_span_join_zero_negative_dur(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('span_join_zero_negative_dur_test.sql'),
        out=Path('span_join_zero_negative_dur.out'))

  def test_android_sched_and_ps_slice_span_join_b118665515(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('slice_span_join_b118665515_test.sql'),
        out=Path('android_sched_and_ps_slice_span_join_b118665515.out'))

  def test_span_join_unpartitioned_empty(self):
    return DiffTestBlueprint(
        trace=Path('../../data/android_sched_and_ps.pb'),
        query=Path('span_join_unpartitioned_empty_test.sql'),
        out=Path('span_join_unpartitioned_empty.out'))

  def test_span_outer_join(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_test.sql'),
        out=Path('span_outer_join.out'))

  def test_span_outer_join_empty(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_empty_test.sql'),
        out=Path('span_outer_join_empty.out'))

  def test_span_outer_join_unpartitioned_empty(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_unpartitioned_empty_test.sql'),
        out=Path('span_outer_join_unpartitioned_empty.out'))

  def test_span_outer_join_unpartitioned_left_empty(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_unpartitioned_left_empty_test.sql'),
        out=Path('span_outer_join_unpartitioned_left_empty.out'))

  def test_span_outer_join_unpartitioned_right_empty(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_unpartitioned_right_empty_test.sql'),
        out=Path('span_outer_join_unpartitioned_right_empty.out'))

  def test_span_outer_join_mixed(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_mixed_test.sql'),
        out=Path('span_outer_join_mixed.out'))

  def test_span_outer_join_mixed_empty(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_mixed_empty_test.sql'),
        out=Path('span_outer_join_mixed_empty.out'))

  def test_span_outer_join_mixed_left_empty(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_mixed_left_empty_test.sql'),
        out=Path('span_outer_join_mixed_left_empty.out'))

  def test_span_outer_join_mixed_left_empty_rev(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_mixed_left_empty_rev_test.sql'),
        out=Path('span_outer_join_mixed_left_empty_rev.out'))

  def test_span_outer_join_mixed_right_empty(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_mixed_right_empty_test.sql'),
        out=Path('span_outer_join_mixed_right_empty.out'))

  def test_span_outer_join_mixed_right_empty_rev(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_mixed_right_empty_rev_test.sql'),
        out=Path('span_outer_join_mixed_right_empty_rev.out'))

  def test_span_outer_join_mixed_2(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_outer_join_mixed_test.sql'),
        out=Path('span_outer_join_mixed.out'))

  def test_span_left_join(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_left_join_test.sql'),
        out=Path('span_left_join.out'))

  def test_span_left_join_unpartitioned(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_left_join_unpartitioned_test.sql'),
        out=Path('span_left_join_unpartitioned.out'))

  def test_span_left_join_left_unpartitioned(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_left_join_left_unpartitioned_test.sql'),
        out=Path('span_left_join_left_unpartitioned.out'))

  def test_span_left_join_left_partitioned(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_left_join_left_partitioned_test.sql'),
        out=Path('span_left_join_left_partitioned.out'))

  def test_span_left_join_empty_right(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_left_join_empty_right_test.sql'),
        out=Path('span_left_join_empty_right.out'))

  def test_span_left_join_unordered_android_sched_and_ps(self):
    return DiffTestBlueprint(
        trace=Path('../common/synth_1.py'),
        query=Path('span_left_join_unordered_test.sql'),
        out=Path('span_left_join_unordered_android_sched_and_ps.out'))

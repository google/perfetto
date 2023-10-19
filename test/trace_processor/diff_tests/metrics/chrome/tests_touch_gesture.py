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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ChromeTouchGesture(TestSuite):

  def test_touch_jank(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_touch_gesture_scroll.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/touch_jank.sql');

        SELECT
          touch_id,
          trace_id,
          jank,
          ts,
          dur,
          jank_budget
        FROM touch_jank;
        """,
        out=Path('touch_jank.out'))

  def test_touch_flow_event(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_touch_gesture_scroll.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/touch_flow_event.sql');

        SELECT
          trace_id,
          ts,
          dur,
          jank,
          step,
          ancestor_end,
          maybe_next_ancestor_ts,
          next_ts,
          next_trace_id,
          next_step
        FROM touch_flow_event
        ORDER BY touch_id, trace_id, ts;
        """,
        out=Path('touch_flow_event.out'))

  def test_touch_flow_event_queuing_delay(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_touch_gesture_scroll.pftrace'),
        query="""
        SELECT RUN_METRIC('chrome/touch_flow_event_queuing_delay.sql');

        SELECT
          trace_id,
          jank,
          step,
          next_step,
          ancestor_end,
          maybe_next_ancestor_ts,
          queuing_time_ns
        FROM touch_flow_event_queuing_delay
        WHERE trace_id = 6915 OR trace_id = 6911 OR trace_id = 6940
        ORDER BY trace_id, ts;
        """,
        out=Path('touch_flow_event_queuing_delay.out'))

  def test_touch_jank_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query="""
        SELECT RUN_METRIC('chrome/touch_jank.sql');

        SELECT
          touch_id,
          trace_id,
          jank,
          ts,
          dur,
          jank_budget
        FROM touch_jank;
        """,
        out=Csv("""
        "touch_id","trace_id","jank","ts","dur","jank_budget"
        87654,34577,0,0,10000000,-31333333.350000
        87654,34578,1,16000000,33000000,14666666.650000
        87654,34579,0,55000000,33000000,-8333333.350000
        """))

  def test_touch_flow_event_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query="""
        SELECT RUN_METRIC('chrome/touch_flow_event.sql');

        SELECT
          trace_id,
          ts,
          dur,
          jank,
          step,
          ancestor_end,
          maybe_next_ancestor_ts,
          next_ts,
          next_trace_id,
          next_step
        FROM touch_flow_event
        ORDER BY touch_id, trace_id, ts;
        """,
        out=Path('touch_flow_event_synth.out'))

  def test_touch_flow_event_queuing_delay_synth(self):
    return DiffTestBlueprint(
        trace=Path('touch_jank.py'),
        query="""
        SELECT RUN_METRIC('chrome/touch_flow_event_queuing_delay.sql');

        SELECT
          trace_id,
          jank,
          step,
          next_step,
          ancestor_end,
          maybe_next_ancestor_ts,
          queuing_time_ns
        FROM touch_flow_event_queuing_delay
        ORDER BY trace_id, ts;
        """,
        out=Path('touch_flow_event_queuing_delay_synth.out'))

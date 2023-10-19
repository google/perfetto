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


class ChromeScrollJankStdlib(TestSuite):

  def test_chrome_frames_with_missed_vsyncs(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_v3;

        SELECT
          cause_of_jank,
          sub_cause_of_jank,
          delay_since_last_frame,
          vsync_interval
        FROM chrome_janky_frames;
        """,
        out=Path('scroll_jank_v3.out'))

  def test_chrome_frames_with_missed_vsyncs_percentage(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_v3;

        SELECT
          delayed_frame_percentage
        FROM chrome_janky_frames_percentage;
        """,
        out=Path('scroll_jank_v3_percentage.out'))

  def test_chrome_scrolls(self):
    return DiffTestBlueprint(
        trace=Path('chrome_scroll_check.py'),
        query="""
        INCLUDE PERFETTO MODULE chrome.chrome_scrolls;

        SELECT
          id,
          ts,
          dur,
          gesture_scroll_begin_ts,
          gesture_scroll_end_ts
        FROM chrome_scrolls
        ORDER by id;
        """,
        out=Csv("""
        "id","ts","dur","gesture_scroll_begin_ts","gesture_scroll_end_ts"
        5678,0,55000000,0,45000000
        5679,60000000,40000000,60000000,90000000
        5680,80000000,30000000,80000000,100000000
        5681,120000000,70000000,120000000,"[NULL]"
        """))

  def test_chrome_scroll_intervals(self):
    return DiffTestBlueprint(
        trace=Path('chrome_scroll_check.py'),
        query="""
        INCLUDE PERFETTO MODULE chrome.chrome_scrolls;

        SELECT
          id,
          ts,
          dur
        FROM chrome_scrolling_intervals
        ORDER by id;
        """,
        out=Csv("""
        "id","ts","dur"
        1,0,55000000
        2,60000000,50000000
        3,120000000,70000000
        """))

  def test_chrome_scroll_input_offsets(self):
    return DiffTestBlueprint(
        trace=DataPath('scroll_offsets.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_offsets;

        SELECT
          scroll_update_id,
          ts,
          delta_y,
          offset_y
        FROM chrome_scroll_input_offsets
        ORDER by ts
        LIMIT 5;
        """,
        out=Csv("""
        "scroll_update_id","ts","delta_y","offset_y"
        1983,4687296612739,-36.999939,-36.999939
        1983,4687307175845,-39.000092,-76.000031
        1987,4687313206739,-35.999969,-112.000000
        1987,4687323152462,-35.000000,-147.000000
        1991,4687329240739,-28.999969,-175.999969
        """))

  def test_chrome_presented_scroll_offsets(self):
    return DiffTestBlueprint(
        trace=DataPath('scroll_offsets.pftrace'),
        query="""
        INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_offsets;

        SELECT
          scroll_update_id,
          ts,
          delta_y,
          offset_y
        FROM chrome_presented_scroll_offsets
        ORDER by ts
        LIMIT 5;
        """,
        out=Csv("""
        "scroll_update_id","ts","delta_y","offset_y"
        1983,4687296612739,"[NULL]",0
        1987,4687313206739,-50,-50
        1991,4687329240739,-50,-100
        1993,4687336155739,-81,-181
        1996,4687346164739,-66,-247
        """))

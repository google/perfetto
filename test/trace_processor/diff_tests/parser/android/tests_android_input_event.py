#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

class AndroidInputEvent(TestSuite):

  def test_key_events_table(self):
    return DiffTestBlueprint(
      trace=Path('input_event_trace.textproto'),
      query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT
          event_id, ts
        FROM
          android_key_events;
        """,
      out=Csv("""
        "event_id","ts"
        759309047,674773501245024
        894093732,674773509276111
        """))

  def test_key_events_args(self):
    return DiffTestBlueprint(
      trace=Path('input_event_trace.textproto'),
      query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT
          args.key, args.display_value
        FROM
          android_key_events AS e JOIN args ON e.arg_set_id = args.arg_set_id
        WHERE e.event_id = 894093732
        ORDER BY args.key;
        """,
      out=Csv("""
        "key","display_value"
        "action","1"
        "device_id","2"
        "display_id","-1"
        "down_time_nanos","517482680619000"
        "event_id","894093732"
        "event_time_nanos","517482832173000"
        "flags","8"
        "key_code","24"
        "meta_state","0"
        "policy_flags","1644167168"
        "repeat_count","0"
        "scan_code","115"
        "source","257"
        """))

  def test_motion_events_table(self):
    return DiffTestBlueprint(
      trace=Path('input_event_trace.textproto'),
      query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT
          event_id, ts
        FROM
          android_motion_events;
        """,
      out=Csv("""
        "event_id","ts"
        330184796,674772186549222
        1327679296,674772186549222
        557261353,674772207730130
        106022695,674772213523384
        313395000,674772222900174
        436499943,674772227946073
        """))

  def test_motion_events_args(self):
    return DiffTestBlueprint(
      trace=Path('input_event_trace.textproto'),
      query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT
          args.key, args.display_value
        FROM
          android_motion_events AS e JOIN args ON e.arg_set_id = args.arg_set_id
        WHERE e.event_id = 557261353
        ORDER BY args.key;
        """,
      out=Csv("""
        "key","display_value"
        "action","2"
        "classification","1"
        "cursor_position_x","[NULL]"
        "cursor_position_y","[NULL]"
        "device_id","4"
        "display_id","0"
        "down_time_nanos","517481507875000"
        "event_id","557261353"
        "event_time_nanos","517481533371000"
        "flags","128"
        "meta_state","0"
        "pointer[0].axis_value[0].axis","0"
        "pointer[0].axis_value[0].value","431.0"
        "pointer[0].axis_value[1].axis","1"
        "pointer[0].axis_value[1].value","624.0"
        "pointer[0].axis_value[2].axis","2"
        "pointer[0].axis_value[2].value","1.32031202316284"
        "pointer[0].axis_value[3].axis","3"
        "pointer[0].axis_value[3].value","0.0392730012536049"
        "pointer[0].axis_value[4].axis","4"
        "pointer[0].axis_value[4].value","110.0"
        "pointer[0].axis_value[5].axis","5"
        "pointer[0].axis_value[5].value","91.0"
        "pointer[0].axis_value[6].axis","6"
        "pointer[0].axis_value[6].value","110.0"
        "pointer[0].axis_value[7].axis","7"
        "pointer[0].axis_value[7].value","91.0"
        "pointer[0].axis_value[8].axis","8"
        "pointer[0].axis_value[8].value","1.12019002437592"
        "pointer[0].pointer_id","0"
        "pointer[0].tool_type","1"
        "policy_flags","1644167168"
        "source","4098"
        """))

  def test_dispatch_table(self):
    return DiffTestBlueprint(
      trace=Path('input_event_trace.textproto'),
      query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT
          id, event_id, vsync_id, window_id
        FROM
          android_input_event_dispatch;
        """,
      out=Csv("""
        "id","event_id","vsync_id","window_id"
        0,1327679296,89110,98
        1,330184796,89110,212
        2,330184796,89110,64
        3,330184796,89110,82
        4,330184796,89110,75
        5,330184796,0,0
        6,557261353,89110,212
        7,557261353,89110,64
        8,557261353,89110,82
        9,557261353,89110,75
        10,557261353,0,0
        11,106022695,89110,212
        12,106022695,89110,64
        13,106022695,89110,82
        14,106022695,89110,75
        15,106022695,0,0
        16,313395000,89110,212
        17,313395000,89110,64
        18,313395000,89110,82
        19,313395000,89110,75
        20,313395000,0,0
        21,436499943,89110,212
        22,436499943,89110,64
        23,436499943,89110,82
        24,436499943,89110,75
        25,436499943,0,0
        26,759309047,89110,212
        27,759309047,0,0
        28,894093732,89110,212
        29,894093732,0,0
        """))

  def test_motion_dispatch_args(self):
    return DiffTestBlueprint(
      trace=Path('input_event_trace.textproto'),
      query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT
          d.id, args.key, args.display_value
        FROM
          android_input_event_dispatch AS d JOIN args ON d.arg_set_id = args.arg_set_id
        WHERE d.event_id = 330184796
        ORDER BY d.id, args.key;
        """,
      out=Csv("""
        "id","key","display_value"
        1,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        1,"dispatched_pointer[0].axis_value_in_window[0].value","1936.0"
        1,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        1,"dispatched_pointer[0].axis_value_in_window[1].value","431.0"
        1,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        1,"dispatched_pointer[0].axis_value_in_window[2].value","-0.450637996196747"
        1,"dispatched_pointer[0].pointer_id","0"
        1,"dispatched_pointer[0].x_in_display","1936.0"
        1,"dispatched_pointer[0].y_in_display","431.0"
        1,"event_id","330184796"
        1,"resolved_flags","128"
        1,"vsync_id","89110"
        1,"window_id","212"
        2,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        2,"dispatched_pointer[0].axis_value_in_window[0].value","1876.36328125"
        2,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        2,"dispatched_pointer[0].axis_value_in_window[1].value","464.5458984375"
        2,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        2,"dispatched_pointer[0].axis_value_in_window[2].value","-0.450681000947952"
        2,"dispatched_pointer[0].pointer_id","0"
        2,"dispatched_pointer[0].x_in_display","1936.0"
        2,"dispatched_pointer[0].y_in_display","431.0"
        2,"event_id","330184796"
        2,"resolved_flags","131"
        2,"vsync_id","89110"
        2,"window_id","64"
        3,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        3,"dispatched_pointer[0].axis_value_in_window[0].value","1936.0"
        3,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        3,"dispatched_pointer[0].axis_value_in_window[1].value","431.0"
        3,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        3,"dispatched_pointer[0].axis_value_in_window[2].value","-0.450637996196747"
        3,"dispatched_pointer[0].pointer_id","0"
        3,"dispatched_pointer[0].x_in_display","1936.0"
        3,"dispatched_pointer[0].y_in_display","431.0"
        3,"event_id","330184796"
        3,"resolved_flags","128"
        3,"vsync_id","89110"
        3,"window_id","82"
        4,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        4,"dispatched_pointer[0].axis_value_in_window[0].value","1936.0"
        4,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        4,"dispatched_pointer[0].axis_value_in_window[1].value","431.0"
        4,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        4,"dispatched_pointer[0].axis_value_in_window[2].value","-0.450637996196747"
        4,"dispatched_pointer[0].pointer_id","0"
        4,"dispatched_pointer[0].x_in_display","1936.0"
        4,"dispatched_pointer[0].y_in_display","431.0"
        4,"event_id","330184796"
        4,"resolved_flags","128"
        4,"vsync_id","89110"
        4,"window_id","75"
        5,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        5,"dispatched_pointer[0].axis_value_in_window[0].value","1936.0"
        5,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        5,"dispatched_pointer[0].axis_value_in_window[1].value","431.0"
        5,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        5,"dispatched_pointer[0].axis_value_in_window[2].value","-0.450637996196747"
        5,"dispatched_pointer[0].pointer_id","0"
        5,"dispatched_pointer[0].x_in_display","1936.0"
        5,"dispatched_pointer[0].y_in_display","431.0"
        5,"event_id","330184796"
        5,"resolved_flags","128"
        5,"vsync_id","0"
        5,"window_id","0"
        """))

  def test_key_dispatch_args(self):
    return DiffTestBlueprint(
      trace=Path('input_event_trace.textproto'),
      query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT
          d.id, args.key, args.display_value
        FROM
          android_input_event_dispatch AS d JOIN args ON d.arg_set_id = args.arg_set_id
        WHERE d.event_id = 759309047
        ORDER BY d.id, args.key;
        """,
      out=Csv("""
        "id","key","display_value"
        26,"event_id","759309047"
        26,"resolved_flags","8"
        26,"vsync_id","89110"
        26,"window_id","212"
        27,"event_id","759309047"
        27,"resolved_flags","8"
        27,"vsync_id","0"
        27,"window_id","0"
        """))

  def test_tables_have_raw_protos(self):
    return DiffTestBlueprint(
        trace=Path('input_event_trace.textproto'),
        query="""
        INCLUDE PERFETTO MODULE android.input;
        SELECT COUNT(*) FROM android_key_events
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        UNION ALL
        SELECT COUNT(*) FROM android_motion_events
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        UNION ALL
        SELECT COUNT(*) FROM android_input_event_dispatch
        WHERE base64_proto IS NOT NULL AND base64_proto_id IS NOT NULL
        """,
        out=Csv("""
        "COUNT(*)"
        2
        6
        30
        """))

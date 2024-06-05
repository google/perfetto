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
        324105269,64182660299000
        60594531,64182816340000
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
        WHERE e.event_id = 60594531
        ORDER BY args.key;
        """,
      out=Csv("""
        "key","display_value"
        "action","1"
        "device_id","2"
        "display_id","-1"
        "down_time_nanos","64182660299000"
        "event_id","60594531"
        "event_time_nanos","64182816340000"
        "flags","8"
        "key_code","25"
        "meta_state","0"
        "policy_flags","1644167168"
        "repeat_count","0"
        "scan_code","114"
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
        104114844,64179212500000
        1141228253,64179212500000
        843076721,64179262122000
        744146837,64179268985000
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
        WHERE e.event_id = 1141228253
        ORDER BY args.key;
        """,
      out=Csv("""
        "key","display_value"
        "action","4"
        "classification","0"
        "cursor_position_x","[NULL]"
        "cursor_position_y","[NULL]"
        "device_id","4"
        "display_id","0"
        "down_time_nanos","64179212500000"
        "event_id","1141228253"
        "event_time_nanos","64179212500000"
        "flags","0"
        "meta_state","0"
        "pointer[0].axis_value[0].axis","0"
        "pointer[0].axis_value[0].value","580.0"
        "pointer[0].axis_value[1].axis","1"
        "pointer[0].axis_value[1].value","798.0"
        "pointer[0].axis_value[2].axis","2"
        "pointer[0].axis_value[2].value","1.00390601158142"
        "pointer[0].axis_value[3].axis","3"
        "pointer[0].axis_value[3].value","0.0339980013668537"
        "pointer[0].axis_value[4].axis","4"
        "pointer[0].axis_value[4].value","92.0"
        "pointer[0].axis_value[5].axis","5"
        "pointer[0].axis_value[5].value","82.0"
        "pointer[0].axis_value[6].axis","6"
        "pointer[0].axis_value[6].value","92.0"
        "pointer[0].axis_value[7].axis","7"
        "pointer[0].axis_value[7].value","82.0"
        "pointer[0].axis_value[8].axis","8"
        "pointer[0].axis_value[8].value","0.983282029628754"
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
        0,1141228253,182239,105
        1,104114844,182239,181
        2,104114844,182239,58
        3,104114844,182239,76
        4,104114844,182239,68
        5,104114844,0,0
        6,843076721,182239,181
        7,843076721,182239,58
        8,843076721,182239,76
        9,843076721,182239,68
        10,843076721,0,0
        11,744146837,182239,181
        12,744146837,182239,58
        13,744146837,182239,76
        14,744146837,182239,68
        15,744146837,0,0
        16,324105269,182239,181
        17,324105269,0,0
        18,60594531,182239,181
        19,60594531,0,0
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
        WHERE d.event_id = 104114844
        ORDER BY d.id, args.key;
        """,
      out=Csv("""
        "id","key","display_value"
        1,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        1,"dispatched_pointer[0].axis_value_in_window[0].value","1762.0"
        1,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        1,"dispatched_pointer[0].axis_value_in_window[1].value","580.0"
        1,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        1,"dispatched_pointer[0].axis_value_in_window[2].value","2.5541570186615"
        1,"dispatched_pointer[0].pointer_id","0"
        1,"dispatched_pointer[0].x_in_display","1762.0"
        1,"dispatched_pointer[0].y_in_display","580.0"
        1,"event_id","104114844"
        1,"resolved_flags","0"
        1,"vsync_id","182239"
        1,"window_id","181"
        2,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        2,"dispatched_pointer[0].axis_value_in_window[0].value","1718.181640625"
        2,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        2,"dispatched_pointer[0].axis_value_in_window[1].value","600.0"
        2,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        2,"dispatched_pointer[0].axis_value_in_window[2].value","2.55407404899597"
        2,"dispatched_pointer[0].pointer_id","0"
        2,"dispatched_pointer[0].x_in_display","1762.0"
        2,"dispatched_pointer[0].y_in_display","580.0"
        2,"event_id","104114844"
        2,"resolved_flags","3"
        2,"vsync_id","182239"
        2,"window_id","58"
        3,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        3,"dispatched_pointer[0].axis_value_in_window[0].value","1762.0"
        3,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        3,"dispatched_pointer[0].axis_value_in_window[1].value","580.0"
        3,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        3,"dispatched_pointer[0].axis_value_in_window[2].value","2.5541570186615"
        3,"dispatched_pointer[0].pointer_id","0"
        3,"dispatched_pointer[0].x_in_display","1762.0"
        3,"dispatched_pointer[0].y_in_display","580.0"
        3,"event_id","104114844"
        3,"resolved_flags","0"
        3,"vsync_id","182239"
        3,"window_id","76"
        4,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        4,"dispatched_pointer[0].axis_value_in_window[0].value","1762.0"
        4,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        4,"dispatched_pointer[0].axis_value_in_window[1].value","580.0"
        4,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        4,"dispatched_pointer[0].axis_value_in_window[2].value","2.5541570186615"
        4,"dispatched_pointer[0].pointer_id","0"
        4,"dispatched_pointer[0].x_in_display","1762.0"
        4,"dispatched_pointer[0].y_in_display","580.0"
        4,"event_id","104114844"
        4,"resolved_flags","0"
        4,"vsync_id","182239"
        4,"window_id","68"
        5,"dispatched_pointer[0].axis_value_in_window[0].axis","0"
        5,"dispatched_pointer[0].axis_value_in_window[0].value","1762.0"
        5,"dispatched_pointer[0].axis_value_in_window[1].axis","1"
        5,"dispatched_pointer[0].axis_value_in_window[1].value","580.0"
        5,"dispatched_pointer[0].axis_value_in_window[2].axis","8"
        5,"dispatched_pointer[0].axis_value_in_window[2].value","2.5541570186615"
        5,"dispatched_pointer[0].pointer_id","0"
        5,"dispatched_pointer[0].x_in_display","1762.0"
        5,"dispatched_pointer[0].y_in_display","580.0"
        5,"event_id","104114844"
        5,"resolved_flags","0"
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
        WHERE d.event_id = 324105269
        ORDER BY d.id, args.key;
        """,
      out=Csv("""
        "id","key","display_value"
        16,"event_id","324105269"
        16,"resolved_flags","8"
        16,"vsync_id","182239"
        16,"window_id","181"
        17,"event_id","324105269"
        17,"resolved_flags","8"
        17,"vsync_id","0"
        17,"window_id","0"
        """))

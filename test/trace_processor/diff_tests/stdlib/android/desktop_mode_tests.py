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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

class DesktopMode(TestSuite):

  def test_android_desktop_mode_windows_statsd_events(self):
    return DiffTestBlueprint(
        trace=DataPath('android_desktop_mode/single_window_add_update_remove.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        1112172132337,1115098491388,1112172132337,2926359051,22,10211
        """))

  def test_android_desktop_mode_windows_statsd_events_multiple_windows(self):
    return DiffTestBlueprint(
        trace=DataPath('android_desktop_mode/multiple_windows_add_update_remove.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        1340951146935,1347096280320,1340951146935,6145133385,24,10211
        1342507511641,1345461733688,1342507511641,2954222047,26,10183
                """))

  def test_android_desktop_mode_windows_statsd_events_add_no_remove(self):
    return DiffTestBlueprint(
        trace=DataPath('android_desktop_mode/single_window_add_update_no_remove.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        1552558346094,"[NULL]",1552558346094,1620521485,27,10211
        """))

  def test_android_desktop_mode_windows_statsd_events_no_add_update_remove(self):
    return DiffTestBlueprint(
        trace=DataPath('android_desktop_mode/single_window_no_add_update_remove.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        "[NULL]",1696520389866,1695387563286,1132826580,29,10211
        """))

  def test_android_desktop_mode_windows_statsd_events_only_update(self):
    return DiffTestBlueprint(
        trace=DataPath('android_desktop_mode/single_window_only_update.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        "[NULL]","[NULL]",1852548597746,3663403770,31,10211
        """))

  def test_android_desktop_mode_windows_statsd_events_multiple_windows_update_only(self):
    return DiffTestBlueprint(
        trace=DataPath('android_desktop_mode/multiple_window_only_update.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        "[NULL]","[NULL]",2137135290268,4737314089,33,10211
        "[NULL]","[NULL]",2137135290268,4737314089,35,10183
        """))

  def test_android_desktop_mode_windows_statsd_events_multiple_windows_same_instance_new_session(self):
    return DiffTestBlueprint(
        trace=DataPath('android_desktop_mode/session_with_same_instance_id.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        8936818061228,8963638163943,8936818061228,26820102715,1000025,1110217
        8966480744267,"[NULL]",8966480744267,3596089886,1000025,1110217
        8966481546961,"[NULL]",8966481546961,3595287192,1000028,1110329
        """))


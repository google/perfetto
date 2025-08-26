#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class DesktopMode(TestSuite):

  def test_android_desktop_mode_windows_statsd_events(self):
    return DiffTestBlueprint(
        trace=DataPath(
            'android_desktop_mode/single_window_add_update_remove.pb'),
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
        trace=DataPath(
            'android_desktop_mode/multiple_windows_add_update_remove.pb'),
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
        trace=DataPath(
            'android_desktop_mode/single_window_add_update_no_remove.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        1552558346094,"[NULL]",1552558346094,1620521485,27,10211
        """))

  def test_android_desktop_mode_windows_statsd_events_no_add_update_remove(
      self):
    return DiffTestBlueprint(
        trace=DataPath(
            'android_desktop_mode/single_window_no_add_update_remove.pb'),
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

  def test_android_desktop_mode_windows_statsd_events_multiple_windows_update_only(
      self):
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

  def test_android_desktop_mode_windows_statsd_events_multiple_windows_same_instance_new_session(
      self):
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

  def test_android_desktop_mode_windows_statsd_events_reset_events(self):
    return DiffTestBlueprint(
        trace=DataPath('android_desktop_mode/task_update_reset_events.pb'),
        query="""
          INCLUDE PERFETTO MODULE android.desktop_mode;
          SELECT * FROM android_desktop_mode_windows;
          """,
        out=Csv("""
        "raw_add_ts","raw_remove_ts","ts","dur","instance_id","uid"
        84164708379314,84188981730278,84164708379314,24273350964,1000054,1010210
        84182861720998,84184961957530,84182861720998,2100236532,1000056,1010197
        84190656246474,"[NULL]",84190656246474,663853800,1000054,1010210
        84190656724094,"[NULL]",84190656724094,663376180,1000058,1010222
        84199757126076,84223637163807,84199757126076,23880037731,1000054,1010210
        84199757350156,84223637751006,84199757350156,23880400850,1000058,1010222
        84204112441752,84208226333681,84204112441752,4113891929,1000062,1010226
        84226052369131,84241418229490,84226052369131,15365860359,1000054,1010210
        84226054551300,84241419846189,84226054551300,15365294889,1000058,1010222
        84248341935751,"[NULL]",84248341935751,407771770,1000054,1010210
        84248342227662,"[NULL]",84248342227662,407479859,1000058,1010222
        84253290279911,84292320102806,84253290279911,39029822895,1000054,1010210
        84253290608646,84292320892072,84253290608646,39030283426,1000058,1010222
        84294338295719,84856791435520,84294338295719,562453139801,1000054,1010210
        84294339318912,84859506938084,84294339318912,565167619172,1000058,1010222
        84304813959094,84858368194309,84304813959094,553554235215,1000069,1010211
        84335762434858,84852636055309,84335762434858,516873620451,1000070,1010297
        84357340686822,84378879321354,84357340686822,21538634532,1000071,1010225
        84361592237320,84363638663184,84361592237320,2046425864,1000072,1010317
        84370549790954,84853627568534,84370549790954,483077777580,1000074,1010317
        84575862070921,84643498791291,84575862070921,67636720370,1000075,1001000
        84619864755554,84638100287708,84619864755554,18235532154,1000076,1010235
        84782344478655,84804145589506,84782344478655,21801110851,1000077,1010238
        """))

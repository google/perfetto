# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto


class AppWakelocks(TestSuite):

  def test_app_wakelocks_sdk_priority(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          track_event {
            type: 1 # TYPE_SLICE_BEGIN
            track_uuid: 1
            name: "my_wakelock"
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 5000
          track_event {
            type: 2 # TYPE_SLICE_END
            track_uuid: 1
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 1
            name: "app_wakelock_events"
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.app_wakelocks;

        SELECT ts, dur, name FROM android_app_wakelocks;
        """,
        out=Csv("""
        "ts","dur","name"
        1000,4000,"my_wakelock"
        """))

  def test_app_wakelocks_batterystats_fallback(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1000
              pid: 1
              print {
                buf: "N|1000|battery_stats.longwake|+longwake=10001:\"my_wakelock\"\n"
              }
            }
            event {
              timestamp: 5000
              pid: 1
              print {
                buf: "N|1000|battery_stats.longwake|-longwake=10001:\"my_wakelock\"\n"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.app_wakelocks;

        SELECT ts, dur, name, uid FROM android_app_wakelocks;
        """,
        out=Csv("""
        "ts","dur","name","uid"
        1000,4000,"my_wakelock",10001
        """))

  def test_app_wakelocks_both_sources_present(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          track_event {
            type: 1 # TYPE_SLICE_BEGIN
            track_uuid: 1
            name: "my_wakelock"
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 5000
          track_event {
            type: 2 # TYPE_SLICE_END
            track_uuid: 1
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 1
            name: "app_wakelock_events"
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 2000
              pid: 1
              print {
                buf: "N|1000|battery_stats.longwake|+longwake=10001:\"my_battery_wakelock\"\n"
              }
            }
            event {
              timestamp: 6000
              pid: 1
              print {
                buf: "N|1000|battery_stats.longwake|-longwake=10001:\"my_battery_wakelock\"\n"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.app_wakelocks;

        SELECT ts, dur, name FROM android_app_wakelocks;
        """,
        out=Csv("""
        "ts","dur","name"
        1000,4000,"my_wakelock"
        """))

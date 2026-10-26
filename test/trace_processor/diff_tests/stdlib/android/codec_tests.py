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


class Codec(TestSuite):

  def test_codec_events(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 1
            name: "codec.track.state.c2.google.av1.decoder.1"
            process { pid: 1000 }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          track_event { track_uuid: 1 name: "event=Configured metadata=inputFormat info=detail pid=2000 uid=10001 render=true intervalMs=10 count=5 ctr=100" type: 3 }
          timestamp: 1000
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.codec;
        SELECT ts, track_name, track_event_type, codec_name, unique_no, event, metadata, info, pid, uid, render, interval_ms, count, ctr
        FROM android_codec_events;
        """,
        out=Csv("""
        "ts","track_name","track_event_type","codec_name","unique_no","event","metadata","info","pid","uid","render","interval_ms","count","ctr"
        1000,"codec.track.state.c2.google.av1.decoder.1","state","c2.google.av1.decoder","1","Configured","inputFormat","detail",2000,10001,"true",10,5,100
        """))

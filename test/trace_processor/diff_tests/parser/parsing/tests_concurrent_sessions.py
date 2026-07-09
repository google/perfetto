#!/usr/bin/env python3
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ParsingConcurrentSessions(TestSuite):
  # ConcurrentSessionEvent packets become one state track per session (shown
  # by the UI under System > Concurrent tracing sessions). STATE_DISABLED
  # closes the track (a disabled session never becomes active again).
  # consumer_uid and num_data_sources become args.
  def test_concurrent_session_events_become_state_tracks(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 400
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_CONFIGURED
            session_name: "session_a"
            session_id: 1
            consumer_uid: 10000
            num_data_sources: 3
          }
        }
        packet {
          timestamp: 500
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_STARTED
            session_name: "session_a"
            session_id: 1
            consumer_uid: 10000
            num_data_sources: 3
          }
        }
        packet {
          timestamp: 1000
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_STARTED
            session_name: "session_b"
            session_id: 2
            consumer_uid: 10001
            num_data_sources: 1
          }
        }
        packet {
          timestamp: 3000
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_DISABLED
            session_name: "session_b"
            session_id: 2
            consumer_uid: 10001
            num_data_sources: 1
          }
        }
        packet {
          timestamp: 4000
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_DISABLED
            session_name: "session_a"
            session_id: 1
            consumer_uid: 10000
            num_data_sources: 3
          }
        }
        """),
        query="""
          SELECT
            t.name AS track_name,
            s.ts,
            s.dur,
            s.value,
            extract_arg(s.arg_set_id, 'consumer_uid') AS consumer_uid,
            extract_arg(s.arg_set_id, 'num_data_sources') AS num_data_sources
          FROM state s
          JOIN track t ON s.track_id = t.id
          WHERE t.type = 'concurrent_tracing_sessions'
          ORDER BY t.name, s.ts;
        """,
        out=Csv("""
        "track_name","ts","dur","value","consumer_uid","num_data_sources"
        "session_a",400,100,"CONFIGURED",10000,3
        "session_a",500,3500,"STARTED",10000,3
        "session_b",1000,2000,"STARTED",10001,1
        """))

  # Tracks are keyed by session_id, so overlapping unnamed sessions get their
  # own track, named "Session <id>". Cloned sessions (born in
  # CLONED_READ_ONLY) get a " (clone)" suffix. STATE_DISABLED closes each
  # track.
  def test_concurrent_unnamed_sessions_paired_by_id(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_STARTED
            session_id: 1
          }
        }
        packet {
          timestamp: 2000
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_STARTED
            session_id: 2
          }
        }
        packet {
          timestamp: 3000
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_DISABLING_WAITING_STOP_ACKS
            session_id: 1
          }
        }
        packet {
          timestamp: 3500
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_DISABLED
            session_id: 1
          }
        }
        packet {
          timestamp: 4000
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_CLONED_READ_ONLY
            session_name: "snapshot"
            session_id: 3
          }
        }
        packet {
          timestamp: 4500
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_DISABLED
            session_name: "snapshot"
            session_id: 3
          }
        }
        packet {
          timestamp: 5000
          trusted_packet_sequence_id: 1
          concurrent_session_event {
            state: STATE_DISABLED
            session_id: 2
          }
        }
        """),
        query="""
          SELECT t.name AS track_name, s.ts, s.dur, s.value
          FROM state s
          JOIN track t ON s.track_id = t.id
          WHERE t.type = 'concurrent_tracing_sessions'
          ORDER BY t.name, s.ts;
        """,
        out=Csv("""
        "track_name","ts","dur","value"
        "Session 1",1000,2000,"STARTED"
        "Session 1",3000,500,"DISABLING_WAITING_STOP_ACKS"
        "Session 2",2000,3000,"STARTED"
        "snapshot (clone)",4000,500,"CLONED_READ_ONLY"
        """))

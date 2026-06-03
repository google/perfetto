#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class Viz(TestSuite):
  chronological_trace = TextProto(r"""
        packet {
          track_descriptor {
            uuid: 1
            name: "Root Chronological"
            child_ordering: 2
          }
        }
        packet {
          track_descriptor {
            uuid: 11
            name: "A"
            parent_uuid: 1
          }
        }
        packet {
          timestamp: 220
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 11
            name: "A1"
          }
        }
        packet {
          timestamp: 230
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 11
          }
        }
        packet {
          track_descriptor {
            uuid: 12
            name: "B"
            parent_uuid: 1
          }
        }
        packet {
          timestamp: 210
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 12
            name: "B"
          }
        }
        packet {
          timestamp: 240
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 12
          }
        }
        """)

  explicit_trace = TextProto(r"""
        packet {
          track_descriptor {
            uuid: 2
            name: "Root Explicit"
            child_ordering: 3
          }
        }
        packet {
          track_descriptor {
            uuid: 110
            name: "B"
            parent_uuid: 2
            sibling_order_rank: 1
          }
        }
        packet {
          track_descriptor {
            uuid: 120
            name: "A"
            parent_uuid: 2
            sibling_order_rank: 100
          }
        }
        packet {
          track_descriptor {
            uuid: 130
            name: "C"
            parent_uuid: 2
            sibling_order_rank: -100
          }
        }
        packet {
          timestamp: 220
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 110
            name: "1"
          }
        }
        packet {
          timestamp: 230
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 110
          }
        }
        packet {
          timestamp: 230
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 120
            name: "2"
          }
        }
        packet {
          timestamp: 240
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 120
          }
        }
        packet {
          timestamp: 225
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 130
            name: "3"
          }
        }
        packet {
          timestamp: 235
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 130
          }
        }
          """)

  lexicographic_trace = TextProto(r"""
        packet {
          track_descriptor {
            uuid: 3
            name: "Root Lexicographic"
            child_ordering: 1
          }
        }
        packet {
          track_descriptor {
            uuid: 1100
            name: "B"
            parent_uuid: 3
          }
        }
        packet {
          track_descriptor {
            uuid: 1200
            name: "A"
            parent_uuid: 3
          }
        }
        packet {
          track_descriptor {
            uuid: 1300
            name: "C"
            parent_uuid: 3
          }
        }
        packet {
          timestamp: 220
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 1100
            name: "A1"
          }
        }
        packet {
          timestamp: 230
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 1100
          }
        }
        packet {
          timestamp: 210
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 1200
            name: "B1"
          }
        }
        packet {
          timestamp: 300
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 1200
          }
        }
        packet {
          timestamp: 350
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 1300
            name: "C1"
          }
        }
        packet {
          timestamp: 400
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 1300
          }
        }
        """)

  all_ordering_trace = TextProto(f"""{chronological_trace.contents}
      {explicit_trace.contents}
      {lexicographic_trace.contents}""")

  def test_track_event_tracks_chronological(self):
    return DiffTestBlueprint(
        trace=self.chronological_trace,
        query="""
        SELECT
          t.name,
          p.name as parent_name,
          EXTRACT_ARG(t.source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(t.source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track t
        LEFT JOIN track p ON t.parent_id = p.id
        ORDER BY p.name, t.name;
        """,
        out=Csv("""
        "name","parent_name","ordering","rank"
        "Root Chronological","[NULL]","chronological","[NULL]"
        "A","Root Chronological","[NULL]","[NULL]"
        "B","Root Chronological","[NULL]","[NULL]"
        """))

  def test_all_tracks_ordered_chronological(self):
    return DiffTestBlueprint(
        trace=self.chronological_trace,
        query="""
        INCLUDE PERFETTO MODULE viz.summary.track_event;
        SELECT t.name, g.order_id
        FROM _track_event_tracks_ordered_groups g
        JOIN track t on CAST(g.track_ids AS INT) = t.id
        ORDER BY t.name;
        """,
        out=Csv("""
        "name","order_id"
        "A",2
        "B",1
        "Root Chronological",1
        """))

  def test_track_event_tracks_explicit(self):
    return DiffTestBlueprint(
        trace=self.explicit_trace,
        query="""
        SELECT
          t.name,
          p.name as parent_name,
          EXTRACT_ARG(t.source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(t.source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track t
        LEFT JOIN track p ON t.parent_id = p.id
        ORDER BY p.name, t.name;
        """,
        out=Csv("""
        "name","parent_name","ordering","rank"
        "Root Explicit","[NULL]","explicit","[NULL]"
        "A","Root Explicit","[NULL]",100
        "B","Root Explicit","[NULL]",1
        "C","Root Explicit","[NULL]",-100
        """))

  def test_all_tracks_ordered_explicit(self):
    return DiffTestBlueprint(
        trace=self.explicit_trace,
        query="""
        INCLUDE PERFETTO MODULE viz.summary.track_event;
        SELECT t.name, g.order_id
        FROM _track_event_tracks_ordered_groups g
        JOIN track t on CAST(g.track_ids AS INT) = t.id
        ORDER BY t.name;
        """,
        out=Csv("""
        "name","order_id"
        "A",3
        "B",2
        "C",1
        "Root Explicit",1
        """))

  def test_track_event_tracks_lexicographic(self):
    return DiffTestBlueprint(
        trace=self.lexicographic_trace,
        query="""
        SELECT
          t.name,
          p.name as parent_name,
          EXTRACT_ARG(t.source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(t.source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track t
        LEFT JOIN track p ON t.parent_id = p.id
        ORDER BY p.name, t.name;
        """,
        out=Csv("""
        "name","parent_name","ordering","rank"
        "Root Lexicographic","[NULL]","lexicographic","[NULL]"
        "A","Root Lexicographic","[NULL]","[NULL]"
        "B","Root Lexicographic","[NULL]","[NULL]"
        "C","Root Lexicographic","[NULL]","[NULL]"
        """))

  def test_all_tracks_ordered_lexicographic(self):
    return DiffTestBlueprint(
        trace=self.lexicographic_trace,
        query="""
        INCLUDE PERFETTO MODULE viz.summary.track_event;
        SELECT t.name, g.order_id
        FROM _track_event_tracks_ordered_groups g
        JOIN track t on CAST(g.track_ids AS INT) = t.id
        ORDER BY t.name;
        """,
        out=Csv("""
        "name","order_id"
        "A",1
        "B",2
        "C",3
        "Root Lexicographic",1
        """))

  def test_track_event_tracks_all_orderings(self):
    return DiffTestBlueprint(
        trace=self.all_ordering_trace,
        query="""
        SELECT
          t.name,
          p.name as parent_name,
          EXTRACT_ARG(t.source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(t.source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track t
        LEFT JOIN track p ON t.parent_id = p.id
        ORDER BY p.name, t.name;
        """,
        out=Csv("""
        "name","parent_name","ordering","rank"
        "Root Chronological","[NULL]","chronological","[NULL]"
        "Root Explicit","[NULL]","explicit","[NULL]"
        "Root Lexicographic","[NULL]","lexicographic","[NULL]"
        "A","Root Chronological","[NULL]","[NULL]"
        "B","Root Chronological","[NULL]","[NULL]"
        "A","Root Explicit","[NULL]",100
        "B","Root Explicit","[NULL]",1
        "C","Root Explicit","[NULL]",-100
        "A","Root Lexicographic","[NULL]","[NULL]"
        "B","Root Lexicographic","[NULL]","[NULL]"
        "C","Root Lexicographic","[NULL]","[NULL]"
        """))

  def test_all_tracks_ordered_all_ordering(self):
    return DiffTestBlueprint(
        trace=self.all_ordering_trace,
        query="""
        INCLUDE PERFETTO MODULE viz.summary.track_event;
        SELECT
          t.name,
          p.name as parent_name,
          g.order_id
        FROM _track_event_tracks_ordered_groups g
        JOIN track t ON CAST(g.track_ids AS INT) = t.id
        LEFT JOIN track p ON g.parent_id = p.id
        ORDER BY p.name, t.name;
        """,
        out=Csv("""
        "name","parent_name","order_id"
        "Root Chronological","[NULL]",1
        "Root Explicit","[NULL]",2
        "Root Lexicographic","[NULL]",3
        "A","Root Chronological",2
        "B","Root Chronological",1
        "A","Root Explicit",3
        "B","Root Explicit",2
        "C","Root Explicit",1
        "A","Root Lexicographic",1
        "B","Root Lexicographic",2
        "C","Root Lexicographic",3
        """))

  def test_sanity_ordering_tracks(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks_ordering.textproto'),
        query="""
        SELECT
          t.name,
          p.name as parent_name,
          EXTRACT_ARG(t.source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(t.source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track t
        LEFT JOIN track p ON t.parent_id = p.id
        ORDER BY p.name, t.name;
        """,
        out=Csv("""
          "name","parent_name","ordering","rank"
          "chronological_parent","[NULL]","chronological","[NULL]"
          "explicit_parent","[NULL]","explicit",-10
          "lexicographic_parent","[NULL]","lexicographic","[NULL]"
          "chrono","chronological_parent","[NULL]","[NULL]"
          "chrono1","chronological_parent","[NULL]","[NULL]"
          "chrono2","chronological_parent","[NULL]","[NULL]"
          "explicit_child:-5 z-index","explicit_parent","[NULL]",-5
          "explicit_child:5 z-index","explicit_parent","[NULL]",5
          "explicit_child:no z-index","explicit_parent","[NULL]","[NULL]"
          "a","lexicographic_parent","[NULL]","[NULL]"
          "ab","lexicographic_parent","[NULL]","[NULL]"
          "b","lexicographic_parent","[NULL]","[NULL]"
          "event_for_unnamed_child","lexicographic_parent","[NULL]","[NULL]"
        """))

  def test_sanity_ordering(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks_ordering.textproto'),
        query="""
        INCLUDE PERFETTO MODULE viz.summary.track_event;
        SELECT t.name, g.order_id
        FROM _track_event_tracks_ordered_groups g
        JOIN track t ON CAST(g.track_ids AS INT) = t.id
        ORDER BY t.name;
        """,
        out=Csv("""
        "name","order_id"
        "a",1
        "ab",2
        "b",3
        "chrono",1
        "chrono1",3
        "chrono2",2
        "chronological_parent",1
        "event_for_unnamed_child",4
        "explicit_child:-5 z-index",1
        "explicit_child:5 z-index",3
        "explicit_child:no z-index",2
        "explicit_parent",2
        "lexicographic_parent",3
        """))

  def test_ordered_tracks_description(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          track_descriptor {
            uuid: 1
            name: "A"
            description: "Track A's description"
          }
        }
        packet {
          timestamp: 220
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 1
            name: "A1"
          }
        }
        packet {
          timestamp: 230
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 1
          }
        }
        packet {
          track_descriptor {
            uuid: 2
            name: "B"
          }
        }
        packet {
          timestamp: 210
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 2
            name: "B1"
          }
        }
        packet {
          timestamp: 215
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 2
          }
        }
        packet {
          track_descriptor {
            uuid: 3
            process {
              pid: 5
              process_name: "p1"
            }
            description: "Process p1 description"
          }
        }
        packet {
          timestamp: 210
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 3
            name: "P1"
          }
        }
        packet {
          timestamp: 215
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 3
          }
        }
        packet {
          track_descriptor {
            uuid: 4
            name: "C"
            description: "Track C's description"
          }
        }
        packet {
          timestamp: 210
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_BEGIN
            track_uuid: 4
            name: "C1"
          }
        }
        packet {
          timestamp: 215
          trusted_packet_sequence_id: 3903809
          track_event {
            type: TYPE_SLICE_END
            track_uuid: 4
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE viz.summary.track_event;
        SELECT name, description
        FROM _track_event_tracks_ordered_groups
        ORDER BY name;
        """,
        out=Csv("""
          "name","description"
          "[NULL]","Process p1 description"
          "A","Track A's description"
          "B","[NULL]"
          "C","Track C's description"
        """),
    )

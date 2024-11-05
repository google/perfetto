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
          id,
          parent_id,
          EXTRACT_ARG(source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track;
        """,
        out=Csv("""
        "id","parent_id","ordering","rank"
        0,"[NULL]","chronological","[NULL]"
        1,0,"[NULL]","[NULL]"
        2,0,"[NULL]","[NULL]"
        """))

  def test_all_tracks_ordered_chronological(self):
    return DiffTestBlueprint(
        trace=self.chronological_trace,
        query="""
        INCLUDE PERFETTO MODULE viz.summary.tracks;
        SELECT id, order_id
        FROM _track_event_tracks_ordered
        ORDER BY id;
        """,
        out=Csv("""
        "id","order_id"
        1,2
        2,1
        """))

  def test_track_event_tracks_explicit(self):
    return DiffTestBlueprint(
        trace=self.explicit_trace,
        query="""
        SELECT
          id,
          parent_id,
          EXTRACT_ARG(source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track;
        """,
        out=Csv("""
        "id","parent_id","ordering","rank"
        0,"[NULL]","explicit","[NULL]"
        1,0,"[NULL]",1
        2,0,"[NULL]",100
        3,0,"[NULL]",-100
        """))

  def test_all_tracks_ordered_explicit(self):
    return DiffTestBlueprint(
        trace=self.explicit_trace,
        query="""
        INCLUDE PERFETTO MODULE viz.summary.tracks;
        SELECT id, order_id
        FROM _track_event_tracks_ordered
        ORDER BY id;
        """,
        out=Csv("""
        "id","order_id"
        1,2
        2,3
        3,1
        """))

  def test_track_event_tracks_lexicographic(self):
    return DiffTestBlueprint(
        trace=self.lexicographic_trace,
        query="""
        SELECT
          id,
          parent_id,
          name,
          EXTRACT_ARG(source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track;
        """,
        out=Csv("""
        "id","parent_id","name","ordering","rank"
        0,"[NULL]","Root Lexicographic","lexicographic","[NULL]"
        1,0,"B","[NULL]","[NULL]"
        2,0,"A","[NULL]","[NULL]"
        3,0,"C","[NULL]","[NULL]"
        """))

  def test_all_tracks_ordered_lexicographic(self):
    return DiffTestBlueprint(
        trace=self.lexicographic_trace,
        query="""
        INCLUDE PERFETTO MODULE viz.summary.tracks;
        SELECT id, order_id
        FROM _track_event_tracks_ordered
        ORDER BY id;
        """,
        out=Csv("""
        "id","order_id"
        1,2
        2,1
        3,3
        """))

  def test_track_event_tracks_all_orderings(self):
    return DiffTestBlueprint(
        trace=self.all_ordering_trace,
        query="""
        SELECT
          id,
          parent_id,
          name,
          EXTRACT_ARG(source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track
        ORDER BY parent_id, id;
        """,
        out=Csv("""
        "id","parent_id","name","ordering","rank"
        0,"[NULL]","Root Chronological","chronological","[NULL]"
        3,"[NULL]","Root Lexicographic","lexicographic","[NULL]"
        5,"[NULL]","Root Explicit","explicit","[NULL]"
        1,0,"A","[NULL]","[NULL]"
        2,0,"B","[NULL]","[NULL]"
        4,3,"A","[NULL]","[NULL]"
        7,3,"B","[NULL]","[NULL]"
        10,3,"C","[NULL]","[NULL]"
        6,5,"B","[NULL]",1
        8,5,"C","[NULL]",-100
        9,5,"A","[NULL]",100
        """))

  def test_all_tracks_ordered_all_ordering(self):
    return DiffTestBlueprint(
        trace=self.all_ordering_trace,
        query="""
        INCLUDE PERFETTO MODULE viz.summary.tracks;
        SELECT id, parent_id, order_id
        FROM _track_event_tracks_ordered
        JOIN track USING (id)
        ORDER BY parent_id, id
        """,
        out=Csv("""
        "id","parent_id","order_id"
        1,0,2
        2,0,1
        4,3,1
        7,3,2
        10,3,3
        6,5,2
        8,5,1
        9,5,3
        """))

  def test_sanity_ordering_tracks(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks_ordering.textproto'),
        query="""
        SELECT
          id,
          parent_id,
          name,
          EXTRACT_ARG(source_arg_set_id, 'child_ordering') AS ordering,
          EXTRACT_ARG(source_arg_set_id, 'sibling_order_rank') AS rank
        FROM track
        ORDER BY parent_id, id;
        """,
        out=Csv("""
          "id","parent_id","name","ordering","rank"
          0,"[NULL]","explicit_parent","explicit",-10
          4,"[NULL]","chronological_parent","chronological","[NULL]"
          9,"[NULL]","lexicographic_parent","lexicographic","[NULL]"
          1,0,"explicit_child:no z-index","[NULL]","[NULL]"
          2,0,"explicit_child:5 z-index","[NULL]",5
          3,0,"explicit_child:-5 z-index","[NULL]",-5
          8,0,"explicit_child:-5 z-index","[NULL]",-5
          5,4,"chrono","[NULL]","[NULL]"
          6,4,"chrono2","[NULL]","[NULL]"
          7,4,"chrono1","[NULL]","[NULL]"
          10,9,"[NULL]","[NULL]","[NULL]"
          11,9,"a","[NULL]","[NULL]"
          12,9,"b","[NULL]","[NULL]"
          13,9,"ab","[NULL]","[NULL]"
        """))

  def test_sanity_ordering(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks_ordering.textproto'),
        query="""
        INCLUDE PERFETTO MODULE viz.summary.tracks;
        SELECT id, order_id
        FROM _track_event_tracks_ordered
        ORDER BY id;
        """,
        out=Csv("""
        "id","order_id"
        1,3
        2,4
        3,1
        5,1
        6,2
        7,3
        8,2
        10,1
        11,2
        12,4
        13,3
        """))

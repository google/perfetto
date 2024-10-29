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

  def test_track_event_tracks_ordering(self):
    return DiffTestBlueprint(
        trace=Path('track_event_tracks_ordering.textproto'),
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
        0,"[NULL]","explicit",-10
        1,0,"[NULL]","[NULL]"
        2,0,"[NULL]",5
        3,0,"[NULL]",-5
        4,"[NULL]","chronological","[NULL]"
        5,4,"[NULL]","[NULL]"
        6,4,"[NULL]","[NULL]"
        7,4,"[NULL]","[NULL]"
        8,0,"[NULL]",-5
        9,"[NULL]","lexicographic","[NULL]"
        10,9,"[NULL]","[NULL]"
        11,9,"[NULL]","[NULL]"
        12,9,"[NULL]","[NULL]"
        13,9,"[NULL]","[NULL]"
        """))

  def test_all_tracks_ordered(self):
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
        1,4
        2,3
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
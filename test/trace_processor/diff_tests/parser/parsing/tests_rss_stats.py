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
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ParsingRssStats(TestSuite):

  def test_rss_stat_mm_id(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_mm_id.py'),
        query="""
        SELECT c.ts, t.name, p.pid, p.name, c.value
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        JOIN process p USING (upid)
        ORDER BY ts, pid;
        """,
        out=Csv("""
        "ts","name","pid","name","value"
        90,"mem.rss.file",3,"kthreadd_child",9.000000
        99,"mem.rss.file",3,"kthreadd_child",10.000000
        100,"mem.rss.file",10,"process",1000.000000
        101,"mem.rss.file",10,"process",900.000000
        """))

  def test_rss_stat_mm_id_clone(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_mm_id_clone.py'),
        query="""
        SELECT c.ts, t.name, p.pid, p.name, c.value
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        JOIN process p USING (upid)
        ORDER BY ts, pid;
        """,
        out=Csv("""
        "ts","name","pid","name","value"
        100,"mem.rss.file",3,"kernel_thread",10.000000
        100,"mem.rss.file",10,"parent_process",100.000000
        102,"mem.rss.file",4,"kernel_thread2",20.000000
        102,"mem.rss.file",11,"child_process",90.000000
        104,"mem.rss.file",11,"child_process",10.000000
        105,"mem.rss.file",10,"parent_process",95.000000
        107,"mem.rss.file",10,"parent_process",105.000000
        108,"mem.rss.file",10,"parent_process",110.000000
        """))

  def test_rss_stat_mm_id_reuse(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_mm_id_reuse.py'),
        query="""
        SELECT c.ts, t.name, p.pid, p.name, c.value
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        JOIN process p USING (upid)
        ORDER BY ts, pid;
        """,
        out=Csv("""
        "ts","name","pid","name","value"
        100,"mem.rss.file",10,"parent_process",100.000000
        103,"mem.rss.file",10,"new_process",10.000000
        """))

  def test_rss_stat_legacy(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_legacy.py'),
        query="""
        SELECT c.ts, t.name, p.pid, p.name, c.value
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        JOIN process p USING (upid)
        ORDER BY ts, pid;
        """,
        out=Csv("""
        "ts","name","pid","name","value"
        90,"mem.rss.file",3,"kthreadd_child",9.000000
        91,"mem.rss.file",3,"kthreadd_child",900.000000
        99,"mem.rss.file",10,"process",10.000000
        100,"mem.rss.file",10,"process",1000.000000
        101,"mem.rss.file",3,"kthreadd_child",900.000000
        """))

  def test_rss_stat_after_free(self):
    return DiffTestBlueprint(
        trace=Path('rss_stat_after_free.py'),
        query="""
        SELECT
          pid,
          max(c.ts) AS last_rss,
          p.end_ts AS process_end
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        JOIN process p USING(upid)
        GROUP BY upid;
        """,
        out=Csv("""
        "pid","last_rss","process_end"
        10,100,101
        11,90,"[NULL]"
        """))

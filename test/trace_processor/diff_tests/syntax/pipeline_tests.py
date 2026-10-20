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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv, ExpectedError, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PerfettoPipeline(TestSuite):

  def test_tree_accumulate_up_and_down(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE tree AS
        SELECT 0 AS id, NULL AS parent_id, 'root' AS name, 10 AS self
        UNION ALL SELECT 1, 0, 'a', 20
        UNION ALL SELECT 2, 0, 'b', 30
        UNION ALL SELECT 3, 1, 'c', 40;

        CREATE PERFETTO TABLE totals AS
        FROM tree
        |> TREE ACCUMULATE UP SUM(self) AS total
        |> TREE ACCUMULATE DOWN SUM(self) AS path;

        SELECT name, self, total, path FROM totals ORDER BY name;
        """,
        out=Csv("""
        "name","self","total","path"
        "a",20,60,30
        "b",30,30,40
        "c",40,40,70
        "root",10,100,10
        """))

  # A stage walks the tree its own id and parent_id describe, even when an
  # earlier stage walked another.
  def test_tree_accumulate_after_the_tree_changes(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE t AS
        SELECT 0 AS id, NULL AS parent_id, 1 AS v, 0 AS id2, 1 AS p2
        UNION ALL SELECT 1, 0, 10, 1, 2
        UNION ALL SELECT 2, 1, 100, 2, NULL;

        FROM t
        |> TREE ACCUMULATE UP SUM(v) AS a
        |> SET id = id2, parent_id = p2
        |> TREE ACCUMULATE UP SUM(v) AS b
        |> ORDER BY v
        |> SELECT v, a, b;
        """,
        out=Csv("""
        "v","a","b"
        1,111,1
        10,110,11
        100,100,111
        """))

  # Rows dropped between two stages can leave a parent missing, which the
  # second stage finds as it would with no stage before it.
  def test_tree_accumulate_after_rows_are_dropped(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE t AS
        SELECT 0 AS id, NULL AS parent_id, 1 AS v
        UNION ALL SELECT 1, 0, 10
        UNION ALL SELECT 2, 1, 100;

        FROM t
        |> TREE ACCUMULATE UP SUM(v) AS a
        |> WHERE v > 1
        |> TREE ACCUMULATE UP SUM(v) AS b
        |> SELECT v, a, b;
        """,
        out=ExpectedError('a row names a parent which is not itself a row'))

  # Must match the descendant closure, which this is meant to replace.
  def test_tree_accumulate_matches_closure(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE piped AS
        FROM (SELECT id, parent_id, dur FROM slice)
        |> TREE ACCUMULATE UP SUM(dur) AS subtree_dur, COUNT(*) AS subtree_size
        |> TREE ACCUMULATE DOWN SUM(dur) AS path_dur, COUNT(*) AS depth;

        CREATE PERFETTO TABLE closure AS
        WITH RECURSIVE descendants(root_id, id) AS (
          SELECT id, id FROM slice
          UNION ALL
          SELECT d.root_id, c.id
          FROM descendants d JOIN slice c ON c.parent_id = d.id
        )
        SELECT
          d.root_id AS ancestor,
          d.id AS descendant,
          s.dur AS dur
        FROM descendants d JOIN slice s ON s.id = d.id;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (
            SELECT count(*) FROM (
              SELECT id, subtree_dur, subtree_size, path_dur, depth FROM piped
              EXCEPT
              SELECT
                up.id, up.subtree_dur, up.subtree_size, down.path_dur,
                down.depth
              FROM (
                SELECT ancestor AS id, sum(dur) AS subtree_dur,
                  count(*) AS subtree_size
                FROM closure GROUP BY ancestor
              ) up
              JOIN (
                SELECT c.descendant AS id, sum(s.dur) AS path_dur,
                  count(*) AS depth
                FROM closure c JOIN slice s ON s.id = c.ancestor
                GROUP BY c.descendant
              ) down USING (id)
            )
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        74228,0
        """))

  # Must match interval_self_intersect, which this is meant to replace.
  def test_interval_flatten_matches_self_intersect(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE piped AS
        FROM (SELECT ts, dur FROM slice WHERE dur > 0)
        |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS n;

        CREATE PERFETTO TABLE macro AS
        SELECT ts, dur, sum(NOT interval_ends_at_ts) AS n
        FROM interval_self_intersect!(
          (SELECT id, ts, dur FROM slice WHERE dur > 0)
        )
        GROUP BY group_id
        HAVING n > 0;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (
            SELECT count(*) FROM (
              SELECT ts, dur, n FROM piped
              EXCEPT SELECT ts, dur, n FROM macro
            )
          ) + (
            SELECT count(*) FROM (
              SELECT ts, dur, n FROM macro
              EXCEPT SELECT ts, dur, n FROM piped
            )
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        107295,0
        """))

  # Every aggregate over what's live in a segment, as the macro gives it.
  def test_interval_flatten_aggregates_match_self_intersect(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE piped AS
        FROM (SELECT ts, dur, depth FROM slice WHERE dur > 0)
        |> INTERVAL FLATTEN AGGREGATE
          SUM(depth) AS total, MIN(depth) AS lo, MAX(depth) AS hi;

        CREATE PERFETTO TABLE macro AS
        SELECT
          i.ts,
          i.dur,
          sum(IIF(i.interval_ends_at_ts, NULL, s.depth)) AS total,
          min(IIF(i.interval_ends_at_ts, NULL, s.depth)) AS lo,
          max(IIF(i.interval_ends_at_ts, NULL, s.depth)) AS hi
        FROM interval_self_intersect!(
          (SELECT id, ts, dur FROM slice WHERE dur > 0)
        ) i
        JOIN slice s ON s.id = i.id
        GROUP BY i.group_id
        HAVING sum(NOT i.interval_ends_at_ts) > 0;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (
            SELECT count(*) FROM (
              SELECT ts, dur, total, lo, hi FROM piped
              EXCEPT SELECT ts, dur, total, lo, hi FROM macro
            )
          ) + (
            SELECT count(*) FROM (
              SELECT ts, dur, total, lo, hi FROM macro
              EXCEPT SELECT ts, dur, total, lo, hi FROM piped
            )
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        107295,0
        """))

  # Keys come out as the types they went in as, nulls included.
  def test_aggregate_keys_keep_their_types(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE t AS
        SELECT 1.5 AS d, 'a' AS s, 7 AS i
        UNION ALL SELECT 1.5, NULL, NULL
        UNION ALL SELECT -2.25, 'a', 7
        UNION ALL SELECT 1.5, 'a', 7;

        FROM t
        |> AGGREGATE COUNT(*) AS n GROUP BY d, s, i;
        """,
        out=Csv("""
        "d","s","i","n"
        1.500000,"a",7,2
        1.500000,"[NULL]","[NULL]",1
        -2.250000,"a",7,1
        """))

  # AGGREGATE ... GROUP BY gives what SQLite's GROUP BY does.
  def test_aggregate_matches_group_by(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;

        CREATE PERFETTO TABLE piped AS
        FROM (SELECT track_id, name, depth, dur FROM slice)
        |> AGGREGATE COUNT(*) AS n, SUM(dur) AS total, MIN(depth) AS lo,
             MAX(dur) AS hi
           GROUP BY track_id, name;

        CREATE PERFETTO TABLE grouped AS
        SELECT track_id, name, count(*) AS n, sum(dur) AS total,
          min(depth) AS lo, max(dur) AS hi
        FROM slice
        GROUP BY track_id, name;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (
            SELECT count(*) FROM (
              SELECT * FROM piped EXCEPT SELECT * FROM grouped
            )
          ) + (
            SELECT count(*) FROM (
              SELECT * FROM grouped EXCEPT SELECT * FROM piped
            )
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        583,0
        """))

  # Per key, sum(n * dur) over segments equals sum(dur) over rows.
  def test_interval_flatten_keys_over_many_batches(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;

        CREATE PERFETTO TABLE piped AS
        FROM (
          SELECT ts, dur, track_id, IIF(depth % 2 = 0, 'even', 'odd') AS parity
          FROM slice
          WHERE dur > 0
        )
        |> INTERVAL FLATTEN PER track_id, parity AGGREGATE COUNT(*) AS n;

        CREATE PERFETTO TABLE expected AS
        SELECT
          track_id,
          IIF(depth % 2 = 0, 'even', 'odd') AS parity,
          sum(dur) AS covered
        FROM slice
        WHERE dur > 0
        GROUP BY track_id, parity;

        SELECT
          (SELECT count(*) FROM expected) AS keys,
          (
            SELECT count(*) FROM expected e
            FULL JOIN (
              SELECT track_id, parity, sum(n * dur) AS covered
              FROM piped
              GROUP BY track_id, parity
            ) p USING (track_id, parity)
            WHERE e.covered IS NOT p.covered
          ) AS mismatches;
        """,
        out=Csv("""
        "keys","mismatches"
        101,0
        """))

  # `ts` is already in order on `slice`, so the first sorts nothing.
  def test_order_by_matches_sql(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;

        CREATE PERFETTO TABLE by_ts AS
        FROM slice
        |> ORDER BY ts
        |> SELECT id;

        CREATE PERFETTO TABLE by_many AS
        FROM (SELECT id, dur, depth, parent_id FROM slice)
        |> ORDER BY depth DESC, parent_id, dur DESC, id
        |> SELECT id;

        SELECT
          (SELECT count(*) FROM by_many) AS rows,
          (
            SELECT count(*)
            FROM (SELECT row_number() OVER () AS n, id FROM by_ts) AS p
            JOIN (
              SELECT row_number() OVER (ORDER BY ts, id) AS n, id FROM slice
            ) AS s USING (n)
            WHERE p.id != s.id
          ) + (
            SELECT count(*)
            FROM (SELECT row_number() OVER () AS n, id FROM by_many) AS p
            JOIN (
              SELECT
                row_number() OVER (
                  ORDER BY depth DESC, parent_id, dur DESC, id
                ) AS n,
                id
              FROM slice
            ) AS s USING (n)
            WHERE p.id != s.id
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        74228,0
        """))

  # thread_state only renames __intrinsic_thread_state's columns, reading
  # ucpu twice, so it is read from the table without SQLite.
  def test_a_renaming_view_reads_its_table(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        PERFETTO PRAGMA pipelines = 1;

        CREATE PERFETTO TABLE by_dur AS
        FROM thread_state
        |> ORDER BY dur, id
        |> SELECT id, cpu, ucpu, state;

        SELECT
          (SELECT count(*) FROM by_dur) AS rows,
          (
            SELECT count(*)
            FROM (SELECT row_number() OVER () AS n, * FROM by_dur) AS p
            JOIN (
              SELECT row_number() OVER (ORDER BY dur, id) AS n, *
              FROM thread_state
            ) AS s USING (n)
            WHERE p.id != s.id OR p.cpu IS NOT s.cpu
              OR p.ucpu IS NOT s.ucpu OR p.state != s.state
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        549904,0
        """))

  def test_pipeline_errors_name_the_problem(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 1 AS id, NULL AS parent_id)
        |> TREE ACCUMULATE UP AVG(id) AS biggest;
        """,
        out=ExpectedError('aggregate AVG is not supported yet'))

  # Must match SQL's WHERE and DISTINCT, including comparing an integer
  # column with a fraction.
  def test_where_and_distinct_match_sql(self):
    return DiffTestBlueprint(
        trace=DataPath('chrome_input_with_frame_view.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE piped AS
        FROM (SELECT track_id, depth, dur FROM slice)
        |> WHERE dur > 999.5 AND depth <= 2
        |> SELECT track_id, depth
        |> DISTINCT;

        CREATE PERFETTO TABLE expected AS
        SELECT DISTINCT track_id, depth FROM slice
        WHERE dur > 999.5 AND depth <= 2;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (SELECT count(*) FROM expected) AS expected_rows,
          (SELECT count(*) FROM (
            SELECT * FROM piped EXCEPT SELECT * FROM expected))
          + (SELECT count(*) FROM (
            SELECT * FROM expected EXCEPT SELECT * FROM piped)) AS mismatches;
        """,
        out=Csv("""
        "rows","expected_rows","mismatches"
        126,126,0
        """))

  # DISTINCT compares the columns a bare name finds, so two rows of `a`
  # meeting `b` over one region give one row.
  def test_distinct_after_intersection(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 10 AS dur, 1 AS k
        UNION ALL SELECT 1, 10, 1;
        CREATE PERFETTO TABLE b AS SELECT 5 AS ts, 2 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y)
        |> DISTINCT
        |> SELECT ts, dur, x.k;
        """,
        out=Csv("""
        "ts","dur","k"
        5,2,1
        """))

  # A column DISTINCT does not compare has no one value per row, so it is
  # gone from the aliases too.
  def test_distinct_drops_columns_it_does_not_compare(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE t AS SELECT 1 AS x, 2 AS y;

        FROM t AS t |> DROP x |> DISTINCT |> SELECT t.x;
        """,
        out=ExpectedError("no such column: 't.x'"))

  def test_a_pipeline_needs_the_pragma(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        FROM (SELECT 1 AS id, NULL AS parent_id)
        |> TREE ACCUMULATE UP SUM(id) AS total;
        """,
        out=ExpectedError('Pipelines are not enabled'))

  def test_the_pragma_admits_a_pipeline(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 1 AS id, NULL AS parent_id, 7 AS self)
        |> TREE ACCUMULATE UP SUM(self) AS total;
        """,
        out=Csv("""
        "id","parent_id","self","total"
        1,"[NULL]",7,7
        """))

  def test_the_pragma_can_be_turned_back_off(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        PERFETTO PRAGMA pipelines = 0;
        FROM (SELECT 1 AS id, NULL AS parent_id)
        |> TREE ACCUMULATE UP SUM(id) AS total;
        """,
        out=ExpectedError('Pipelines are not enabled'))

  def test_an_unknown_pragma_says_so(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA nonsense = 1;
        """,
        out=ExpectedError("there is no setting 'nonsense'"))

  def test_a_pragma_takes_a_whole_number(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 'yes';
        """,
        out=ExpectedError('expected a whole number'))

  # The region carries its own bounds, and each operand's row comes along
  # under the name it was given.
  def test_interval_intersection_of_two_tables(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 30 AS dur, 7 AS freq
        UNION ALL SELECT 40, 10, 8;

        CREATE PERFETTO TABLE b AS
        SELECT 10 AS ts, 10 AS dur, 1 AS state
        UNION ALL SELECT 35, 10, 2;

        INTERVAL INTERSECTION OF (a AS x, b AS y)
        |> SELECT ts, dur, x.freq, y.state;
        """,
        out=Csv("""
        "ts","dur","freq","state"
        10,10,7,1
        40,5,8,2
        """))

  # An operand's own columns are still readable through its name, so a caller
  # which needs the whole interval rather than the region can have it.
  def test_interval_intersection_keeps_the_operand_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y)
        |> SELECT ts, dur, x.ts AS x_ts, x.dur AS x_dur, y.ts AS y_ts;
        """,
        out=Csv("""
        "ts","dur","x_ts","x_dur","y_ts"
        10,20,0,30,10
        """))

  # A star gives the region's ts and dur, not each operand's.
  def test_interval_intersection_star_is_the_region(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur, 7 AS freq;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur, 1 AS state;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT *;
        """,
        out=Csv("""
        "ts","dur","freq","state"
        10,20,7,1
        """))

  def test_interval_intersection_without_a_stage_is_the_region(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur, 7 AS freq;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur, 1 AS state;

        INTERVAL INTERSECTION OF (a AS x, b AS y);
        """,
        out=Csv("""
        "ts","dur","freq","state"
        10,20,7,1
        """))

  def test_interval_intersection_operand_columns_by_bare_name(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur, 7 AS freq;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur, 1 AS state;

        INTERVAL INTERSECTION OF (a AS x, b AS y)
        |> SELECT ts, dur, freq, state, x.freq AS x_freq;
        """,
        out=Csv("""
        "ts","dur","freq","state","x_freq"
        10,20,7,1,7
        """))

  def test_interval_intersection_new_alias_is_the_region(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur, 7 AS freq;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur, 1 AS state;

        INTERVAL INTERSECTION OF (a AS x, b AS y)
        |> AS u
        |> SELECT u.ts, u.dur, u.freq;
        """,
        out=Csv("""
        "ts","dur","freq"
        10,20,7
        """))

  # A later operand's copy of a PER column is reached through its alias.
  def test_interval_intersection_per_column_of_a_later_operand(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur, 2 AS cpu;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur, 2 AS cpu;

        INTERVAL INTERSECTION OF (a AS x, b AS y) PER cpu
        |> SELECT *, y.cpu AS y_cpu;
        """,
        out=Csv("""
        "ts","dur","cpu","y_cpu"
        10,20,2,2
        """))

  def test_interval_intersection_shared_column_is_ambiguous(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur, 7 AS n;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur, 1 AS n;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT n;
        """,
        out=ExpectedError("column 'n' is ambiguous"))

  # PER confines regions to rows which agree on the columns named, and those
  # columns come out under their bare names.
  def test_interval_intersection_per_a_key(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 30 AS dur, 1 AS cpu
        UNION ALL SELECT 0, 30, 2;

        CREATE PERFETTO TABLE b AS
        SELECT 10 AS ts, 10 AS dur, 2 AS cpu
        UNION ALL SELECT 10, 10, 3;

        INTERVAL INTERSECTION OF (a AS x, b AS y) PER cpu
        |> SELECT ts, dur, cpu;
        """,
        out=Csv("""
        "ts","dur","cpu"
        10,10,2
        """))

  # Rows holding no value in a PER column agree on it, as they would under
  # GROUP BY, which is what the macro this replaces does.
  def test_interval_intersection_per_an_absent_key(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 30 AS dur, 1 AS cpu, 'one' AS name
        UNION ALL SELECT 0, 30, NULL, 'two';

        CREATE PERFETTO TABLE b AS
        SELECT 10 AS ts, 10 AS dur, 1 AS cpu
        UNION ALL SELECT 10, 10, NULL;

        CREATE PERFETTO TABLE met AS
        INTERVAL INTERSECTION OF (a AS x, b AS y) PER cpu
        |> SELECT ts, dur, cpu, x.name;

        SELECT ts, dur, cpu, name FROM met ORDER BY name;
        """,
        out=Csv("""
        "ts","dur","cpu","name"
        10,10,1,"one"
        10,10,"[NULL]","two"
        """))

  # -0.0 and 0.0 are the same key, as they are under GROUP BY.
  def test_interval_intersection_per_a_negative_zero_key(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 10 AS dur, 0.0 AS k, 'pos' AS name
        UNION ALL SELECT 0, 10, -0.0, 'neg';

        CREATE PERFETTO TABLE b AS SELECT 0 AS ts, 10 AS dur, 0.0 AS k;

        CREATE PERFETTO TABLE met AS
        INTERVAL INTERSECTION OF (a AS x, b AS y) PER k
        |> SELECT ts, dur, x.name;

        SELECT ts, dur, name FROM met ORDER BY name;
        """,
        out=Csv("""
        "ts","dur","name"
        0,10,"neg"
        0,10,"pos"
        """))

  # A row with no duration has no bounds, so it covers nothing; the last row
  # of a table built with `lead(ts) - ts` is usually one of these.
  def test_interval_intersection_skips_a_row_without_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS
        SELECT 0 AS ts, 10 AS dur, 1 AS n
        UNION ALL SELECT 10, NULL, 2
        UNION ALL SELECT NULL, 10, 3;

        CREATE PERFETTO TABLE b AS SELECT 0 AS ts, 100 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT ts, dur, x.n;
        """,
        out=Csv("""
        "ts","dur","n"
        0,10,1
        """))

  # Two intervals meet over the instants they share, so touching end to start
  # is no meeting at all.
  def test_interval_intersection_touching_is_no_meeting(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 10 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 10 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT ts, dur;
        """,
        out=Csv("""
        "ts","dur"
        """))

  # A row of no width is a point: it meets an interval holding the instant it
  # sits at, and the interval's end is not such an instant.
  def test_interval_intersection_point_positions(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        #        5    10   15   19   20   25
        # o:          [--------------)
        # p:     .    .    .    .    .    .
        # res:        x    x    x
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE o AS SELECT 10 AS ts, 10 AS dur;

        CREATE PERFETTO TABLE p AS
        SELECT 5 AS ts, 0 AS dur
        UNION ALL SELECT 10, 0
        UNION ALL SELECT 15, 0
        UNION ALL SELECT 19, 0
        UNION ALL SELECT 20, 0
        UNION ALL SELECT 25, 0;

        CREATE PERFETTO TABLE met AS
        INTERVAL INTERSECTION OF (p AS x, o AS y) |> SELECT ts, dur;

        SELECT ts, dur FROM met ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur"
        10,0
        15,0
        19,0
        """))

  # Narrowing an interval down to a point keeps the point, and a point which
  # no longer meets anything takes the region with it.
  def test_interval_intersection_through_three_tables(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 100 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 20 AS ts, 10 AS dur;
        CREATE PERFETTO TABLE c AS
        SELECT 25 AS ts, 0 AS dur UNION ALL SELECT 60, 0;

        INTERVAL INTERSECTION OF (a AS x, b AS y, c AS z)
        |> SELECT ts, dur;
        """,
        out=Csv("""
        "ts","dur"
        25,0
        """))

  # A duration which never ends is refused rather than read as a span.
  def test_interval_intersection_refuses_a_negative_duration(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 10 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 5 AS ts, -1 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT ts, dur;
        """,
        out=ExpectedError('ts or dur is below zero'))

  def test_interval_intersection_refuses_an_overflowing_end(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 9223372036854775800 AS ts, 100 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 0 AS ts, 10 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT ts, dur;
        """,
        out=ExpectedError('integer overflow'))

  def test_interval_intersection_needs_the_pragma(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 10 AS dur;
        CREATE PERFETTO TABLE b AS SELECT 5 AS ts, 10 AS dur;

        INTERVAL INTERSECTION OF (a AS x, b AS y) |> SELECT ts, dur;
        """,
        out=ExpectedError('Pipelines are not enabled'))

  # Must match the macro, which this is meant to replace.
  def test_interval_intersection_matches_the_macro(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        INCLUDE PERFETTO MODULE intervals.intersect;

        CREATE PERFETTO TABLE running AS
        SELECT id, ts, dur, cpu FROM sched WHERE dur > 0;

        CREATE PERFETTO TABLE freq AS
        SELECT * FROM (
          SELECT
            c.id,
            c.ts,
            lead(c.ts) OVER (PARTITION BY t.cpu ORDER BY c.ts) - c.ts AS dur,
            t.cpu,
            cast_int!(c.value) AS freq
          FROM counter c
          JOIN cpu_counter_track t ON c.track_id = t.id
          WHERE t.name = 'cpufreq'
        ) WHERE dur > 0;

        CREATE PERFETTO TABLE piped AS
        INTERVAL INTERSECTION OF (running AS r, freq AS f) PER cpu
        |> SELECT ts, dur, cpu, r.id AS sched_id, f.freq;

        CREATE PERFETTO TABLE macroed AS
        SELECT ii.ts, ii.dur, ii.cpu, r.id AS sched_id, f.freq
        FROM _interval_intersect!((running, freq), (cpu)) ii
        JOIN running r ON r.id = ii.id_0
        JOIN freq f ON f.id = ii.id_1;

        SELECT
          (SELECT count(*) FROM piped) AS rows,
          (
            SELECT count(*) FROM (
              SELECT * FROM piped EXCEPT SELECT * FROM macroed
              UNION ALL
              SELECT * FROM macroed EXCEPT SELECT * FROM piped
            )
          ) AS mismatches;
        """,
        out=Csv("""
        "rows","mismatches"
        45728,0
        """))

  # A SQL source asked for fewer columns still runs, whether it is read by a
  # tree fold or an intersection.
  def test_pruned_sql_sources_run(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE folded AS
        FROM (
          SELECT 0 AS id, NULL AS parent_id, 5 AS self, 'root' AS name
          UNION ALL SELECT 1, 0, 7, 'a'
        ) AS t
        |> TREE ACCUMULATE UP SUM(self) AS total
        |> SELECT id, total;

        CREATE PERFETTO TABLE regions AS
        INTERVAL INTERSECTION OF (
          (SELECT 0 AS ts, 30 AS dur, 7 AS freq, 'x' AS unused) AS a,
          (SELECT 10 AS ts, 100 AS dur, 1 AS state) AS b
        )
        |> SELECT ts, dur, a.freq;

        SELECT
          (SELECT group_concat(id || ':' || total) FROM (SELECT * FROM folded ORDER BY id))
            AS folded,
          (SELECT group_concat(ts || ':' || dur || ':' || freq) FROM regions)
            AS regions;
        """,
        out=Csv("""
        "folded","regions"
        "0:12,1:7","10:20:7"
        """))

  # The select-like stages only change which columns the row has and what
  # they are called; the values follow the columns they name.
  def test_select_like_stages(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE tree AS
        SELECT 0 AS id, NULL AS parent_id, 'root' AS name, 10 AS self
        UNION ALL SELECT 1, 0, 'a', 20
        UNION ALL SELECT 2, 0, 'b', 30;

        CREATE PERFETTO TABLE shaped AS
        FROM tree AS t
        |> TREE ACCUMULATE UP SUM(self) AS total
        |> DROP self
        |> RENAME total AS subtree
        |> EXTEND t.self AS own
        |> SET name = t.name
        |> AS s
        |> SELECT s.* EXCEPT (parent_id) REPLACE (own AS id);

        SELECT * FROM shaped ORDER BY name;
        """,
        out=Csv("""
        "id","name","subtree","own"
        20,"a",20,20
        30,"b",30,30
        10,"root",60,10
        """))

  # Stages naming bare columns see the region's ts and dur, not an operand's.
  def test_select_like_stages_after_intersection(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE a AS SELECT 0 AS ts, 30 AS dur, 7 AS freq;
        CREATE PERFETTO TABLE b AS SELECT 10 AS ts, 100 AS dur, 1 AS state;

        INTERVAL INTERSECTION OF (a AS x, b AS y)
        |> RENAME ts AS start
        |> EXTEND x.ts AS x_start
        |> SELECT start, dur, x_start, y.state;
        """,
        out=Csv("""
        "start","dur","x_start","state"
        10,20,0,1
        """))

  # Matches the stdlib macro it replaces, on rows which never overlap.
  def test_interval_fill_gaps_matches_the_macro(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        INCLUDE PERFETTO MODULE intervals.fill_gaps;

        CREATE PERFETTO TABLE sched_rows AS
        SELECT ts, dur, cpu, utid FROM sched WHERE dur > 0 AND utid != 0;

        CREATE PERFETTO TABLE piped AS
        FROM sched_rows
        |> INTERVAL FILL GAPS WITH (
             SELECT trace_start() AS ts, trace_dur() AS dur
           ) PER cpu;

        CREATE PERFETTO TABLE macro AS
        SELECT ts, dur, cpu, utid
        FROM _intervals_fill_gaps!((cpu), (utid), sched_rows);

        SELECT
          (SELECT count(*) FROM piped WHERE utid IS NULL) AS fillers,
          (
            SELECT count(*) FROM (
              SELECT ts, dur, cpu, utid FROM piped
              EXCEPT SELECT ts, dur, cpu, utid FROM macro
            )
          ) + (
            SELECT count(*) FROM (
              SELECT ts, dur, cpu, utid FROM macro
              EXCEPT SELECT ts, dur, cpu, utid FROM piped
            )
          ) AS mismatches;
        """,
        out=Csv("""
        "fillers","mismatches"
        76331,0
        """))

  # Without PER, a filler takes the background's columns by name and its own
  # bounds.
  def test_interval_fill_gaps_takes_the_background_by_name(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS
        SELECT 2 AS ts, 3 AS dur, 'run' AS state
        UNION ALL SELECT 6, 1, 'run';

        FROM spans
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 10 AS dur, 'idle' AS state)
        |> ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","state"
        0,2,"idle"
        2,3,"run"
        5,1,"idle"
        6,1,"run"
        7,3,"idle"
        """))

  # A background without the PER columns fills every lane of the input, and a
  # filler takes its lane's PER values.
  def test_interval_fill_gaps_shares_the_background_across_lanes(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS
        SELECT 2 AS ts, 3 AS dur, 0 AS cpu, 'run' AS state
        UNION ALL SELECT 1, 2, 1, 'sleep';

        FROM spans
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 10 AS dur) PER cpu
        |> ORDER BY cpu, ts;
        """,
        out=Csv("""
        "ts","dur","cpu","state"
        0,2,0,"[NULL]"
        2,3,0,"run"
        5,5,0,"[NULL]"
        0,1,1,"[NULL]"
        1,2,1,"sleep"
        3,7,1,"[NULL]"
        """))

  # A background with the PER columns names the lanes: one without input is
  # filled whole, and input in a lane it does not name fills nothing.
  def test_interval_fill_gaps_background_names_the_lanes(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS
        SELECT 2 AS ts, 3 AS dur, 0 AS cpu, 'run' AS state
        UNION ALL SELECT 1, 2, 1, 'sleep';
        CREATE PERFETTO TABLE lanes AS
        SELECT 0 AS ts, 10 AS dur, 0 AS cpu, 'idle' AS state
        UNION ALL SELECT 0, 4, 2, 'off';

        FROM spans
        |> INTERVAL FILL GAPS WITH lanes PER cpu
        |> ORDER BY cpu, ts;
        """,
        out=Csv("""
        "ts","dur","cpu","state"
        0,2,0,"idle"
        2,3,0,"run"
        5,5,0,"idle"
        1,2,1,"sleep"
        0,4,2,"off"
        """))

  # Without input, the background is filled whole.
  def test_interval_fill_gaps_without_input(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS
        SELECT 0 AS ts, 0 AS dur, 'run' AS state WHERE 0;

        FROM spans
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 4 AS dur, 'idle' AS state);
        """,
        out=Csv("""
        "ts","dur","state"
        0,4,"idle"
        """))

  # Overlapping input covers its union, a dur of -1 covers to the end, and a
  # row with a null ts covers nothing.
  def test_interval_fill_gaps_coverage(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS
        SELECT 2 AS ts, 3 AS dur, 1 AS id
        UNION ALL SELECT 4, 3, 2
        UNION ALL SELECT 25, -1, 3
        UNION ALL SELECT NULL, 5, 4;

        FROM spans
        |> INTERVAL FILL GAPS WITH (
             SELECT 0 AS ts, 10 AS dur UNION ALL SELECT 20, 10)
        |> ORDER BY ts, id;
        """,
        out=Csv("""
        "ts","dur","id"
        "[NULL]",5,4
        0,2,"[NULL]"
        2,3,1
        4,3,2
        7,3,"[NULL]"
        20,5,"[NULL]"
        25,-1,3
        """))

  # An alias still reaches its columns.
  def test_interval_fill_gaps_keeps_aliases(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS SELECT 2 AS ts, 3 AS dur, 'run' AS state;

        FROM spans AS s
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 10 AS dur, 'idle' AS state)
        |> SELECT s.ts, s.state
        |> ORDER BY ts;
        """,
        out=Csv("""
        "ts","state"
        0,"idle"
        2,"run"
        5,"idle"
        """))

  # A dataframe column and a SQL one are brought to one type.
  def test_interval_fill_gaps_unifies_types(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS
        SELECT 1 AS ts, 2 AS dur, 1 AS cpu, 1.5 AS weight;

        FROM spans
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 5 AS dur, 0 AS weight)
        |> ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","cpu","weight"
        0,1,"[NULL]",0.000000
        1,2,1,1.500000
        3,2,"[NULL]",0.000000
        """))

  # PER columns are brought to one type even when they are not output.
  def test_interval_fill_gaps_unifies_unused_per_types(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS
        SELECT 2 AS ts, 3 AS dur, 0 AS cpu
        UNION ALL SELECT 1, 2, 1;
        CREATE PERFETTO TABLE lanes AS
        SELECT 0 AS ts, 10 AS dur, 0.0 AS cpu
        UNION ALL SELECT 0, 4, 1.0;

        FROM spans
        |> INTERVAL FILL GAPS WITH lanes PER cpu
        |> SELECT ts, dur
        |> ORDER BY ts, dur;
        """,
        out=Csv("""
        "ts","dur"
        0,1
        0,2
        1,2
        2,3
        3,1
        5,5
        """))

  def test_interval_fill_gaps_needs_background_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 0 AS ts, 1 AS dur)
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS dur);
        """,
        out=ExpectedError('expected the background to have a ts'))

  def test_interval_fill_gaps_needs_every_per_column_or_none(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 0 AS ts, 1 AS dur, 0 AS cpu, 0 AS state)
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 1 AS dur, 0 AS cpu)
           PER cpu, state;
        """,
        out=ExpectedError('every PER column or none'))

  def test_interval_fill_gaps_needs_background_columns_in_the_input(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 0 AS ts, 1 AS dur)
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 5 AS dur, 1 AS filled);
        """,
        out=ExpectedError("the input to have the background's 'filled'"))

  def test_interval_fill_gaps_refuses_strings_meeting_numbers(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        CREATE PERFETTO TABLE spans AS SELECT 2 AS ts, 3 AS dur, 'run' AS state;

        FROM spans
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 10 AS dur, 1 AS state);
        """,
        out=ExpectedError('holds strings in one input and numbers in another'))

  def test_interval_fill_gaps_refuses_a_negative_ts(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT -1 AS ts, 2 AS dur)
        |> INTERVAL FILL GAPS WITH (SELECT 0 AS ts, 10 AS dur);
        """,
        out=ExpectedError('ts is below zero'))

  def test_interval_fill_gaps_refuses_overlapping_background(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 0 AS ts, 1 AS dur)
        |> INTERVAL FILL GAPS WITH (
             SELECT 0 AS ts, 10 AS dur UNION ALL SELECT 5, 10);
        """,
        out=ExpectedError('overlap within a lane'))

  # SELECT, EXTEND and SET parse any expression, but only a column can be
  # computed yet.
  def test_computed_columns_are_not_supported_yet(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        PERFETTO PRAGMA pipelines = 1;
        FROM (SELECT 1 AS a)
        |> EXTEND a + 1 AS b;
        """,
        out=ExpectedError('a computed column is not supported yet'))

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
from python.generators.diff_tests.testing import DiffTestBlueprint, ExpectedError
from python.generators.diff_tests.testing import TestSuite


class PerfettoScatterMipmap(TestSuite):

  def test_scatter_mipmap_basic_and_stats(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE PERFETTO TABLE pts AS
            SELECT 1 AS id, 1.0 AS x, 1.0 AS y, 10.0 AS c
            UNION ALL SELECT 2, 2.0, 2.0, 20.0
            UNION ALL SELECT 3, 8.0, 8.0, NULL
            UNION ALL SELECT 4, 9.0, 9.0, 40.0;

          CREATE VIRTUAL TABLE t USING __intrinsic_scatter_mipmap(pts);

          SELECT
            count,
            out_count,
            out_x_min,
            out_x_max,
            out_y_min,
            out_y_max,
            out_c_min,
            out_c_max
          FROM t(0.0, 10.0, 0.0, 10.0, 2, 2);
        """,
        out=Csv("""
          "count","out_count","out_x_min","out_x_max","out_y_min","out_y_max","out_c_min","out_c_max"
          2,4,1.000000,9.000000,1.000000,9.000000,10.000000,40.000000
          2,4,1.000000,9.000000,1.000000,9.000000,10.000000,40.000000
        """))

  def test_scatter_mipmap_omitted_id_and_null_skipping(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE VIRTUAL TABLE t_sub USING __intrinsic_scatter_mipmap((
            SELECT NULL AS id, CAST(NULL AS REAL) AS x, 1.0 AS y, 10 AS c
            UNION ALL SELECT NULL, 2.0, 3.0, 20
            UNION ALL SELECT NULL, CAST(NULL AS REAL), 4.0, 30
            UNION ALL SELECT NULL, 7.0, 8.0, NULL
          ));

          SELECT id, x, y, c, count FROM t_sub(0.0, 10.0, 0.0, 10.0, 2, 2) ORDER BY id;
        """,
        out=Csv("""
          "id","x","y","c","count"
          1,2.000000,3.000000,20.000000,1
          3,7.000000,8.000000,"[NULL]",1
        """))

  def test_scatter_mipmap_auto_categories_listing_mode(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE PERFETTO TABLE pts AS
            SELECT 1 AS id, 1.0 AS x, 1.0 AS y, 'A' AS name
            UNION ALL SELECT 2, 2.0, 2.0, 'A'
            UNION ALL SELECT 3, 3.0, 3.0, 'A'
            UNION ALL SELECT 4, 4.0, 4.0, 'B'
            UNION ALL SELECT 5, 5.0, 5.0, 'B'
            UNION ALL SELECT 6, 6.0, 6.0, 'C'
            UNION ALL SELECT 7, 7.0, 7.0, 'D'
            UNION ALL SELECT 8, 8.0, 8.0, CAST(NULL AS STRING);

          CREATE VIRTUAL TABLE t_auto USING __intrinsic_scatter_mipmap(
            pts, id, x, y, name, 2
          );

          SELECT out_cat_idx, out_cat_value, out_cat_count FROM t_auto(NULL, NULL, NULL, NULL, 0, 0);
        """,
        out=Csv("""
          "out_cat_idx","out_cat_value","out_cat_count"
          0,"A",3
          1,"B",2
          2,"Other",2
          -1,"[NULL]",1
        """))

  def test_scatter_mipmap_slice_dataframe_fast_path(self):
    return DiffTestBlueprint(
        trace=TextProto("""
          packet {
            ftrace_events {
              cpu: 0
              event { timestamp: 1000 pid: 1 print { buf: "B|1|slice_A\\n" } }
              event { timestamp: 2000 pid: 1 print { buf: "E|1\\n" } }
              event { timestamp: 3000 pid: 1 print { buf: "B|1|slice_A\\n" } }
              event { timestamp: 4000 pid: 1 print { buf: "E|1\\n" } }
              event { timestamp: 5000 pid: 1 print { buf: "B|1|slice_B\\n" } }
              event { timestamp: 6000 pid: 1 print { buf: "E|1\\n" } }
              event { timestamp: 7000 pid: 1 print { buf: "B|1|slice_C\\n" } }
              event { timestamp: 8000 pid: 1 print { buf: "E|1\\n" } }
            }
          }
        """),
        query="""
          CREATE VIRTUAL TABLE t_slice USING __intrinsic_scatter_mipmap(
            slice, id, ts, dur, name, 1
          );

          SELECT out_cat_idx, out_cat_value, out_cat_count FROM t_slice(NULL, NULL, NULL, NULL, 0, 0);
        """,
        out=Csv("""
          "out_cat_idx","out_cat_value","out_cat_count"
          0,"slice_A",2
          1,"Other",2
        """))

  def test_scatter_mipmap_invalid_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE PERFETTO TABLE pts_err AS
            SELECT 1 AS id, 1.0 AS x, 1.0 AS y, 1.0 AS c;

          CREATE VIRTUAL TABLE t_err USING __intrinsic_scatter_mipmap(pts_err);

          SELECT * FROM t_err(10.0, 5.0, 0.0, 10.0, 2, 2);
        """,
        out=ExpectedError("scatter_mipmap: x_max must be greater than x_min\n"))

  def test_scatter_mipmap_hidden_categories_small(self):
    # Cell (0,0) mixes A, B and NULL; cell (1,1) only has B.
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE PERFETTO TABLE pts AS
            SELECT 1 AS id, 1.0 AS x, 1.0 AS y, 'A' AS name
            UNION ALL SELECT 2, 2.0, 2.0, 'A'
            UNION ALL SELECT 3, 3.0, 3.0, 'A'
            UNION ALL SELECT 4, 4.0, 4.0, 'B'
            UNION ALL SELECT 5, 4.5, 4.5, NULL
            UNION ALL SELECT 6, 8.0, 8.0, 'B';

          CREATE VIRTUAL TABLE t USING __intrinsic_scatter_mipmap(
            pts, id, x, y, name, 2);

          SELECT 'none' AS hidden, id, c, count
          FROM t(0.0, 10.0, 0.0, 10.0, 2, 2)
          UNION ALL
          SELECT 'A', id, c, count FROM t(0.0, 10.0, 0.0, 10.0, 2, 2, '0')
          UNION ALL
          SELECT 'B,NULL', id, c, count
          FROM t(0.0, 10.0, 0.0, 10.0, 2, 2, ' 1, -1 ')
          UNION ALL
          SELECT 'all', id, c, count
          FROM t(0.0, 10.0, 0.0, 10.0, 2, 2, '0,1,-1')
          UNION ALL
          SELECT 'empty', id, c, count FROM t(0.0, 10.0, 0.0, 10.0, 2, 2, '')
          UNION ALL
          SELECT 'unknown', id, c, count
          FROM t(0.0, 10.0, 0.0, 10.0, 2, 2, '99');
        """,
        out=Csv("""
          "hidden","id","c","count"
          "none",1,0.000000,5
          "none",6,1.000000,1
          "A",4,1.000000,2
          "A",6,1.000000,1
          "B,NULL",1,0.000000,3
          "empty",1,0.000000,5
          "empty",6,1.000000,1
          "unknown",1,0.000000,5
          "unknown",6,1.000000,1
        """))

  def test_scatter_mipmap_hidden_categories_brute_force(self):
    # 20k points with 7 string categories (+ NULLs) reduced to top-3 + Other.
    # Filtered per-cell counts must match a brute force GROUP BY and no
    # representative may belong to a hidden category.
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE PERFETTO TABLE pts AS
          WITH RECURSIVE r(i) AS (
            SELECT 0 UNION ALL SELECT i + 1 FROM r WHERE i < 19999
          )
          SELECT
            i AS id,
            (i * 7919) % 1000 AS x,
            (i * 104729) % 997 AS y,
            IIF(i % 11 = 0, NULL, 'k' || ((i * i) % 7)) AS name
          FROM r;

          CREATE VIRTUAL TABLE t USING __intrinsic_scatter_mipmap(
            pts, id, x, y, name, 3);

          CREATE PERFETTO TABLE pt AS
          WITH cats AS (
            SELECT out_cat_idx AS idx, out_cat_value AS v
            FROM t(0, 0, 0, 0, 0, 0)
          )
          SELECT
            p.id,
            MIN(36, CAST(p.x * 37 / 1000.0 AS INT)) AS cx,
            MIN(22, CAST(p.y * 23 / 1000.0 AS INT)) AS cy,
            IFNULL((SELECT idx FROM cats WHERE v = p.name),
                   IIF(p.name IS NULL, -1, 3)) AS idx
          FROM pts p;

          CREATE PERFETTO TABLE bf AS
          SELECT cx, cy, COUNT(*) AS n
          FROM pt
          WHERE idx NOT IN (0, 3, -1)
          GROUP BY cx, cy;

          CREATE PERFETTO TABLE op AS
          SELECT
            id,
            MIN(36, CAST(x * 37 / 1000.0 AS INT)) AS cx,
            MIN(22, CAST(y * 23 / 1000.0 AS INT)) AS cy,
            count AS n
          FROM t(0, 1000, 0, 1000, 37, 23, '0,3,-1');

          SELECT
            (SELECT COUNT(*) FROM op) AS op_cells,
            (SELECT COUNT(*) FROM bf) AS bf_cells,
            (SELECT SUM(n) FROM op) AS op_total,
            (SELECT SUM(n) FROM bf) AS bf_total,
            (SELECT COUNT(*) FROM bf LEFT JOIN op USING (cx, cy)
             WHERE op.n IS NULL OR op.n != bf.n)
            + (SELECT COUNT(*) FROM op LEFT JOIN bf USING (cx, cy)
               WHERE bf.n IS NULL) AS mismatched_cells,
            (SELECT COUNT(*) FROM op JOIN pt USING (id)
             WHERE pt.idx IN (0, 3, -1)) AS hidden_reps;
        """,
        out=Csv("""
          "op_cells","bf_cells","op_total","bf_total","mismatched_cells","hidden_reps"
          851,851,10388,10388,0,0
        """))

  def test_scatter_mipmap_hidden_null_numeric(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE PERFETTO TABLE pts AS
            SELECT 1 AS id, 1.0 AS x, 1.0 AS y, 10.0 AS c
            UNION ALL SELECT 2, 2.0, 2.0, NULL
            UNION ALL SELECT 3, 8.0, 8.0, NULL;

          CREATE VIRTUAL TABLE t USING __intrinsic_scatter_mipmap(pts);

          SELECT id, c, count FROM t(0.0, 10.0, 0.0, 10.0, 2, 2, '-1');
        """,
        out=Csv("""
          "id","c","count"
          1,10.000000,1
        """))

  def test_scatter_mipmap_hidden_invalid(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
          CREATE PERFETTO TABLE pts_err AS
            SELECT 1 AS id, 1.0 AS x, 1.0 AS y, 1.0 AS c;

          CREATE VIRTUAL TABLE t_err USING __intrinsic_scatter_mipmap(pts_err);

          SELECT * FROM t_err(0.0, 10.0, 0.0, 10.0, 2, 2, '1,x');
        """,
        out=ExpectedError(
            "scatter_mipmap: hidden must be a comma-separated list of "
            "category indexes >= -1\n"))

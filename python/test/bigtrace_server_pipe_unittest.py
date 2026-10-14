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
"""Tests `|>` chain syntax in DuckDB SQL mode (pf_compile_pipe)."""

import json
import os
import shutil
import tempfile
import unittest

import duckdb

from perfetto.bigtrace.server import load_extension
from test.bigtrace_server_unittest import TRACES, Server, make_traces

_TABLES = """
CREATE TABLE t AS FROM (VALUES
  (1, 'a', 10, 'x'), (2, 'b', 20, 'x'), (3, 'c', 30, 'y'),
  (4, 'd', 40, 'y'), (5, 'e', 50, NULL)) v(id, name, dur, grp);
CREATE TABLE u AS FROM (VALUES (1, 'one'), (2, 'two'), (9, 'nine')) v(id, label);
"""


class PipeCompilerTest(unittest.TestCase):
  """Compiles chain syntax and checks it against equivalent plain SQL."""

  @classmethod
  def setUpClass(cls):
    cls.con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    load_extension(cls.con)
    cls.con.execute(_TABLES)

  @classmethod
  def tearDownClass(cls):
    cls.con.close()

  def compile(self, sql):
    return self.con.execute("SELECT pf_compile_pipe(?)", [sql]).fetchone()[0]

  def rows(self, sql, ordered=False):
    rows = self.con.execute(sql).fetchall()
    return rows if ordered else sorted(rows, key=repr)

  def assertSameRows(self, pipe, plain, ordered=False):
    self.assertEqual(
        self.rows(self.compile(pipe), ordered), self.rows(plain, ordered))

  def assertSameColumns(self, pipe, columns):
    cur = self.con.execute(self.compile(pipe))
    self.assertEqual([d[0] for d in cur.description], columns)

  def assertCompileError(self, sql, message):
    with self.assertRaises(duckdb.Error) as cm:
      self.compile(sql)
    self.assertIn("pipe syntax error: " + message, str(cm.exception))

  def test_stages_fold_into_one_select(self):
    self.assertEqual(
        self.compile("FROM t AS s |> WHERE s.dur > 10 |> WHERE grp = 'y' "
                     "|> AGGREGATE count(*) AS n GROUP BY s.grp "
                     "|> ORDER BY n DESC |> LIMIT 3"),
        "SELECT * FROM (SELECT s.grp, count(*) AS n FROM t AS s "
        "WHERE (s.dur > 10) AND (grp = 'y') GROUP BY s.grp) "
        "ORDER BY n DESC LIMIT 3")
    self.assertEqual(
        self.compile("FROM t |> WHERE id > 1 |> ORDER BY id |> LIMIT 2"),
        "SELECT * FROM t WHERE (id > 1) ORDER BY id LIMIT 2")

  def test_where(self):
    self.assertSameRows("FROM t |> WHERE dur > 10 |> WHERE grp = 'y'",
                        "SELECT * FROM t WHERE dur > 10 AND grp = 'y'")

  def test_select(self):
    self.assertSameRows("FROM t |> SELECT id, dur * 2 AS d",
                        "SELECT id, dur * 2 AS d FROM t")
    self.assertSameColumns("FROM t |> SELECT * EXCEPT (name, grp)",
                           ["id", "dur"])
    self.assertSameRows("FROM t |> SELECT * REPLACE (dur + 1 AS dur)",
                        "SELECT id, name, dur + 1 AS dur, grp FROM t")
    self.assertSameRows("FROM t |> SELECT DISTINCT grp",
                        "SELECT DISTINCT grp FROM t")

  def test_extend(self):
    self.assertSameColumns("FROM t |> EXTEND dur * 2 AS d, 1 AS one",
                           ["id", "name", "dur", "grp", "d", "one"])
    # Extended columns can be used by later stages.
    self.assertSameRows("FROM t |> EXTEND dur * 2 AS d |> WHERE d > 50",
                        "SELECT *, dur * 2 AS d FROM t WHERE dur * 2 > 50")

  def test_set(self):
    self.assertSameRows("FROM t |> SET dur = dur + 1, name = upper(name)",
                        "SELECT id, upper(name), dur + 1, grp FROM t")
    # All values are read before any is set, so this swaps.
    self.assertSameRows("FROM t |> SELECT id, dur |> SET id = dur, dur = id",
                        "SELECT dur, id FROM t")

  def test_drop(self):
    self.assertSameColumns("FROM t |> DROP name, grp", ["id", "dur"])

  def test_rename(self):
    self.assertSameColumns("FROM t |> RENAME name AS n, dur d",
                           ["id", "n", "d", "grp"])
    self.assertSameRows(
        "FROM t |> SELECT id, dur |> RENAME id AS dur, dur AS id"
        " |> SELECT id", "SELECT dur FROM t")

  def test_aggregate(self):
    self.assertSameRows("FROM t |> AGGREGATE count(*) AS n, sum(dur) AS s",
                        "SELECT count(*), sum(dur) FROM t")
    # Keys come first, named by their alias.
    self.assertSameColumns(
        "FROM t |> AGGREGATE sum(dur) AS s GROUP BY grp, id % 2 AS odd",
        ["grp", "odd", "s"])
    self.assertSameRows(
        "FROM t |> AGGREGATE sum(dur) AS s GROUP BY grp, id % 2 AS odd",
        "SELECT grp, id % 2, sum(dur) FROM t GROUP BY grp, id % 2")
    self.assertSameRows("FROM t |> AGGREGATE GROUP BY grp",
                        "SELECT DISTINCT grp FROM t")
    # Filtering aggregates (HAVING) needs a subquery.
    self.assertSameRows(
        "FROM t |> AGGREGATE sum(dur) AS s GROUP BY grp |> WHERE s > 40",
        "SELECT grp, sum(dur) FROM t GROUP BY grp HAVING sum(dur) > 40")

  def test_join(self):
    self.assertSameRows("FROM t |> JOIN u USING (id)",
                        "SELECT * FROM t JOIN u USING (id)")
    self.assertSameRows(
        "FROM t AS a |> LEFT JOIN u AS b ON a.id = b.id "
        "|> WHERE b.label IS NULL |> SELECT a.id",
        "SELECT id FROM t WHERE id NOT IN (SELECT id FROM u)")
    # A pipeline as the joined relation, and both aliases in later stages.
    self.assertSameRows(
        "FROM t AS a |> JOIN (FROM u |> WHERE id > 1) AS b ON a.id = b.id "
        "|> AGGREGATE count(*) AS n GROUP BY b.label", "SELECT 'two', 1")
    self.assertSameRows(
        "FROM t |> WHERE id < 3 |> CROSS JOIN u |> AGGREGATE "
        "count(*) AS n", "SELECT 6")

  def test_order_by_sees_the_previous_stage_columns(self):
    # `name` is the output of `x.name`, and `d` is not an input column.
    self.assertSameRows(
        "FROM t AS x |> AGGREGATE sum(dur) AS d GROUP BY x.name "
        "|> ORDER BY name DESC |> LIMIT 2",
        "SELECT 'e', 50 UNION ALL SELECT 'd', 40",
        ordered=False)
    self.assertSameRows(
        "FROM t |> SELECT id * 10 AS id |> ORDER BY id DESC |> LIMIT 1",
        "SELECT 50")

  def test_order_by_and_limit(self):
    self.assertSameRows(
        "FROM t |> ORDER BY dur DESC |> LIMIT 2",
        "SELECT * FROM t ORDER BY dur DESC LIMIT 2",
        ordered=True)
    self.assertSameRows(
        "FROM t |> SELECT id |> ORDER BY id |> LIMIT 2 OFFSET 1",
        "SELECT id FROM t ORDER BY id LIMIT 2 OFFSET 1",
        ordered=True)
    # A WHERE after LIMIT filters the limited rows.
    self.assertSameRows(
        "FROM t |> ORDER BY id |> LIMIT 2 |> WHERE id > 1",
        "SELECT * FROM (SELECT * FROM t ORDER BY id LIMIT 2) WHERE id > 1")

  def test_table_alias_survives_subqueries(self):
    # `s.` stays valid after stages that need a subquery, until SELECT.
    self.assertSameRows(
        "FROM t AS s |> EXTEND dur * 2 AS d |> WHERE s.d > 50 |> SELECT s.id",
        "SELECT id FROM t WHERE dur * 2 > 50")
    # An unaliased table is its own alias.
    self.assertSameRows("FROM t |> EXTEND 1 AS one |> WHERE t.id = 1",
                        "SELECT *, 1 FROM t WHERE id = 1")

  def test_as(self):
    self.assertSameRows(
        "FROM t |> SELECT id, dur |> AS x |> WHERE x.dur > 20 |> SELECT x.id",
        "SELECT id FROM t WHERE dur > 20")

  def test_fork(self):
    # The stream continues unchanged, and the snapshot can be joined.
    self.assertSameRows(
        "FROM t |> WHERE grp IS NOT NULL |> FORK AS base "
        "|> AGGREGATE max(dur) AS dur GROUP BY grp "
        "|> JOIN base USING (grp, dur) |> SELECT id",
        "SELECT 2 UNION ALL SELECT 4")
    self.assertSameRows("FROM t AS s |> FORK AS f |> WHERE s.id = 1",
                        "SELECT * FROM t WHERE id = 1")

  def test_nested_pipelines(self):
    self.assertSameRows(
        "WITH big AS (FROM t |> WHERE dur > 20) "
        "SELECT id FROM big WHERE id IN (FROM u |> SELECT id + 3)",
        "SELECT 4 UNION ALL SELECT 5")
    self.assertSameRows(
        "WITH big AS (FROM t |> WHERE dur > 20) "
        "FROM big |> AGGREGATE count(*) AS n", "SELECT 3")
    self.assertSameRows("(SELECT * FROM t) |> WHERE id = 1",
                        "SELECT * FROM t WHERE id = 1")
    self.assertSameRows("SELECT * FROM t WHERE id = 1 |> SELECT name",
                        "SELECT 'a'")
    self.assertSameRows(
        "SELECT (FROM u |> AGGREGATE max(id) AS m) AS m, "
        "(SELECT count(*) FROM (FROM t |> WHERE grp = 'x')) AS n",
        "SELECT 9, 2")

  def test_text_without_pipes_is_unchanged(self):
    for sql in ("SELECT 1", "SELECT '|>' AS s -- |> x",
                "SELECT 1 /* |> */, \"a|>b\" FROM (SELECT 2 AS \"a|>b\")"):
      self.assertEqual(self.compile(sql), sql)

  def test_strings_and_comments_with_pipes(self):
    self.assertSameRows(
        "FROM t -- |> WHERE id = 1\n"
        "|> WHERE name != '|> x' /* |> LIMIT 1 */ |> SELECT '|>' AS s",
        "SELECT '|>' FROM t")
    self.assertSameRows("FROM t |> WHERE name = 'it''s |> fine'",
                        "SELECT * FROM t WHERE false")

  def test_duration_literals(self):
    self.assertSameRows(
        "FROM t |> SELECT 16ms AS a, 1.5s AS b, 2m AS c, 100ns AS d, 1h AS e",
        "SELECT 16000000, 1500000000, 120000000000, 100, 3600000000000 "
        "FROM t")
    # Strings, identifiers and plain numbers are left alone.
    self.assertSameRows("FROM t |> SELECT '16ms' AS s1, 2.5 AS x, 1e3 AS y",
                        "SELECT '16ms', 2.5, 1e3 FROM t")

  def test_multiple_statements(self):
    self.assertEqual(
        self.compile("SELECT 1; FROM t |> SELECT id;"),
        "SELECT 1;\nSELECT id FROM t")

  def test_null(self):
    self.assertIsNone(
        self.con.execute("SELECT pf_compile_pipe(NULL)").fetchone()[0])

  def test_errors(self):
    self.assertCompileError(
        "FROM t |> FILTER dur > 0",
        "unknown |> stage 'FILTER dur > 0': expected WHERE, SELECT, EXTEND, "
        "SET, DROP, RENAME, AGGREGATE, JOIN, ORDER BY, LIMIT, AS, FORK AS, "
        "INTERVAL ... or TREE ...")
    self.assertCompileError("FROM t |> |> SELECT 1", "empty |> stage")
    self.assertCompileError("FROM t |> SELECT id |>", "empty |> stage")
    self.assertCompileError("FROM t |> WHERE", "WHERE needs an argument")
    self.assertCompileError("FROM t |> SET dur",
                            "SET expects 'column = expression', got 'dur'")
    self.assertCompileError("FROM t |> SET dur = 1, x",
                            "SET expects 'column = expression', got 'x'")
    self.assertCompileError("FROM t |> RENAME a b c",
                            "RENAME expects 'column AS new_name', got 'a b c'")
    self.assertCompileError(
        "FROM t |> DROP a b",
        "DROP expects a list of column names, got 'DROP a b'")
    self.assertCompileError("FROM t |> DROP a,",
                            "DROP expects a list of column names")
    self.assertCompileError("FROM t |> AS", "AS expects a table alias")
    self.assertCompileError("FROM t |> FORK x",
                            "FORK expects 'FORK AS name', got 'FORK x'")
    self.assertCompileError("FROM t |> AGGREGATE",
                            "AGGREGATE needs aggregates or a GROUP BY")
    self.assertCompileError(
        "CREATE TABLE x AS FROM t |> WHERE id = 1",
        "a pipeline starts with FROM <table> or a query, got "
        "'CREATE TABLE x AS FROM t'")
    self.assertCompileError("FROM t |> WHERE name = 'x",
                            "unterminated string literal")
    self.assertCompileError("FROM t |> WHERE (id = 1", "unbalanced '('")
    self.assertCompileError("FROM t |> WHERE id = 1)", "unbalanced ')'")
    self.assertCompileError("FROM t /* |> x", "unterminated /* comment")


class BigtraceServerPipeTest(unittest.TestCase):
  """Chain syntax over tables extracted from several traces."""

  @classmethod
  def setUpClass(cls):
    cls.tmp = tempfile.mkdtemp()
    make_traces(cls.tmp)
    manifest = os.path.join(cls.tmp, "bigtrace_manifest.json")
    with open(manifest, "w") as f:
      json.dump(
          {
              "traces": [{
                  "file_path": t,
                  "tag": t.split("_")[2].split(".")[0],
                  "device_name": "pixel"
              } for t in TRACES]
          }, f)
    cls.server = Server(cls.tmp)
    cls.server.ok(
        "POST", "/execute_bigtrace_query", {
            "queries": [
                {
                    "perfetto_sql":
                        "SELECT s.ts, s.dur, s.name, s.depth, tt.utid "
                        "FROM slice s JOIN thread_track tt "
                        "ON s.track_id = tt.id WHERE s.dur >= 0",
                    "table_name": "pipe_slices",
                },
                {
                    "perfetto_sql": "SELECT utid, tid, name FROM thread",
                    "table_name": "pipe_threads",
                },
            ],
            "limit": 1000000,
        })

  @classmethod
  def tearDownClass(cls):
    cls.server.stop()
    shutil.rmtree(cls.tmp)

  def _duckdb(self, sql):
    status, res, _ = self.server.call(
        "POST", "/execute_bigtrace_query", {
            "engine": "duckdb",
            "materialized": False,
            "perfetto_sql": sql,
            "limit": 100000
        })
    return status, res

  def _rows(self, sql):
    status, res = self._duckdb(sql)
    self.assertEqual(status, 200, res)
    return [r["values"] for r in res["rows"]]

  def assertSameRows(self, pipe, plain):
    rows = self._rows(pipe)
    self.assertGreater(len(rows), 0)
    self.assertEqual(
        sorted(rows, key=repr), sorted(self._rows(plain), key=repr))

  def test_per_trace_aggregation(self):
    self.assertSameRows(
        """FROM pipe_slices
           |> WHERE depth = 0
           |> AGGREGATE count(*) AS slices, max(dur) AS max_dur
              GROUP BY _trace_uuid""", """
        SELECT _trace_uuid, count(*), max(dur) FROM pipe_slices
        WHERE depth = 0 GROUP BY _trace_uuid""")

  def test_join_with_trace_metadata_and_per_trace_tables(self):
    self.assertSameRows(
        """FROM pipe_slices AS s
           |> JOIN pipe_threads AS t USING (_trace_uuid, utid)
           |> JOIN trace_metadata AS m USING (_trace_uuid)
           |> WHERE t.name ILIKE 'binder%'
           |> AGGREGATE sum(s.dur) AS binder_thread_dur
              GROUP BY m.tag, m.device_name
           |> ORDER BY binder_thread_dur DESC""", """
        SELECT m.tag, m.device_name, sum(s.dur)
        FROM pipe_slices s
        JOIN pipe_threads t USING (_trace_uuid, utid)
        JOIN trace_metadata m USING (_trace_uuid)
        WHERE t.name ILIKE 'binder%'
        GROUP BY m.tag, m.device_name""")

  def test_top_slices_per_trace_with_a_window(self):
    self.assertSameRows(
        """FROM pipe_slices
           |> EXTEND row_number() OVER (
                PARTITION BY _trace_uuid ORDER BY dur DESC, ts) AS rank
           |> WHERE rank <= 3
           |> SELECT _trace_uuid, rank, name, dur""", """
        SELECT _trace_uuid, rank, name, dur FROM (
          SELECT *, row_number() OVER (
            PARTITION BY _trace_uuid ORDER BY dur DESC, ts) AS rank
          FROM pipe_slices)
        WHERE rank <= 3""")

  def test_syntax_errors_fail_the_query(self):
    status, res = self._duckdb("FROM pipe_slices |> FILTER dur > 0")
    self.assertEqual(status, 400, res)
    self.assertIn("pipe syntax error: unknown |> stage 'FILTER dur > 0'",
                  res["detail"])


if __name__ == "__main__":
  unittest.main()

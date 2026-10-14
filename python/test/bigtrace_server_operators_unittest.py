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
"""Interval, tree and graph operators in DuckDB SQL mode.

Runs the operators' own tests (duckdb_ext/tests) and checks them, through
the server, against trace processor on real traces, one result per trace.
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import duckdb

from perfetto.bigtrace.server import EXT_DIR, build_extension, load_extension
from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig
from test.bigtrace_server_unittest import ROOT, TRACES, Server, make_traces

_TESTS_DIR = os.path.join(EXT_DIR, "tests")
_HEAP_TRACE = "system-server-heap-graph-new.pftrace"

_SLICES = ("SELECT s.id, s.parent_id, s.ts, s.dur, s.depth, s.name, "
           "s.track_id, tt.utid FROM slice s "
           "JOIN thread_track tt ON s.track_id = tt.id WHERE s.dur >= 0")
_SCHED = "SELECT id, ts, dur, cpu, utid FROM sched WHERE utid != 0 AND dur >= 0"
_THREAD_STATE = (
    "SELECT id, ts, dur, state, utid FROM thread_state WHERE dur >= 0")


def _row(tp: TraceProcessor, sql: str) -> list:
  """The first row of `sql`, as ints."""
  return [int(v) for v in vars(next(iter(tp.query(sql)))).values()]


def _per_trace_from_tp(sql: str, setup: str = "") -> dict:
  """Runs `sql` (one row of numbers) on each trace: {file name: row}."""
  out = {}
  for name in TRACES:
    tp = TraceProcessor(
        trace=os.path.join(ROOT, "test", "data", name),
        config=TraceProcessorConfig(bin_path=os.environ.get("SHELL_PATH")))
    try:
      if setup:
        tp.query(setup)
      out[name] = _row(tp, sql)
    finally:
      tp.close()
  return out


class OperatorKernelTest(unittest.TestCase):
  """The operators' tests against brute-force references (duckdb_ext/tests)."""

  def _run(self, script):
    env = dict(os.environ, PF_EXT=build_extension())
    proc = subprocess.run(
        [sys.executable, os.path.join(_TESTS_DIR, script)],
        env=env,
        capture_output=True,
        text=True,
        timeout=1200)
    self.assertEqual(proc.returncode, 0, (proc.stdout + proc.stderr)[-4000:])

  def test_intervals(self):
    self._run("test_intervals.py")

  def test_trees(self):
    self._run("test_trees.py")

  def test_graphs(self):
    self._run("test_graphs.py")

  def test_partition_kernels(self):
    self._run("test_partition_kernels.py")

  def test_every_script_is_run(self):
    scripts = {
        os.path.basename(p)
        for p in glob.glob(os.path.join(_TESTS_DIR, "test_*.py"))
    }
    self.assertEqual(
        scripts, {
            "test_intervals.py", "test_trees.py", "test_graphs.py",
            "test_partition_kernels.py"
        })


class BigtraceServerOperatorsTest(unittest.TestCase):
  """Operators over tables extracted from three traces, vs trace processor."""

  @classmethod
  def setUpClass(cls):
    cls.tmp = tempfile.mkdtemp()
    make_traces(cls.tmp)
    cls.server = Server(cls.tmp)
    cls.server.ok(
        "POST", "/execute_bigtrace_query", {
            "queries": [
                {
                    "perfetto_sql": _SLICES,
                    "table_name": "op_slices"
                },
                {
                    "perfetto_sql": _SCHED,
                    "table_name": "op_sched"
                },
                {
                    "perfetto_sql": _THREAD_STATE,
                    "table_name": "op_thread_state"
                },
            ],
            "limit": 0,
        })

  @classmethod
  def tearDownClass(cls):
    cls.server.stop()
    shutil.rmtree(cls.tmp)

  def _duckdb(self, sql):
    status, res, _ = self.server.call("POST", "/execute_bigtrace_query", {
        "engine": "duckdb",
        "materialized": False,
        "perfetto_sql": sql,
        "limit": 0
    })
    return status, res

  def _per_trace(self, sql):
    """Runs `sql`, whose first column is the trace's file name."""
    status, res = self._duckdb(sql)
    self.assertEqual(status, 200, res)
    return {
        r["values"][0]: [int(float(v)) for v in r["values"][1:]]
        for r in res["rows"]
    }

  def test_interval_intersection_matches_trace_processor(self):
    want = _per_trace_from_tp(
        "SELECT count(*), sum(dur) FROM _interval_intersect!("
        "(cmp_slices, cmp_sched), (utid))",
        "INCLUDE PERFETTO MODULE intervals.intersect;"
        f"CREATE PERFETTO TABLE cmp_slices AS {_SLICES};"
        f"CREATE PERFETTO TABLE cmp_sched AS {_SCHED};")
    pipe = self._per_trace("""
        INTERVAL INTERSECTION OF (op_slices AS s, op_sched AS c)
          PER _trace_uuid, utid
        |> JOIN trace_metadata USING (_trace_uuid)
        |> AGGREGATE count(*) AS n, sum(dur) AS d GROUP BY file_name""")
    macro = self._per_trace("""
        SELECT file_name, count(*), sum(dur)
        FROM pf_interval_intersection(['op_slices', 'op_sched'], ['s', 'c'],
                                      per := ['_trace_uuid', 'utid'])
        JOIN trace_metadata USING (_trace_uuid)
        GROUP BY file_name""")
    self.assertEqual(pipe, want)
    self.assertEqual(macro, want)

  def test_3_way_interval_intersection_matches_trace_processor(self):
    want = _per_trace_from_tp(
        "SELECT count(*), sum(dur) FROM _interval_intersect!("
        "(cmp_slices, cmp_sched, cmp_ts), (utid))",
        "INCLUDE PERFETTO MODULE intervals.intersect;"
        f"CREATE PERFETTO TABLE cmp_slices AS {_SLICES};"
        f"CREATE PERFETTO TABLE cmp_sched AS {_SCHED};"
        f"CREATE PERFETTO TABLE cmp_ts AS {_THREAD_STATE};")
    got = self._per_trace("""
        INTERVAL INTERSECTION OF (
          op_slices AS s, op_sched AS c, op_thread_state AS st
        ) PER _trace_uuid, utid
        |> JOIN trace_metadata USING (_trace_uuid)
        |> AGGREGATE count(*) AS n, sum(dur) AS d GROUP BY file_name""")
    self.assertEqual(got, want)

  def test_interval_flatten_conserves_busy_time(self):
    want = self._per_trace("""
        FROM op_slices |> JOIN trace_metadata USING (_trace_uuid)
        |> AGGREGATE sum(dur) AS d GROUP BY file_name""")
    got = self._per_trace("""
        FROM op_slices
        |> INTERVAL FLATTEN PER _trace_uuid, track_id
           AGGREGATE count(*) AS active
        |> JOIN trace_metadata USING (_trace_uuid)
        |> AGGREGATE sum(dur * active) AS d GROUP BY file_name""")
    self.assertEqual(got, want)

  def test_tree_accumulate_and_keep_if_match_trace_processor(self):
    want = _per_trace_from_tp("""
        WITH RECURSIVE s AS (
          SELECT s.id, s.parent_id, s.dur FROM slice s
          JOIN thread_track tt ON s.track_id = tt.id WHERE s.dur >= 0
        ), sub(root_id, id, dur) AS (
          SELECT id, id, dur FROM s
          UNION ALL
          SELECT sub.root_id, c.id, c.dur FROM sub JOIN s c ON c.parent_id = sub.id
        )
        SELECT
          (SELECT count(DISTINCT root_id) FROM sub),
          (SELECT sum(dur) FROM sub),
          (SELECT count(*) FROM s WHERE parent_id IS NOT NULL),
          (SELECT sum(dur) FROM s WHERE parent_id IS NOT NULL)
    """)
    got = self._per_trace("""
        WITH acc AS (
          FROM op_slices
          |> TREE ACCUMULATE UP SUM(dur) AS total PER _trace_uuid
          |> AGGREGATE count(*) AS n, sum(total) AS total GROUP BY _trace_uuid
        ), descendants AS (
          FROM op_slices
          |> TREE KEEP IF DESCENDANT OF (
               FROM op_slices |> WHERE parent_id IS NULL) PER _trace_uuid
          |> AGGREGATE count(*) AS n, sum(dur) AS d GROUP BY _trace_uuid
        )
        FROM acc
        |> JOIN descendants USING (_trace_uuid)
        |> JOIN trace_metadata USING (_trace_uuid)
        |> SELECT file_name, acc.n, acc.total, descendants.n, descendants.d""")
    self.assertEqual(got, want)

  def test_heap_graph_dominator_tree_matches_trace_processor(self):
    tp = TraceProcessor(
        trace=os.path.join(ROOT, "test", "data", _HEAP_TRACE),
        config=TraceProcessorConfig(bin_path=os.environ.get("SHELL_PATH")))
    try:
      want = _row(
          tp, """
          INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;
          SELECT count(*), sum(dominated_size_bytes),
                 max(dominated_size_bytes)
          FROM heap_graph_dominator_tree""")
      want_top = [(int(r.id), int(r.dominated_size_bytes)) for r in tp.query("""
          SELECT id, dominated_size_bytes FROM heap_graph_dominator_tree
          ORDER BY dominated_size_bytes DESC, id LIMIT 10""")]
    finally:
      tp.close()

    with tempfile.TemporaryDirectory() as tmp:
      make_traces(tmp, [_HEAP_TRACE])
      srv = Server(tmp)
      try:
        srv.ok(
            "POST", "/execute_bigtrace_query", {
                "queries": [
                    {
                        "perfetto_sql":
                            "SELECT o.id, o.upid, o.graph_sample_ts, "
                            "o.self_size, c.name AS class_name "
                            "FROM heap_graph_object o "
                            "JOIN heap_graph_class c ON o.type_id = c.id "
                            "WHERE o.reachable != 0",
                        "table_name": "heap_objects",
                    },
                    {
                        "perfetto_sql":
                            "INCLUDE PERFETTO MODULE "
                            "android.memory.heap_graph.excluded_refs; "
                            "SELECT o.upid, o.graph_sample_ts, "
                            "r.owner_id AS src, r.owned_id AS dst "
                            "FROM heap_graph_reference r "
                            "JOIN heap_graph_object o ON r.owner_id = o.id "
                            "WHERE o.reachable != 0 "
                            "AND r.owned_id IS NOT NULL "
                            "AND r.id NOT IN _excluded_refs",
                        "table_name": "heap_refs",
                    },
                    {
                        "perfetto_sql": "SELECT id, upid, graph_sample_ts "
                                        "FROM heap_graph_object "
                                        "WHERE root_type IS NOT NULL",
                        "table_name": "gc_roots",
                    },
                ],
                "limit": 0,
            })

        def run_duckdb(sql):
          res = srv.ok(
              "POST", "/execute_bigtrace_query", {
                  "engine": "duckdb",
                  "materialized": False,
                  "perfetto_sql": sql,
                  "limit": 0
              })
          return [[int(float(v)) for v in r["values"]] for r in res["rows"]]

        retained = """
            GRAPH DOMINATOR TREE NODES heap_objects EDGES heap_refs
              FROM gc_roots PER _trace_uuid, upid, graph_sample_ts
            |> TREE ACCUMULATE UP SUM(self_size) AS retained
               PER _trace_uuid, upid, graph_sample_ts"""
        self.assertEqual(
            run_duckdb(retained + """
            |> AGGREGATE count(*) AS n, sum(retained) AS s,
                 max(retained) AS m"""), [want])
        self.assertEqual(
            run_duckdb(retained + """
            |> ORDER BY retained DESC, id |> LIMIT 10
            |> SELECT id, retained"""), [list(r) for r in want_top])
        macro = run_duckdb("""
            SELECT count(*), sum(total), max(total)
            FROM pf_tree_accumulate_up(
              '(SELECT * FROM pf_graph_dominator_tree(''heap_objects'',
                ''heap_refs'', ''gc_roots'',
                per := [''_trace_uuid'', ''upid'', ''graph_sample_ts'']))',
              'self_size', per := ['_trace_uuid', 'upid', 'graph_sample_ts'])
        """)
        self.assertEqual(macro, [want])
      finally:
        srv.stop()

  def test_operator_errors_name_the_stage(self):
    status, res = self._duckdb("FROM op_slices |> INTERVAL SQUASH")
    self.assertEqual(status, 400, res)
    self.assertIn("unknown INTERVAL stage 'INTERVAL SQUASH'", res["detail"])
    status, res = self._duckdb(
        "FROM op_slices |> TREE ACCUMULATE SIDEWAYS SUM(dur) AS t")
    self.assertEqual(status, 400, res)
    self.assertIn("needs UP or DOWN", res["detail"])


class OperatorPipeCompileTest(unittest.TestCase):
  """Compiler errors and detection of the INTERVAL / TREE / GRAPH syntax."""

  @classmethod
  def setUpClass(cls):
    cls.con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    load_extension(cls.con)

  @classmethod
  def tearDownClass(cls):
    cls.con.close()

  def assertCompileError(self, sql, message):
    with self.assertRaises(duckdb.Error) as cm:
      self.con.execute("SELECT pf_compile_pipe(?)", [sql]).fetchone()
    self.assertIn("pipe syntax error: ", str(cm.exception))
    self.assertIn(message, str(cm.exception))

  def test_errors(self):
    self.assertCompileError(
        "FROM t |> INTERVAL SQUASH",
        "unknown INTERVAL stage 'INTERVAL SQUASH': expected INTERVAL "
        "FLATTEN, MERGE, SPLIT, SUBTRACT, FILL GAPS, SET BOUNDS, QUANTIZE, "
        "JOIN, FIND, KEEP IF or DROP IF")
    self.assertCompileError("FROM t |> LEFT INTERVAL FLATTEN",
                            "unknown INTERVAL stage")
    self.assertCompileError("FROM t |> TREE GROW", "unknown TREE stage")
    self.assertCompileError("FROM t |> INTERVAL JOIN",
                            "'INTERVAL JOIN' needs a relation")
    self.assertCompileError(
        "FROM t |> INTERVAL FLATTEN PER cpu FOO",
        "unexpected 'FOO' in 'INTERVAL FLATTEN PER cpu FOO'")
    self.assertCompileError("FROM t |> INTERVAL MERGE CONSECUTIVE PER cpu",
                            "needs BY columns")
    self.assertCompileError("FROM t |> TREE ACCUMULATE SUM(v) AS s",
                            "needs UP or DOWN")
    self.assertCompileError("GRAPH BFS TREE NODES n EDGES e",
                            "needs NODES, EDGES and FROM relations")
    self.assertCompileError("INTERVAL INTERSECTION OF a, b",
                            "expected '(' after 'INTERVAL INTERSECTION OF'")

  def test_operator_sources_need_no_pipe(self):
    sql = self.con.execute(
        "SELECT pf_compile_pipe('INTERVAL INTERSECTION OF (a, b) PER utid')"
    ).fetchone()[0]
    self.assertIn("pf_interval_intersect2_lane_agg", sql)
    # DuckDB's own INTERVAL literals are left alone.
    self.assertEqual(
        self.con.execute(
            "SELECT pf_compile_pipe('SELECT INTERVAL 1 DAY')").fetchone()[0],
        "SELECT INTERVAL 1 DAY")


if __name__ == "__main__":
  unittest.main()

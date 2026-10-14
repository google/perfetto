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
"""End-to-end tests: loads the built extension into the duckdb Python package
and runs queries on a real trace.

Also runs every example in the "Chain syntax" section of README.md and checks
its shown output, so that the docs can't rot.

Usage (needs `pip install duckdb==1.5.6`):
  contrib/duckdb-perfetto/e2e_test.py [--extension out/duckdb_ext/...]
"""

import argparse
import os
import re
import sys
import unittest

# Python loads _duckdb.so with RTLD_LOCAL, which would hide DuckDB's symbols
# from the extension.
sys.setdlopenflags(os.RTLD_GLOBAL | os.RTLD_NOW)
import duckdb  # pylint: disable=wrong-import-position

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
README = os.path.join(ROOT, 'contrib', 'duckdb-perfetto', 'README.md')
ARGS = None


def connect():
  con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
  con.execute(f"LOAD '{ARGS.extension}'")
  con.execute(f"ATTACH '{ARGS.trace}' AS t (TYPE perfetto)")
  return con


def format_result(cur):
  """Formats a result as an aligned text table, or an EXPLAIN as its plan."""
  cols = [d[0] for d in cur.description]
  if cols == ['explain_key', 'explain_value']:
    return cur.fetchall()[0][1]
  rows = [['NULL' if v is None else str(v) for v in r] for r in cur.fetchall()]
  widths = [max(len(r[i]) for r in [cols] + rows) for i in range(len(cols))]
  line = lambda r: ' | '.join(c.ljust(w) for c, w in zip(r, widths)).rstrip()
  sep = '-+-'.join('-' * w for w in widths)
  return '\n'.join([line(cols), sep] + [line(r) for r in rows])


def readme_examples():
  """(sql, expected output) for each ```sql block of the "Chain syntax"
  section which is directly followed by a ```text block."""
  with open(README) as f:
    text = f.read()
  section = text.split('\n## Chain syntax\n', 1)[1].split('\n## ', 1)[0]
  return re.findall(r'```sql\n(.*?)```\n+```text\n(.*?)```', section, re.S)


class ChainTest(unittest.TestCase):

  @classmethod
  def setUpClass(cls):
    cls.con = connect()
    cls.con.execute("CREATE TABLE names AS SELECT * FROM (VALUES "
                    "('binder reply', 'binder'), ('drawing', 'ui')) "
                    "AS v(name, kind)")

  def rows(self, q):
    return self.con.execute(q).fetchall()

  def assertSameRows(self, pipeline, plain, ordered=False):
    got, want = self.rows(pipeline), self.rows(plain)
    self.assertGreater(len(want), 0, plain)
    if not ordered:
      got, want = sorted(got, key=repr), sorted(want, key=repr)
    self.assertEqual(got, want)

  def assertParserError(self, q, msg, caret_at):
    with self.assertRaises(duckdb.ParserException) as cm:
      self.con.execute(q)
    err = str(cm.exception)
    self.assertIn(msg, err)
    # DuckDB prints the query after "LINE 1: " and a caret under the error.
    lines = err.splitlines()
    query_line = next(l for l in lines if l.startswith('LINE 1: '))
    caret = lines[lines.index(query_line) + 1].index('^') - len('LINE 1: ')
    self.assertEqual(q[caret:caret + len(caret_at)], caret_at)

  BLOCK = 'PERFETTO(t)(SELECT id, name, dur, depth FROM slice)'

  def test_where(self):
    self.assertSameRows('FROM t.slice |> WHERE dur > 1000000',
                        'SELECT * FROM t.slice WHERE dur > 1000000')
    self.assertSameRows(
        f'FROM {self.BLOCK} |> WHERE dur > 1000000 AND name LIKE \'b%\'',
        f'SELECT * FROM {self.BLOCK} WHERE dur > 1000000 AND name LIKE \'b%\'')

  def test_select_extend_set_drop_rename(self):
    b = self.BLOCK
    self.assertSameRows(f'FROM {b} |> SELECT name, dur / 1e3 AS us',
                        f'SELECT name, dur / 1e3 AS us FROM {b}')
    self.assertSameRows(
        f'FROM {b} |> EXTEND dur / 1e3 AS us',
        f'SELECT id, name, dur, depth, dur / 1e3 AS us FROM {b}')
    self.assertSameRows(f'FROM {b} |> SET name = upper(name), dur = 0',
                        f'SELECT id, upper(name), 0, depth FROM {b}')
    self.assertSameRows(f'FROM {b} |> DROP name, depth',
                        f'SELECT id, dur FROM {b}')
    self.assertSameRows(f'FROM {b} |> RENAME dur AS duration',
                        f'SELECT id, name, dur AS duration, depth FROM {b}')
    cur = self.con.execute(f'FROM {b} |> RENAME dur AS duration')
    self.assertEqual([d[0] for d in cur.description],
                     ['id', 'name', 'duration', 'depth'])

  def test_aggregate(self):
    b = self.BLOCK
    self.assertSameRows(f'FROM {b} |> AGGREGATE count(*) AS n, max(dur) AS m',
                        f'SELECT count(*), max(dur) FROM {b}')
    self.assertSameRows(
        f'FROM {b} |> AGGREGATE count(*) AS n GROUP BY name, depth AS d',
        f'SELECT name, depth, count(*) FROM {b} GROUP BY name, depth')

  def test_join(self):
    b = self.BLOCK
    self.assertSameRows(f'FROM {b} |> JOIN names USING (name)',
                        f'SELECT * FROM {b} JOIN names USING (name)')
    self.assertSameRows(
        f'FROM {b} AS s |> LEFT JOIN names AS n ON s.name = n.name '
        f'|> WHERE depth = 0 |> SELECT s.id, kind',
        f'SELECT s.id, n.kind FROM {b} AS s LEFT JOIN names AS n '
        f'ON s.name = n.name WHERE depth = 0')

  def test_order_by_limit(self):
    self.assertSameRows(
        f'FROM {self.BLOCK} |> ORDER BY dur DESC, id |> LIMIT 5 OFFSET 1',
        f'SELECT * FROM {self.BLOCK} ORDER BY dur DESC, id LIMIT 5 OFFSET 1',
        ordered=True)
    # Order survives later stages which don't reorder.
    self.assertSameRows(
        f'FROM {self.BLOCK} |> ORDER BY dur DESC |> SELECT dur |> LIMIT 5',
        f'SELECT dur FROM {self.BLOCK} ORDER BY dur DESC LIMIT 5',
        ordered=True)

  def test_alias(self):
    self.assertSameRows(
        f'FROM {self.BLOCK} |> WHERE depth > 0 |> AS x |> SELECT x.name',
        f'SELECT name FROM {self.BLOCK} WHERE depth > 0')

  def test_sources(self):
    self.assertSameRows('FROM t.thread |> SELECT utid, name',
                        'SELECT utid, name FROM t.thread')
    self.assertSameRows('FROM (SELECT * FROM t.slice LIMIT 10) |> DROP name',
                        'SELECT * EXCLUDE (name) FROM t.slice LIMIT 10')
    self.assertSameRows(f'{self.BLOCK} |> WHERE depth = 3',
                        f'SELECT * FROM {self.BLOCK} WHERE depth = 3')

  def test_nesting(self):
    self.assertSameRows(
        'FROM t.thread |> WHERE utid IN (FROM t.thread_track '
        '|> WHERE id IN (FROM t.slice |> SELECT track_id) |> SELECT utid)',
        'SELECT * FROM t.thread WHERE utid IN (SELECT utid FROM '
        't.thread_track WHERE id IN (SELECT track_id FROM t.slice))')
    self.assertSameRows(
        'WITH s AS (FROM t.slice |> WHERE depth = 0) SELECT count(*) FROM s',
        'SELECT count(*) FROM t.slice WHERE depth = 0')

  def test_block_with_tp_pipes(self):
    tp = ('PERFETTO(t)(PERFETTO PRAGMA pipelines = 1; '
          'FROM slice |> SELECT id, name, dur)')
    self.assertSameRows(f'FROM {tp} |> WHERE dur > 1000000 |> SELECT name',
                        'SELECT name FROM t.slice WHERE dur > 1000000')

  def test_untouched(self):
    self.assertEqual(self.rows("SELECT '|>' || 'x' -- |>"), [('|>x',)])
    self.assertEqual(self.rows('SELECT 6 | 3'), [(7,)])

  def explain(self, q):
    return self.con.execute('EXPLAIN ' + q).fetchall()[0][1]

  def test_pushdown(self):
    # A leading WHERE / SELECT after a block is pushed into TP...
    q = f'FROM {self.BLOCK} |> WHERE dur > 1000000 |> SELECT name'
    plan = self.explain(q)
    self.assertIn('PerfettoSQL Filters', plan)
    self.assertIn('"dur" > 1000000', plan)
    self.assertNotIn('FILTER', plan)
    self.assertRegex(plan, r'Projections:\s+name')
    self.assertSameRows(
        q, 'FROM PERFETTO(t)(SELECT name FROM slice WHERE dur > 1000000)')
    # The block's row order survives the pushdown.
    self.assertSameRows(
        'FROM PERFETTO(t)(SELECT id, dur FROM slice ORDER BY dur DESC, id) '
        '|> WHERE dur > 10000 |> SELECT id',
        'SELECT id FROM t.slice WHERE dur > 10000 ORDER BY dur DESC, id',
        ordered=True)
    # ... and after a table.
    self.assertIn('PerfettoSQL Filters',
                  self.explain('FROM t.slice |> WHERE depth = 0'))
    # Only translatable predicates are pushed; the rest stay in DuckDB.
    plan = self.explain(f'FROM {self.BLOCK} |> WHERE dur > 0 AND abs(id) > 1')
    self.assertIn('"dur" > 0', plan)
    self.assertIn('FILTER', plan)
    # A block ending in a TP pipeline can't be wrapped: no pushdown, same
    # results.
    tp = ('PERFETTO(t)(PERFETTO PRAGMA pipelines = 1; '
          'FROM slice |> SELECT name, dur)')
    plan = self.explain(f'FROM {tp} |> WHERE dur > 1000000')
    self.assertNotIn('PerfettoSQL Filters', plan)
    self.assertIn('FILTER', plan)

  def test_pushdown_multi_statement_and_multi_trace(self):
    self.assertSameRows(
        'FROM PERFETTO(t)(CREATE PERFETTO TABLE big AS '
        'SELECT * FROM slice WHERE dur > 1000; SELECT name, dur FROM big) '
        '|> WHERE dur > 1000000 |> AGGREGATE count(*) AS n',
        'SELECT count(*) FROM t.slice WHERE dur > 1000000')
    paths = f"['{ARGS.trace}', '{ARGS.trace}']"
    q = (f"FROM perfetto_query({paths}, 'SELECT name, dur FROM slice') "
         f"|> WHERE dur > 1000000 |> AGGREGATE count(*) AS n GROUP BY trace")
    self.assertIn('PerfettoSQL Filters', self.explain(q))
    n = self.rows('SELECT count(*) FROM t.slice WHERE dur > 1000000')[0][0]
    self.assertEqual(self.rows(q), [(ARGS.trace, 2 * n)])

  def test_errors(self):
    self.assertParserError('SELECT * FROM t.slice |> WHERE dur > 0',
                           '|> must follow a pipeline source', '|>')
    self.assertParserError(
        'FROM PERFETTO(t)(SELECT 1 AS x) |> WHERE x = 1 |> FROB x',
        'unknown pipe stage', 'FROB')
    self.assertParserError(
        'FROM PERFETTO(t)(SELECT * FROM slice) '
        '|> TREE ACCUMULATE UP SUM(dur) AS s',
        'only available inside PERFETTO blocks', 'TREE')
    self.assertParserError('FROM t.slice WHERE dur > 0 |> LIMIT 1',
                           'a pipeline must start from a table', 't.slice')

  def test_readme_examples(self):
    examples = readme_examples()
    self.assertGreaterEqual(len(examples), 5)
    for sql, want in examples:
      with self.subTest(sql=sql):
        con = connect()
        try:
          got = format_result(con.execute(sql))
        except duckdb.Error as e:
          got = str(e)
        self.assertEqual(got.strip(), want.strip())


def main():
  global ARGS
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '--extension',
      default=os.path.join(ROOT, 'out', 'duckdb_ext',
                           'perfetto.duckdb_extension'))
  parser.add_argument(
      '--trace',
      default=os.path.join(ROOT, 'test', 'data',
                           'api24_startup_cold.perfetto-trace'))
  parser.add_argument('--print-examples', action='store_true')
  ARGS, rest = parser.parse_known_args()
  ARGS.extension = os.path.abspath(ARGS.extension)
  ARGS.trace = os.path.abspath(ARGS.trace)
  if ARGS.print_examples:
    for sql, _ in readme_examples():
      try:
        print(format_result(connect().execute(sql)))
      except duckdb.Error as e:
        print(e)
      print()
    return 0
  unittest.main(argv=[sys.argv[0]] + rest)
  return 0


if __name__ == '__main__':
  sys.exit(main())

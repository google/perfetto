#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
# SPDX-License-Identifier: Apache-2.0
"""Cross-check generated recursive SQL against a separate tree-walk oracle."""

import sqlite3
from contextlib import closing
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from workloads import (COLUMNS, SHAPES, WORKLOADS, DatasetSpec, Workload,
                       canonical_rows)


class WorkloadTest(unittest.TestCase):

  def test_scan_sources_are_physically_narrow(self):
    for name, columns in (('scan_int', ('value',)),
                          ('scan_numeric', COLUMNS[:4]), ('scan', COLUMNS)):
      workload = Workload(DatasetSpec(41), name)
      with self.subTest(name=name):
        with closing(sqlite3.connect(':memory:')) as connection:
          connection.executescript(workload.setup_sql('sqlite'))
          self.assertEqual(
              tuple(row[1]
                    for row in connection.execute('PRAGMA table_info(tree)')),
              columns)
        for backend in ('pipeline', 'dataframe'):
          # Exercise the actual staging SQL in SQLite up to materialization.
          setup = workload.setup_sql(backend)
          staging, materialization = setup.split('CREATE PERFETTO TABLE', 1)
          with closing(sqlite3.connect(':memory:')) as connection:
            connection.executescript(staging)
            names = ', '.join(columns)
            self.assertEqual(
                connection.execute(f'SELECT {names} FROM staging '
                                   'ORDER BY insertion_ordinal').fetchall(),
                workload.expected_rows())
          self.assertIn(
              'tree AS SELECT ' + ', '.join(columns) +
              ' FROM staging ORDER BY insertion_ordinal;', materialization)
        self.assertEqual(workload.query_sql('pipeline'), 'FROM tree;')

  def test_native_dataframe_layout(self):
    executable = os.environ.get('PIPELINE_PERFETTO_RUNNER')
    if not executable:
      self.skipTest('PIPELINE_PERFETTO_RUNNER is not set')
    runner = Path(executable)
    if not runner.is_file():
      self.skipTest('build pipeline_benchmark_runner to check native layout')
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      for shuffled in (False, True):
        for dense_ids in (False, True):
          for backend in ('pipeline', 'dataframe'):
            with self.subTest(
                shuffled=shuffled, dense_ids=dense_ids, backend=backend):
              spec = DatasetSpec(57, shuffled=shuffled, dense_ids=dense_ids)
              workload = Workload(spec, 'scan')
              (root / 'setup.sql').write_text(workload.setup_sql(backend))
              (root / 'query.sql').write_text(workload.query_sql(backend))
              subprocess.run([
                  str(runner), '--backend', backend, '--setup',
                  str(root / 'setup.sql'), '--query',
                  str(root / 'query.sql'), '--output',
                  str(root / 'result'), '--repeat', '0'
              ],
                             check=True,
                             capture_output=True,
                             text=True)
              lines = (root / 'result.rows.tsv').read_text().splitlines()
              self.assertEqual([int(line.split('\t')[0][2:]) for line in lines],
                               [row[0] for row in spec.data()])
              self.assertTrue(
                  all(len(line.split('\t')) == len(COLUMNS) for line in lines))

  def check_workload(self, workload):
    with closing(sqlite3.connect(':memory:')) as connection:
      connection.executescript(workload.setup_sql('sqlite'))
      cursor = connection.execute(workload.query_sql('sqlite'))
      self.assertEqual(
          tuple(col[0] for col in cursor.description), workload.columns)
      self.assertEqual(
          canonical_rows(cursor), canonical_rows(workload.expected_rows()))

  def test_shapes_stages_and_nulls(self):
    for shape in SHAPES:
      for size in (0, 1, 2, 41):
        for name in WORKLOADS:
          for all_null in (False, True):
            with self.subTest(
                shape=shape, size=size, name=name, all_null=all_null):
              self.check_workload(
                  Workload(DatasetSpec(size, shape, all_null=all_null), name))

  def test_batch_boundaries(self):
    for size in (2047, 2048, 2049, 4097):
      for name in WORKLOADS:
        with self.subTest(size=size, name=name):
          self.check_workload(Workload(DatasetSpec(size), name))

  def test_insertion_order_does_not_change_results(self):
    for shape in SHAPES:
      for name in WORKLOADS:
        ordered = Workload(DatasetSpec(57, shape, shuffled=False), name)
        shuffled = Workload(DatasetSpec(57, shape), name)
        with self.subTest(shape=shape, name=name):
          self.assertEqual(
              canonical_rows(ordered.expected_rows()),
              canonical_rows(shuffled.expected_rows()))
          self.check_workload(ordered)

  def test_oracle_hand_calculated_chain(self):
    # Substitute hand-computed values without relying on SQL or generator RNG.
    class FixedSpec:

      def data(self):
        return [(9, 3, -2, None, 0.5, 'leaf'), (3, 7, None, 4, None, 'middle'),
                (7, None, 5, 1, -0.5, 'root')]

    expected = {
        'down': {
            7: (5,),
            3: (5,),
            9: (3,)
        },
        'up': {
            7: (3,),
            3: (-2,),
            9: (-2,)
        },
        'up_multi': {
            7: (3, 5),
            3: (-2, 4),
            9: (-2, 0)
        },
        'down_up': {
            7: (5, 13),
            3: (5, 8),
            9: (3, 3)
        },
        'up_down': {
            7: (3, 3),
            3: (-2, 1),
            9: (-2, -1)
        },
        'down_down_up': {
            7: (5, 5, 28),
            3: (5, 10, 23),
            9: (3, 13, 13)
        },
    }
    for name, totals in expected.items():
      with self.subTest(name=name):
        rows = Workload(FixedSpec(), name).expected_rows()
        self.assertEqual({row[0]: row[len(COLUMNS):] for row in rows}, totals)

  def test_recursive_cost(self):
    self.assertEqual(
        Workload(DatasetSpec(100, 'chain'), 'up').estimated_recursive_rows,
        5050)
    self.assertEqual(
        Workload(DatasetSpec(100, 'chain'), 'down').estimated_recursive_rows,
        100)
    self.assertEqual(
        Workload(DatasetSpec(100, 'star'), 'up').estimated_recursive_rows, 199)

  def test_rejects_invalid_specs(self):
    with self.assertRaises(ValueError):
      DatasetSpec(-1)
    with self.assertRaises(ValueError):
      DatasetSpec(1, 'cycle')
    with self.assertRaises(ValueError):
      Workload(DatasetSpec(1), 'unknown')


if __name__ == '__main__':
  unittest.main()

#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
# SPDX-License-Identifier: Apache-2.0
"""Native runner correctness tests (never compare execution speed).

Set PIPELINE_PERFETTO_RUNNER and/or PIPELINE_DUCKDB_RUNNER to built executables.
An unset variable skips that engine, allowing the pure Python suite to run
without optional native dependencies.
"""

import csv
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


def decode_rows(text):
  result = []
  for line in text.splitlines():
    row = []
    for cell in line.split('\t'):
      if cell == 'N':
        row.append(None)
      elif cell.startswith('I:'):
        row.append(int(cell[2:]))
      elif cell.startswith('F:'):
        row.append(float.fromhex(cell[2:]))
      elif cell.startswith('S:'):
        row.append(bytes.fromhex(cell[2:]).decode('utf-8'))
      else:
        raise AssertionError('Invalid cell: ' + cell)
    result.append(tuple(row))
  return result


def checksum(rows):
  """Independent implementation of protocol.h's unordered row digest."""
  mask = (1 << 64) - 1
  result = 0
  for row in rows:
    payload = bytearray()
    for cell in row:
      if cell is None:
        payload += b'N'
      elif isinstance(cell, int):
        payload += b'I' + struct.pack('<q', cell)
      elif isinstance(cell, float):
        payload += b'F' + struct.pack('<d', cell)
      elif isinstance(cell, str):
        encoded = cell.encode('utf-8')
        payload += b'S' + struct.pack('<Q', len(encoded)) + encoded
      else:
        raise AssertionError(type(cell))
    payload += b'\n'
    value = 14695981039346656037
    for byte in payload:
      value = ((value ^ byte) * 1099511628211) & mask
    result = (result + value) & mask
  return result


class RunnerTests:
  engine = None

  def setUp(self):
    variable = 'PIPELINE_' + self.engine.upper() + '_RUNNER'
    executable = os.environ.get(variable)
    if not executable:
      self.skipTest(variable + ' is not set')
    self.executable = str(Path(executable).resolve())
    self.temp = tempfile.TemporaryDirectory(prefix='pipeline-runner-test-')
    self.addCleanup(self.temp.cleanup)
    self.directory = Path(self.temp.name)

  def invoke(self,
             query,
             setup='SELECT 1;',
             mode='end_to_end',
             repeat=2,
             warmup=1,
             extra=(),
             backend='sqlite',
             success=True):
    (self.directory / 'query.sql').write_text(query, encoding='utf-8')
    (self.directory / 'setup.sql').write_text(setup, encoding='utf-8')
    prefix = self.directory / 'result'
    args = [
        self.executable, '--query',
        str(self.directory / 'query.sql'), '--setup',
        str(self.directory / 'setup.sql'), '--output',
        str(prefix), '--mode', mode, '--repeat',
        str(repeat), '--warmup',
        str(warmup)
    ]
    if self.engine == 'perfetto':
      args += ['--backend', backend]
    args += list(extra)
    process = subprocess.run(args, capture_output=True, text=True, timeout=10)
    if not success:
      self.assertGreater(process.returncode, 0, process.stderr)
      self.assertTrue(process.stderr)
      return None
    self.assertEqual(process.returncode, 0, process.stderr)
    rows = decode_rows(Path(str(prefix) + '.rows.tsv').read_text())
    metadata = json.loads(Path(str(prefix) + '.metadata.json').read_text())
    with Path(str(prefix) + '.timings.csv').open() as stream:
      timings = list(csv.DictReader(stream))
    self.assertEqual(len(timings), repeat)
    self.assertEqual(metadata['validation_rows'], len(rows))
    self.assertIsInstance(metadata['validation_columns'], int)
    if rows:
      self.assertEqual(metadata['validation_columns'], len(rows[0]))
    self.assertEqual(metadata['validation_checksum'], checksum(rows))
    self.assertEqual(metadata['mode'], mode)
    self.assertIsInstance(metadata['version'], str)
    self.assertIsInstance(metadata['threads'], int)
    self.assertGreaterEqual(metadata['setup_ns'], 0)
    for i, measurement in enumerate(timings):
      self.assertEqual(int(measurement['repetition']), i)
      self.assertEqual(int(measurement['rows']), len(rows))
      self.assertEqual(int(measurement['checksum']), checksum(rows))
      self.assertGreaterEqual(int(measurement['elapsed_ns']), 0)
    return rows, metadata

  def test_empty_result_both_modes(self):
    for mode in ('end_to_end', 'prepared'):
      with self.subTest(mode=mode):
        rows, metadata = self.invoke('SELECT 1, NULL WHERE FALSE', mode=mode)
        self.assertEqual(rows, [])
        self.assertEqual(metadata['validation_columns'], 2)

  def test_nulls_both_modes(self):
    for mode in ('end_to_end', 'prepared'):
      with self.subTest(mode=mode):
        rows, _ = self.invoke(
            'SELECT NULL, NULL UNION ALL SELECT NULL, NULL', mode=mode)
        self.assertEqual(rows, [(None, None), (None, None)])

  def test_numeric_boundaries_and_strings(self):
    nul = 'chr(0)' if self.engine == 'duckdb' else 'char(0)'
    query = ("SELECT -9223372036854775808, 9223372036854775807, "
             "CAST(0.125 AS DOUBLE), CAST(-0.5 AS DOUBLE), "
             "'λ雪\t\n' || " + nul + " || 'fin', '', NULL")
    expected = [(-(1 << 63), (1 << 63) - 1, 0.125, -0.5, 'λ雪\t\n\0fin', '',
                 None)]
    for mode in ('end_to_end', 'prepared'):
      with self.subTest(mode=mode):
        rows, _ = self.invoke(query, mode=mode)
        self.assertEqual(rows, expected)

  def test_reset_multiple_batches(self):
    query = ('WITH RECURSIVE t(i) AS (SELECT 0 UNION ALL '
             'SELECT i+1 FROM t WHERE i < 4096) '
             'SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE -i END FROM t')
    rows, _ = self.invoke(query, mode='prepared', repeat=3)
    self.assertEqual(rows,
                     [(i, None if i % 3 == 0 else -i) for i in range(4097)])

  def test_validation_only(self):
    rows, metadata = self.invoke(
        'SELECT 7', mode='prepared', repeat=0, warmup=17)
    self.assertEqual(rows, [(7,)])
    prepare_key = ('prepare_ns'
                   if self.engine == 'duckdb' else 'prepare_and_first_step_ns')
    self.assertEqual(metadata[prepare_key], 0)

  def test_malformed_options(self):
    for extra in (('--repeat', '-1'), ('--warmup', 'x'), ('--threads', '0'),
                  ('--mode', 'bad'), ('--unknown', 'value'), ('--repeat',)):
      with self.subTest(extra=extra):
        self.invoke('SELECT 1', extra=extra, success=False)

  def test_malformed_sql(self):
    self.invoke('not valid SQL', success=False)

  def test_empty_sql(self):
    self.invoke('', success=False)

  def test_unsupported_blob(self):
    self.invoke("SELECT CAST('abc' AS BLOB)", success=False)


class DuckdbRunnerTests(RunnerTests, unittest.TestCase):
  engine = 'duckdb'

  def test_reject_integer_overflow(self):
    for query in ('SELECT 9223372036854775808::HUGEINT',
                  'SELECT (-9223372036854775808)::HUGEINT - 1',
                  'SELECT 18446744073709551615::UBIGINT'):
      with self.subTest(query=query):
        self.invoke(query, success=False)


class PerfettoRunnerTests(RunnerTests, unittest.TestCase):
  engine = 'perfetto'

  def test_pipeline_reset_and_table_cleanup(self):
    setup = ('CREATE PERFETTO TABLE nodes AS '
             'SELECT 7 AS id, NULL AS parent_id, 3 AS value UNION ALL '
             'SELECT 9, 7, 5 UNION ALL SELECT 4, 9, -1;')
    query = ('FROM nodes |> TREE ACCUMULATE UP SUM(value) AS total')
    for mode in ('end_to_end', 'prepared'):
      with self.subTest(mode=mode):
        rows, _ = self.invoke(
            query, setup=setup, backend='pipeline', mode=mode, repeat=4)
        self.assertEqual(
            sorted(rows), [(4, 9, -1, -1), (7, None, 3, 7), (9, 7, 5, 4)])


if __name__ == '__main__':
  unittest.main()

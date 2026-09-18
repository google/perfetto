#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
# SPDX-License-Identifier: Apache-2.0
import argparse
import collections
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import run
from workloads import DatasetSpec, Workload, WORKLOADS


class HarnessTest(unittest.TestCase):

  def test_resolve_runtime_duckdb_with_loader_environment(self):
    completed = mock.Mock(
        stdout='libduckdb.so => /tmp/duck db/libduckdb.so (0x1234)\n')
    with mock.patch.object(
        run.subprocess, 'run', return_value=completed) as invoke:
      with mock.patch.dict(run.os.environ,
                           {'LD_LIBRARY_PATH': '/tmp/override'}):
        self.assertEqual(
            run.loaded_duckdb_library(Path('/tmp/runner')),
            Path('/tmp/duck db/libduckdb.so'))
        self.assertEqual(invoke.call_args.kwargs['env']['LD_LIBRARY_PATH'],
                         '/tmp/override')
    with mock.patch.object(
        run.subprocess,
        'run',
        return_value=mock.Mock(stdout='/tmp/duck db/libduckdb.so (0x1234)\n')):
      self.assertEqual(
          run.loaded_duckdb_library(Path('/tmp/runner')),
          Path('/tmp/duck db/libduckdb.so'))
    with mock.patch.object(
        run.subprocess,
        'run',
        return_value=mock.Mock(stdout='libduckdb.so => not found\n')):
      with self.assertRaisesRegex(ValueError, 'Cannot find a resolved'):
        run.loaded_duckdb_library(Path('/tmp/runner'))

  def test_manifest_checks_runner_and_actual_loaded_library(self):
    with tempfile.TemporaryDirectory() as directory:
      runner = Path(directory) / 'runner'
      runner.write_bytes(b'runner')
      library = Path(directory) / 'libduckdb.so'
      library.write_bytes(b'library')
      sidecar = Path(str(runner) + '.build.json')
      sidecar.write_text(
          json.dumps({
              'runner_sha256': run.sha256(runner),
              'library_path': str(library),
              'library_sha256': run.sha256(library)
          }))
      with mock.patch.object(run.sys, 'platform', 'linux'):
        with mock.patch.object(
            run, 'loaded_duckdb_library', return_value=library):
          self.assertEqual(
              run.read_build_metadata(runner)['runtime_library_path'],
              str(library))
          library.write_bytes(b'changed library')
          with self.assertRaisesRegex(ValueError, 'runtime library differs'):
            run.read_build_metadata(runner)
        other = Path(directory) / 'override.so'
        other.write_bytes(b'library')
        with mock.patch.object(
            run, 'loaded_duckdb_library', return_value=other):
          with self.assertRaisesRegex(ValueError, 'runtime library differs'):
            run.read_build_metadata(runner)
      runner.write_bytes(b'changed runner')
      with self.assertRaisesRegex(ValueError, 'Runner differs'):
        run.read_build_metadata(runner)

  def test_missing_build_sidecar_is_optional(self):
    with tempfile.TemporaryDirectory() as directory:
      self.assertIsNone(run.read_build_metadata(Path(directory) / 'runner'))

  def test_exact_typed_validation_and_duplicate_rows(self):
    expected = collections.Counter(
        run.normalize_row(r)
        for r in [(1, None, 0.5, 'λ\x00'), (1, None, 0.5, 'λ\x00')])
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / 'rows.tsv'
      row = 'I:1\tN\tF:0x1p-1\tS:cebb00\n'
      path.write_text(row * 2)
      run.validate_rows(path, expected)
      path.write_text(row)
      with self.assertRaises(ValueError):
        run.validate_rows(path, expected)
      path.write_text(row.replace('I:1', 'F:0x1p+0') * 2)
      with self.assertRaises(ValueError):
        run.validate_rows(path, expected)

  def test_reject_duplicate_options(self):
    with self.assertRaises(argparse.ArgumentTypeError):
      run.csv_list('scan,scan')
    with self.assertRaises(argparse.ArgumentTypeError):
      run.csv_list('1,1', integer=True)
    with self.assertRaises(argparse.ArgumentTypeError):
      run.csv_list('-1', integer=True)

  def test_profiles_apply_filters_and_overrides(self):
    args = argparse.Namespace(
        rows=None,
        profile='quick',
        seed=1729,
        shapes=['balanced'],
        workloads=['scan'],
        ordered=True,
        dense_ids=True,
        all_null=True)
    cases = run.cases(args)
    self.assertEqual([w.spec.rows for w in cases], [1000, 10000])
    self.assertTrue(
        all(w.name == 'scan' and w.spec.dense_ids and w.spec.all_null and
            not w.spec.shuffled for w in cases))

  def test_explicit_rows_use_default_shapes_and_workloads(self):
    args = argparse.Namespace(
        rows=[0],
        seed=1,
        shapes=None,
        workloads=None,
        ordered=False,
        dense_ids=False,
        all_null=False)
    self.assertEqual(len(run.cases(args)), len(WORKLOADS))

  def test_setup_cache_distinguishes_physical_schemas(self):
    spec = DatasetSpec(10)
    self.assertEqual(
        run.dataset_key(Workload(spec, 'scan')),
        run.dataset_key(Workload(spec, 'up')))
    keys = {
        run.dataset_key(Workload(spec, name))
        for name in ('scan', 'scan_int', 'scan_numeric')
    }
    self.assertEqual(len(keys), 3)

  def test_summary_uses_median_and_rejects_invalid_samples(self):
    stats = run.summarize([{
        'elapsed_ns': str(n)
    } for n in (1_000_000, 100_000_000, 2_000_000)])
    self.assertEqual(stats['median_ms'], 2)
    with self.assertRaises(ValueError):
      run.summarize([])
    with self.assertRaises(ValueError):
      run.summarize([{'elapsed_ns': '0'}])


if __name__ == '__main__':
  unittest.main()

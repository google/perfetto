#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
# SPDX-License-Identifier: Apache-2.0
"""Validate and benchmark equivalent queries with native, persistent runners."""

import argparse
import collections
import csv
from dataclasses import asdict
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import statistics
import struct
import subprocess
import sys
import time

from workloads import DatasetSpec, Workload, SHAPES, WORKLOADS

BACKENDS = ('pipeline', 'sqlite', 'dataframe', 'duckdb', 'pipeline_sql_scan')
ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
  digest = hashlib.sha256()
  with open(path, 'rb') as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
      digest.update(chunk)
  return digest.hexdigest()


def loaded_duckdb_library(runner):
  """Resolve the Linux loader's choice with the benchmark's inherited environment."""
  try:
    result = subprocess.run(['ldd', str(runner)],
                            capture_output=True,
                            text=True,
                            check=True,
                            timeout=10,
                            env={
                                **os.environ, 'LC_ALL': 'C'
                            })
  except (OSError, subprocess.SubprocessError) as error:
    raise ValueError(
        f'Cannot resolve DuckDB runtime library for {runner}: {error}'
    ) from error
  for line in result.stdout.splitlines():
    match = re.match(
        r'\s*libduckdb\.so(?:\.\S+)?\s+=>\s+(.+?)\s+\(0x[0-9a-fA-F]+\)', line)
    if not match:
      # Explicitly preloaded absolute paths have no "name =>" prefix.
      match = re.match(r'\s*(/.*/libduckdb\.so(?:\.\S+)?)\s+\(0x[0-9a-fA-F]+\)',
                       line)
    if match:
      return Path(match.group(1)).resolve()
  raise ValueError(
      f'Cannot find a resolved libduckdb.so in ldd output for {runner}; '
      'check its shared-library search path and LD_PRELOAD')


def read_build_metadata(runner):
  build_file = Path(str(runner) + '.build.json')
  if not build_file.exists():
    return None
  metadata = json.loads(build_file.read_text())
  if metadata.get('runner_sha256') != sha256(runner):
    raise ValueError(
        f'Runner differs from its build manifest: {runner}; rebuild it')
  if 'library_path' in metadata:
    expected = Path(metadata['library_path']).resolve()
    actual = loaded_duckdb_library(runner) if sys.platform.startswith(
        'linux') else expected
    metadata['runtime_library_path'] = str(actual)
    metadata['runtime_library_sha256'] = sha256(actual)
    if actual != expected or metadata.get(
        'library_sha256') != metadata['runtime_library_sha256']:
      raise ValueError(
          f'DuckDB runtime library differs from its build manifest: '
          f'expected {expected}, loaded {actual}; rebuild or correct '
          'the shared-library search path')
  return metadata


def normalize_row(row):
  """Retain SQL type and exact IEEE value; never round floats for validation."""
  cells = []
  for value in row:
    if value is None:
      cells.append(('N', None))
    elif isinstance(value, int):
      cells.append(('I', value))
    elif isinstance(value, float):
      cells.append(('F', struct.pack('>d', value).hex()))
    else:
      cells.append(('S', value.encode('utf-8').hex()))
  return tuple(cells)


def read_rows(path):
  result = []
  with open(path, encoding='ascii') as stream:
    for number, line in enumerate(stream, 1):
      cells = []
      for cell in line.rstrip('\n').split('\t'):
        if cell == 'N':
          cells.append(('N', None))
        elif cell.startswith('I:'):
          cells.append(('I', int(cell[2:])))
        elif cell.startswith('F:'):
          value = float.fromhex(cell[2:])
          if not math.isfinite(value):
            raise ValueError(f'non-finite result at row {number}')
          cells.append(('F', struct.pack('>d', value).hex()))
        elif cell.startswith('S:'):
          cells.append(('S', bytes.fromhex(cell[2:]).hex()))
        else:
          raise ValueError(f'invalid cell at row {number}: {cell!r}')
      result.append(tuple(cells))
  return collections.Counter(result)


def validate_rows(path, expected):
  actual = read_rows(path)
  if actual != expected:
    missing = list((expected - actual).items())[:3]
    extra = list((actual - expected).items())[:3]
    raise ValueError(
        f'exact typed results differ; missing={missing}, extra={extra}')


def csv_list(value, choices=None, integer=False):
  values = value.split(',')
  if integer:
    values = [int(item) for item in values]
    if any(item < 0 for item in values):
      raise argparse.ArgumentTypeError('row counts must be non-negative')
  if len(set(values)) != len(values):
    raise argparse.ArgumentTypeError('duplicate values are not allowed')
  if choices and any(item not in choices for item in values):
    raise argparse.ArgumentTypeError('choose from ' + ','.join(choices))
  return values


def cases(args):
  if args.rows is not None:
    return [
        Workload(
            DatasetSpec(n, shape, args.seed, not args.ordered, args.all_null,
                        args.dense_ids), name)
        for n in args.rows
        for shape in (args.shapes or ['balanced'])
        for name in (args.workloads or WORKLOADS)
    ]
  specs = []
  if args.profile == 'smoke':
    specs += [(n, 'balanced', False, False, name)
              for n in (0, 1, 33)
              for name in WORKLOADS]
    specs += [(n, 'forest', False, False, name)
              for n in (2047, 2048, 2049)
              for name in ('scan', 'down', 'up')]
    specs += [(33, shape, False, True, name)
              for shape in ('chain', 'star')
              for name in ('up', 'down_up')]
    specs += [(2049, 'balanced', True, False, name) for name in ('up', 'down')]
  elif args.profile == 'quick':
    specs += [(n, 'balanced', False, False, name)
              for n in (1000, 10000)
              for name in WORKLOADS]
    specs += [(10000, shape, False, False, name)
              for shape in ('star', 'forest')
              for name in ('up', 'down')]
    specs += [(256, 'chain', False, False, name) for name in ('up', 'down')]
    specs += [(10000, 'balanced', True, False, name) for name in ('up', 'down')]
  else:
    specs += [(n, shape, dense, False, name)
              for n in (1000, 10000, 100000)
              for shape in ('balanced', 'star', 'forest')
              for dense in (False, True)
              for name in WORKLOADS]
    specs += [(n, 'chain', False, False, name)
              for n in (64, 256, 1024)
              for name in WORKLOADS]
  selected = [
      Workload(
          DatasetSpec(n, shape, args.seed, not (dense or args.ordered), null or
                      args.all_null, dense or args.dense_ids), name)
      for n, shape, dense, null, name in specs
      if (args.shapes is None or shape in args.shapes) and
      (args.workloads is None or name in args.workloads)
  ]
  return list(dict.fromkeys(selected))


def case_id(workload):
  spec = workload.spec
  return (f'{spec.shape}-{spec.rows}-{workload.name}-'
          f'{"dense" if spec.dense_ids else "sparse"}-'
          f'{"shuffled" if spec.shuffled else "ordered"}'
          f'{"-null" if spec.all_null else ""}')


def dataset_key(workload):
  return hashlib.sha256(
      json.dumps(
          {
              'spec': asdict(workload.spec),
              'columns': workload.source_columns
          },
          sort_keys=True).encode()).hexdigest()[:16]


def execute(command, prefix, timeout, affinity):
  prefix.parent.mkdir(parents=True, exist_ok=True)
  start = time.monotonic()
  with open(str(prefix) + '.stdout.log',
            'w') as out, open(str(prefix) + '.stderr.log', 'w') as err:
    try:
      # The parent pins itself before launching subprocesses, so child threads
      # inherit the same CPU affinity without a preexec_fn in a threaded process.
      result = subprocess.run(command, stdout=out, stderr=err, timeout=timeout)
      status = 'ok' if result.returncode == 0 else 'error'
      code = result.returncode
    except subprocess.TimeoutExpired:
      status, code = 'timeout', None
  return {
      'status': status,
      'returncode': code,
      'process_seconds': time.monotonic() - start,
      'command': command,
      'affinity': affinity
  }


def summarize(samples):
  values = [int(row['elapsed_ns']) / 1e6 for row in samples]
  if not values or any(value <= 0 for value in values):
    raise ValueError('missing or nonpositive timing samples')
  ordered = sorted(values)
  return {
      'median_ms': statistics.median(values),
      'min_ms': min(values),
      'max_ms': max(values),
      'mean_ms': statistics.mean(values),
      'p95_ms': ordered[max(0,
                            math.ceil(.95 * len(ordered)) - 1)],
      'stdev_ms': statistics.stdev(values) if len(values) > 1 else 0,
      'samples': len(values)
  }


def write_report(output, manifest, records):
  (output / 'results.json').write_text(
      json.dumps({
          'manifest': manifest,
          'results': records
      }, indent=2) + '\n')
  fields = [
      'case', 'backend', 'mode', 'status', 'median_ms', 'min_ms', 'max_ms',
      'p95_ms', 'stdev_ms', 'samples', 'process_seconds', 'reason'
  ]
  with open(output / 'summary.csv', 'w', newline='') as stream:
    writer = csv.DictWriter(stream, fields, extrasaction='ignore')
    writer.writeheader()
    writer.writerows(records)
  lines = [
      '# Pipeline query benchmark', '',
      'Median milliseconds; native full-result consumption with the same '
      'typed checksum sink. Setup and validation are outside timings. '
      'DuckDB uses one thread unless explicitly requested.', '',
      'End-to-end includes query preparation, execution, consumption and '
      'result destruction. Prepared mode reuses the compiled statement and '
      'includes reset/re-execution. SQLite may stream; DuckDB may materialize '
      'before consumption. These are client-observed latencies, not isolated '
      'operator CPU timings.', '',
      'Every timed case first passed exact typed, order-independent oracle '
      'validation for all non-budget-skipped selected backends. Failures and '
      'timeouts are retained below; they are never counted as speedups.', '',
      f'Commit: `{manifest["git_revision"]}`. Raw commands, input SQL, hashes, '
      'versions and every sample are in `results.json` and case directories.',
      ''
  ]
  for mode in manifest['modes']:
    lines += [
        f'## {mode}', '', '| Case | ' + ' | '.join(manifest['backends']) + ' |',
        '|---|' + '---:|' * len(manifest['backends'])
    ]
    ids = list(
        dict.fromkeys(
            record['case'] for record in records if record['mode'] == mode))
    for cid in ids:
      values = []
      for backend in manifest['backends']:
        match = next((r for r in records if r['case'] == cid and
                      r['mode'] == mode and r['backend'] == backend), None)
        values.append('—' if not match else f'{match["median_ms"]:.3f}'
                      if match['status'] == 'ok' else match['status'])
      lines.append('| ' + cid + ' | ' + ' | '.join(values) + ' |')
    lines.append('')
  failed = [r for r in records if r['status'] != 'ok']
  if failed:
    lines += ['## Incomplete comparisons', '']
    for record in failed:
      lines.append(
          f'- {record["case"]} / {record["backend"]} / '
          f'{record["mode"]}: {record["status"]}: {record.get("reason", "see logs")}'
      )
  (output / 'summary.md').write_text('\n'.join(lines) + '\n')


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      '--perfetto-runner',
      type=Path,
      default=ROOT / 'out/linux_clang_release/pipeline_benchmark_runner')
  parser.add_argument(
      '--duckdb-runner',
      type=Path,
      default=ROOT / 'out/pipeline-benchmark/duckdb_runner')
  parser.add_argument('--output', type=Path, required=True)
  parser.add_argument(
      '--profile', choices=('smoke', 'quick', 'full'), default='quick')
  parser.add_argument('--rows', type=lambda s: csv_list(s, integer=True))
  parser.add_argument(
      '--shapes', type=lambda s: csv_list(s, SHAPES), default=None)
  parser.add_argument(
      '--workloads', type=lambda s: csv_list(s, WORKLOADS), default=None)
  parser.add_argument(
      '--backends',
      type=lambda s: csv_list(s, BACKENDS),
      default=list(BACKENDS[:4]))
  parser.add_argument(
      '--modes',
      type=lambda s: csv_list(s, ('end_to_end', 'prepared')),
      default=['end_to_end', 'prepared'])
  parser.add_argument('--repeat', type=int, default=7)
  parser.add_argument('--warmup', type=int, default=2)
  parser.add_argument(
      '--timeout',
      type=float,
      default=60,
      help='seconds per complete runner process, including setup, validation, warmups and every sample'
  )
  parser.add_argument('--max-recursive-rows', type=int, default=2_000_000)
  parser.add_argument('--seed', type=int, default=1729)
  parser.add_argument(
      '--threads',
      type=int,
      default=1,
      help='DuckDB only; keep 1 for comparable headline numbers')
  parser.add_argument(
      '--cpu', type=int, help='pin all runners to one allowed CPU (Linux)')
  parser.add_argument('--ordered', action='store_true')
  parser.add_argument('--dense-ids', action='store_true')
  parser.add_argument('--all-null', action='store_true')
  args = parser.parse_args()
  if (args.repeat <= 0 or args.warmup < 0 or not math.isfinite(args.timeout) or
      args.timeout <= 0 or args.threads <= 0 or args.max_recursive_rows < 0):
    parser.error(
        'repeat, timeout and threads must be positive; warmup and recursive budget nonnegative'
    )
  if args.output.exists() and any(args.output.iterdir()):
    parser.error(
        'output must be a new or empty directory; never overwrite measurements')
  args.output.mkdir(parents=True, exist_ok=True)
  runners = {
      b: (args.duckdb_runner if b == 'duckdb' else
          args.perfetto_runner).resolve() for b in args.backends
  }
  for path in set(runners.values()):
    if not path.is_file() or not os.access(path, os.X_OK):
      parser.error(f'missing executable: {path}')
  if args.cpu is not None:
    if not hasattr(
        os, 'sched_getaffinity') or args.cpu not in os.sched_getaffinity(0):
      parser.error(
          '--cpu must be in this process\'s allowed Linux affinity set')
    os.sched_setaffinity(0, {args.cpu})
  affinity = sorted(os.sched_getaffinity(0)) if hasattr(
      os, 'sched_getaffinity') else None
  selected = cases(args)
  if not selected:
    parser.error('filters selected no cases')
  cpu_model = platform.processor()
  if Path('/proc/cpuinfo').exists():
    cpu_model = next(
        (line.split(':', 1)[1].strip()
         for line in Path('/proc/cpuinfo').read_text().splitlines()
         if line.startswith('model name')), cpu_model)
  manifest = {
      'git_revision':
          subprocess.check_output(['git', 'rev-parse', 'HEAD'],
                                  cwd=ROOT,
                                  text=True).strip(),
      'git_status':
          subprocess.check_output(['git', 'status', '--short'],
                                  cwd=ROOT,
                                  text=True),
      'platform':
          platform.platform(),
      'processor':
          cpu_model,
      'timestamp_utc':
          time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
      'affinity':
          affinity,
      'arguments': {
          k: str(v) if isinstance(v, Path) else v
          for k, v in vars(args).items()
      },
      'runner_sha256': {
          str(p): sha256(p) for p in set(runners.values())
      },
      'source_sha256': {
          str(p.relative_to(ROOT)): sha256(p)
          for p in list(Path(__file__).parent.iterdir()) +
          list((ROOT / 'src/trace_processor/perfetto_sql/benchmarks').iterdir())
          if p.suffix in ('.py', '.cc', '.h', '.gn')
      },
      'backends':
          args.backends,
      'modes':
          args.modes,
      'loader_environment': {
          name: os.environ.get(name)
          for name in ('LD_LIBRARY_PATH', 'LD_PRELOAD')
      }
  }
  manifest['build_metadata'] = {}
  for runner in set(runners.values()):
    try:
      build_metadata = read_build_metadata(runner)
    except (OSError, ValueError) as error:
      parser.error(str(error))
    if build_metadata is not None:
      manifest['build_metadata'][str(runner)] = build_metadata
  args_gn = args.perfetto_runner.parent / 'args.gn'
  if args_gn.exists():
    manifest['perfetto_args_gn'] = args_gn.read_text()
  records = []
  rng = random.Random(args.seed)
  for number, workload in enumerate(selected, 1):
    cid = case_id(workload)
    print(f'[{number}/{len(selected)}] {cid}', flush=True)
    folder = args.output / cid
    folder.mkdir()
    expected = collections.Counter(
        normalize_row(row) for row in workload.expected_rows())
    inputs, validations = {}, {}
    dataset_folder = args.output / 'datasets' / dataset_key(workload)
    dataset_folder.mkdir(parents=True, exist_ok=True)
    backends = list(args.backends)
    rng.shuffle(backends)
    for backend in backends:
      setup, query = dataset_folder / (backend + '.setup.sql'), folder / (
          backend + '.query.sql')
      if not setup.exists():
        setup.write_text(workload.setup_sql(backend))
      query.write_text(workload.query_sql(backend))
      inputs[backend] = (setup, query)
      if backend not in (
          'pipeline', 'pipeline_sql_scan'
      ) and workload.estimated_recursive_rows > args.max_recursive_rows:
        validations[backend] = {
            'status':
                'budget_skipped',
            'reason':
                f'estimated recursive rows {workload.estimated_recursive_rows} exceed {args.max_recursive_rows}'
        }
        continue
      prefix = folder / (backend + '.validate')
      command = [
          str(runners[backend]), '--setup',
          str(setup), '--query',
          str(query), '--output',
          str(prefix), '--repeat', '0', '--warmup', '0', '--threads',
          str(args.threads if backend == 'duckdb' else 1)
      ]
      if backend != 'duckdb':
        command += ['--backend', backend]
      validation = execute(command, prefix, args.timeout, affinity)
      if validation['status'] == 'ok':
        try:
          validate_rows(str(prefix) + '.rows.tsv', expected)
          metadata = json.loads(
              Path(str(prefix) + '.metadata.json').read_text())
          if metadata['validation_columns'] != len(workload.columns):
            raise ValueError(
                'validation column count differs, including on empty input')
          validation['runner_metadata'] = metadata
        except (OSError, ValueError, KeyError) as error:
          validation.update(status='validation_failed', reason=str(error))
      validations[backend] = validation
    valid = all(
        v['status'] in ('ok', 'budget_skipped') for v in validations.values())
    jobs = [(backend, mode) for backend in backends for mode in args.modes]
    rng.shuffle(jobs)
    for backend, mode in jobs:
      validation = validations[backend]
      record = {
          'case': cid,
          'dataset': asdict(workload.spec),
          'workload': workload.name,
          'estimated_recursive_rows': workload.estimated_recursive_rows,
          'backend': backend,
          'mode': mode,
          'validation': validation
      }
      if validation['status'] != 'ok' or not valid:
        record.update(
            status=validation['status']
            if validation['status'] != 'ok' else 'validation_blocked',
            reason=validation.get(
                'reason',
                'case did not validate on every selected backend; see validation logs'
            ))
        records.append(record)
        continue
      setup, query = inputs[backend]
      prefix = folder / (backend + '.' + mode)
      command = [
          str(runners[backend]), '--setup',
          str(setup), '--query',
          str(query), '--output',
          str(prefix), '--repeat',
          str(args.repeat), '--warmup',
          str(args.warmup), '--threads',
          str(args.threads if backend == 'duckdb' else 1), '--mode', mode
      ]
      if backend != 'duckdb':
        command += ['--backend', backend]
      record.update(execute(command, prefix, args.timeout, affinity))
      if record['status'] == 'ok':
        try:
          validate_rows(str(prefix) + '.rows.tsv', expected)
          samples = list(csv.DictReader(open(str(prefix) + '.timings.csv')))
          if len(samples) != args.repeat:
            raise ValueError('wrong number of timing samples')
          if any(
              int(sample['rows']) != workload.spec.rows for sample in samples):
            raise ValueError('timed row count differs')
          record.update(summarize(samples))
          record['raw_samples'] = samples
          record['runner_metadata'] = json.loads(
              Path(str(prefix) + '.metadata.json').read_text())
          if record['runner_metadata']['validation_columns'] != len(
              workload.columns):
            raise ValueError(
                'measurement-process validation column count differs')
          record['query_sha256'] = sha256(query)
          record['setup_sha256'] = sha256(setup)
        except (OSError, ValueError, KeyError) as error:
          record.update(status='invalid_output', reason=str(error))
      records.append(record)
      write_report(args.output, manifest, records)
    write_report(args.output, manifest, records)
  failures = sum(r['status'] not in ('ok', 'budget_skipped') for r in records)
  print(
      f'{len(records)} comparisons; {failures} failed. Report: {args.output / "summary.md"}',
      flush=True)
  return 1 if failures else 0


if __name__ == '__main__':
  sys.exit(main())

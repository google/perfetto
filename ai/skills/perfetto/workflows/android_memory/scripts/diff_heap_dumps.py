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
"""Diffs the Java heap graphs of two warm trace_processor sessions."""

import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass, field
import io
import subprocess
import sys

HEAP_SUMMARY_QUERY = """
INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;

SELECT
  s.upid,
  s.graph_sample_ts,
  COALESCE(p.name, 'pid=' || p.pid) AS process_name,
  s.total_obj_count,
  s.total_heap_size + s.total_native_alloc_registry_size AS total_bytes,
  s.reachable_obj_count,
  s.reachable_heap_size + s.reachable_native_alloc_registry_size
    AS reachable_bytes
FROM android_heap_graph_stats AS s
JOIN process AS p ON s.upid = p.id;
"""

# Sizes come from the stdlib aggregation because it reports non-overlapping
# dominated sizes; summing them per instance would double-count self-nesting
# classes. The GROUP BY merges same-named types loaded by different class
# loaders, which the stdlib keeps apart by type_id.
CLASS_AGGREGATION_QUERY = """
INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_class_aggregation;

SELECT
  upid,
  graph_sample_ts,
  type_name AS class_name,
  IIF(is_libcore_or_array, 'libcore_or_array', 'app_class') AS category,
  SUM(reachable_obj_count) AS obj_count,
  SUM(reachable_size_bytes + reachable_native_size_bytes) AS self_bytes,
  SUM(dominated_size_bytes + dominated_native_size_bytes) AS dominated_bytes
FROM android_heap_graph_class_aggregation
GROUP BY upid, graph_sample_ts, class_name, category;
"""

APP_CLASS = 'app_class'
LIBCORE_OR_ARRAY = 'libcore_or_array'
TOP_APP_CLASSES = 10
TOP_LIBCORE_OR_ARRAY_CLASSES = 5


class HeapDiffError(Exception):
  """Raised when diffing heap dumps fails."""


@dataclass(frozen=True, order=True)
class SnapshotId:
  """Identifies one heap graph dump: one process at one sample timestamp."""
  graph_sample_ts: int
  upid: int


@dataclass(frozen=True)
class HeapSummary:
  """Whole-heap counters of one dump."""
  total_obj_count: int
  total_bytes: int
  reachable_obj_count: int
  reachable_bytes: int


@dataclass(frozen=True)
class Metrics:
  """Counters of all reachable instances of one class."""
  obj_count: int = 0
  self_bytes: int = 0
  dominated_bytes: int = 0


@dataclass
class Snapshot:
  process_name: str
  summary: HeapSummary
  classes: dict[str, dict[str, Metrics]] = field(
      default_factory=lambda: defaultdict(dict))

  def classes_of(self, category: str) -> dict[str, Metrics]:
    return self.classes.get(category, {})


@dataclass(frozen=True)
class Growth:
  """Before/after metrics of a single class."""
  name: str
  before: Metrics
  after: Metrics

  @property
  def obj_count_delta(self) -> int:
    return self.after.obj_count - self.before.obj_count

  @property
  def dominated_bytes_delta(self) -> int:
    return self.after.dominated_bytes - self.before.dominated_bytes

  @property
  def self_bytes_delta(self) -> int:
    return self.after.self_bytes - self.before.self_bytes

  def has_grown(self) -> bool:
    return self.obj_count_delta > 0 or self.dominated_bytes_delta > 0


def query(session: str, trace_processor: str, sql: str) -> list[dict[str, str]]:
  """Runs a query against a warm session and returns its CSV rows."""
  command = [trace_processor, 'query', '--remote', session, '-f', '-']
  try:
    result = subprocess.run(
        command, input=sql, capture_output=True, text=True, check=True)
  except FileNotFoundError as error:
    raise HeapDiffError(f'{trace_processor} not found; pass --trace-processor '
                        'with its path') from error
  except subprocess.CalledProcessError as error:
    raise HeapDiffError(f'querying session `{session}` failed: '
                        f'{error.stderr.strip()}') from error
  return list(csv.DictReader(io.StringIO(result.stdout)))


def snapshot_id_of(row: dict[str, str]) -> SnapshotId:
  return SnapshotId(int(row['graph_sample_ts']), int(row['upid']))


def load_snapshots(session: str,
                   trace_processor: str) -> dict[SnapshotId, Snapshot]:
  snapshots = {
      snapshot_id_of(row):
          Snapshot(
              row['process_name'],
              HeapSummary(
                  int(row['total_obj_count']), int(row['total_bytes']),
                  int(row['reachable_obj_count']), int(row['reachable_bytes'])))
      for row in query(session, trace_processor, HEAP_SUMMARY_QUERY)
  }
  if not snapshots:
    raise HeapDiffError(
        f'`{session}` holds no Java heap graph; is it a heap dump?')
  for row in query(session, trace_processor, CLASS_AGGREGATION_QUERY):
    classes = snapshots[snapshot_id_of(row)].classes
    classes[row['category']][row['class_name']] = Metrics(
        int(row['obj_count']), int(row['self_bytes']),
        int(row['dominated_bytes']))
  return snapshots


def describe(snapshot_id: SnapshotId, snapshot: Snapshot) -> str:
  return (f'`upid={snapshot_id.upid}` (`{snapshot.process_name}`), '
          f'`graph_sample_ts={snapshot_id.graph_sample_ts}`')


def select_snapshot(snapshots: dict[SnapshotId, Snapshot], session: str,
                    upid: int | None, ts: int | None) -> SnapshotId:
  """Selects the first snapshot matching the filters; fails if ambiguous."""
  matches = sorted(
      s for s in snapshots if (upid is None or s.upid == upid) and
      (ts is None or s.graph_sample_ts == ts))
  available = ', '.join(describe(s, snapshots[s]) for s in sorted(snapshots))
  if not matches:
    raise HeapDiffError(f'`{session}`: no snapshot with upid={upid}, '
                        f'graph_sample_ts={ts}. Available: {available}')
  if len({s.upid for s in matches}) > 1:
    raise HeapDiffError(f'`{session}`: snapshots of several processes; select '
                        f'one with --before-upid / --after-upid. '
                        f'Available: {available}')
  return matches[0]


def class_growths(before: Snapshot, after: Snapshot,
                  category: str) -> list[Growth]:
  before_classes = before.classes_of(category)
  after_classes = after.classes_of(category)
  growths = (
      Growth(name, before_classes.get(name, Metrics()),
             after_classes.get(name, Metrics()))
      for name in before_classes.keys() | after_classes.keys())
  return [growth for growth in growths if growth.has_grown()]


def top_by_dominated_bytes(growths: list[Growth], limit: int) -> list[Growth]:
  return sorted((g for g in growths if g.dominated_bytes_delta > 0),
                key=lambda g: (g.dominated_bytes_delta, g.obj_count_delta),
                reverse=True)[:limit]


def top_by_instance_count(growths: list[Growth], limit: int) -> list[Growth]:
  return sorted((g for g in growths if g.obj_count_delta > 0),
                key=lambda g: (g.obj_count_delta, g.dominated_bytes_delta),
                reverse=True)[:limit]


def format_delta(before: int, after: int) -> str:
  delta = after - before
  if delta == 0:
    return f'{before:,} (unchanged)'
  if before == 0:
    return f'0 -> {after:,} (new)'
  return f'{before:,} -> {after:,} ({delta:+,}, {delta / before:+.1%})'


def print_summary(before: HeapSummary, after: HeapSummary) -> None:
  rows = (
      ('Total objects', before.total_obj_count, after.total_obj_count),
      ('Total bytes', before.total_bytes, after.total_bytes),
      ('Reachable objects', before.reachable_obj_count,
       after.reachable_obj_count),
      ('Reachable bytes', before.reachable_bytes, after.reachable_bytes),
  )
  print('\n### Heap Summary\n')
  print('| Metric | Before -> after |')
  print('| :--- | :--- |')
  for label, before_value, after_value in rows:
    print(f'| {label} | {format_delta(before_value, after_value)} |')


def print_class_growths(title: str, growths: list[Growth]) -> None:
  print(f'\n### {title}\n')
  if not growths:
    print('_No growth._')
    return
  print('| Class | Objects (before -> after) |'
        ' Dominated bytes (before -> after) | Self bytes delta |')
  print('| :--- | :--- | :--- | :--- |')
  for growth in growths:
    print(
        f'| `{growth.name}` |'
        f' {format_delta(growth.before.obj_count, growth.after.obj_count)} |'
        f' {format_delta(growth.before.dominated_bytes, growth.after.dominated_bytes)} |'
        f' {growth.self_bytes_delta:+,} |')


def report(args: argparse.Namespace) -> None:
  before_snapshots = load_snapshots(args.before_session, args.trace_processor)
  after_snapshots = (
      before_snapshots if args.after_session == args.before_session else
      load_snapshots(args.after_session, args.trace_processor))

  before_id = select_snapshot(before_snapshots, args.before_session,
                              args.before_upid, args.before_ts)
  after_id = select_snapshot(after_snapshots, args.after_session,
                             args.after_upid, args.after_ts)
  before = before_snapshots[before_id]
  after = after_snapshots[after_id]
  if args.after_session == args.before_session and before_id == after_id:
    available = ', '.join(
        describe(s, before_snapshots[s]) for s in sorted(before_snapshots))
    raise HeapDiffError('before and after select the same snapshot; pass '
                        f'--before-ts and --after-ts. Available: {available}')
  print(f'- **Before**: {describe(before_id, before)}')
  print(f'- **After**: {describe(after_id, after)}')

  app_growths = class_growths(before, after, APP_CLASS)
  libcore_growths = class_growths(before, after, LIBCORE_OR_ARRAY)
  print_summary(before.summary, after.summary)
  print_class_growths('Top Application Classes by Dominated Bytes Growth',
                      top_by_dominated_bytes(app_growths, TOP_APP_CLASSES))
  print_class_growths('Top Application Classes by Instance Count Growth',
                      top_by_instance_count(app_growths, TOP_APP_CLASSES))
  print_class_growths(
      'Top Libcore / Array Classes by Dominated Bytes Growth',
      top_by_dominated_bytes(libcore_growths, TOP_LIBCORE_OR_ARRAY_CLASSES))


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('before_session', help='Session name of the before dump.')
  parser.add_argument('after_session', help='Session name of the after dump.')
  parser.add_argument('--before-upid', type=int)
  parser.add_argument('--before-ts', type=int)
  parser.add_argument('--after-upid', type=int)
  parser.add_argument('--after-ts', type=int)
  parser.add_argument('--trace-processor', default='trace_processor')
  return parser.parse_args()


def main() -> None:
  try:
    report(parse_args())
  except HeapDiffError as error:
    sys.exit(f'Error: {error}')


if __name__ == '__main__':
  main()

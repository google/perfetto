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
"""Explains why Java heap objects are alive, from a warm trace_processor session.

For each target (a class or an object id) it prints:
  1. for a class, how its reachable instances group by their two nearest
     dominators; the largest group supplies the traced instance,
  2. the dominator chain of that instance, with the field through which each
     dominator holds the next object, and
  3. the shortest strong reference path from a GC root to that instance.

Both walks are bounded. Hand-written recursive queries over
heap_graph_reference are not: the table holds millions of cyclic edges, so a
top-down recursion or a self-join runs until the caller gives up.
"""

import argparse
import csv
from dataclasses import dataclass
import io
import subprocess
import sys

SNAPSHOTS_QUERY = """
INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;

SELECT
  s.upid,
  s.graph_sample_ts,
  COALESCE(p.name, 'pid=' || p.pid) AS process_name
FROM android_heap_graph_stats AS s
JOIN process AS p ON s.upid = p.id;
"""

# Instances of one class can be alive for different reasons: a class that grew
# from 3 to 53 instances usually keeps its 3 old ones in a long-lived cache,
# and tracing an arbitrary instance may explain the cache instead of the
# growth. Grouping the instances by their two nearest dominators separates the
# populations, and the traced instance is taken from the largest one.
HOLDER_GROUPS_QUERY = """
INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

SELECT
  COALESCE(c1.deobfuscated_name, c1.name, '(GC root)') AS dominator,
  COALESCE(c2.deobfuscated_name, c2.name, '(GC root)') AS dominator_of_dominator,
  COUNT(*) AS instances,
  MIN(o.id) AS example_object_id
FROM heap_graph_object o
JOIN heap_graph_class c ON o.type_id = c.id
JOIN heap_graph_dominator_tree d ON d.id = o.id
LEFT JOIN heap_graph_object o1 ON o1.id = d.idom_id
LEFT JOIN heap_graph_class c1 ON o1.type_id = c1.id
LEFT JOIN heap_graph_dominator_tree d1 ON d1.id = d.idom_id
LEFT JOIN heap_graph_object o2 ON o2.id = d1.idom_id
LEFT JOIN heap_graph_class c2 ON o2.type_id = c2.id
WHERE o.upid = {upid}
  AND o.graph_sample_ts = {ts}
  AND o.reachable = 1
  AND COALESCE(c.deobfuscated_name, c.name) = '{class_name}'
  {native_size_filter}
GROUP BY 1, 2
ORDER BY instances DESC, example_object_id;
"""

OBJECT_CLASS_QUERY = """
SELECT COALESCE(c.deobfuscated_name, c.name) AS class_name
FROM heap_graph_object o
JOIN heap_graph_class c ON o.type_id = c.id
WHERE o.id = {object_id};
"""

# field_from_dominator is NULL when the dominator reaches the object only
# through intermediate objects that are not dominators themselves.
DOMINATOR_CHAIN_QUERY = """
INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

WITH RECURSIVE chain(id, depth) AS (
  SELECT {object_id}, 0
  UNION ALL
  SELECT d.idom_id, chain.depth + 1
  FROM chain
  JOIN heap_graph_dominator_tree d ON d.id = chain.id
  WHERE d.idom_id IS NOT NULL AND chain.depth < {max_depth}
)
SELECT
  chain.depth AS step,
  chain.id AS object_id,
  COALESCE(c.deobfuscated_name, c.name) AS class_name,
  o.root_type,
  (
    SELECT r.field_name
    FROM heap_graph_reference r
    JOIN heap_graph_dominator_tree d2 ON d2.id = chain.id
    WHERE r.owned_id = chain.id AND r.owner_id = d2.idom_id
    LIMIT 1
  ) AS field,
  (
    SELECT d3.dominated_size_bytes + d3.dominated_native_size_bytes
    FROM heap_graph_dominator_tree d3
    WHERE d3.id = chain.id
  ) AS retained_bytes
FROM chain
JOIN heap_graph_object o ON o.id = chain.id
JOIN heap_graph_class c ON o.type_id = c.id
ORDER BY chain.depth DESC;
"""

# Reference.referent edges are excluded so the path only uses references that
# actually prevent collection. One BFS serves every target: its cost is the
# walk over the whole graph, not the number of targets.
SHORTEST_PATH_QUERY = """
INCLUDE PERFETTO MODULE graphs.search;

WITH bfs_tree AS MATERIALIZED (
  SELECT node_id, parent_node_id
  FROM graph_reachable_bfs!(
    (
      SELECT owner_id AS source_node_id, owned_id AS dest_node_id
      FROM heap_graph_reference
      WHERE owned_id IS NOT NULL
        AND field_name NOT GLOB 'java.lang.ref.*Reference.referent'
    ),
    (
      SELECT id AS node_id
      FROM heap_graph_object
      WHERE upid = {upid} AND graph_sample_ts = {ts} AND root_type IS NOT NULL
    )
  )
),
chain(target_id, node_id, parent_node_id, dist) AS (
  SELECT node_id, node_id, parent_node_id, 0
  FROM bfs_tree
  WHERE node_id IN ({object_ids})
  UNION ALL
  SELECT chain.target_id, b.node_id, b.parent_node_id, chain.dist + 1
  FROM chain
  JOIN bfs_tree b ON b.node_id = chain.parent_node_id
  WHERE chain.parent_node_id IS NOT NULL AND chain.dist < {max_depth}
)
SELECT
  chain.target_id,
  chain.dist AS step,
  chain.node_id AS object_id,
  COALESCE(c.deobfuscated_name, c.name) AS class_name,
  o.root_type,
  (
    SELECT r.field_name
    FROM heap_graph_reference r
    WHERE r.owner_id = chain.parent_node_id AND r.owned_id = chain.node_id
    LIMIT 1
  ) AS field
FROM chain
JOIN heap_graph_object o ON o.id = chain.node_id
JOIN heap_graph_class c ON o.type_id = c.id
ORDER BY chain.target_id, chain.dist DESC;
"""

DEFAULT_MAX_DEPTH = 40
MAX_HOLDER_GROUPS_SHOWN = 5
# How `trace_processor query` renders SQL NULL in its CSV output.
NULL_CELL = '[NULL]'


class RetentionError(Exception):
  """Raised when a retention path cannot be computed."""


@dataclass(frozen=True)
class Snapshot:
  """One heap graph dump: one process at one sample timestamp."""
  upid: int
  graph_sample_ts: int
  process_name: str

  def describe(self) -> str:
    return (f'`upid={self.upid}` (`{self.process_name}`), '
            f'`graph_sample_ts={self.graph_sample_ts}`')


@dataclass(frozen=True)
class HolderGroup:
  """Instances of a class that share their two nearest dominators' classes."""
  dominator: str
  dominator_of_dominator: str
  instances: int
  example_object_id: int


@dataclass(frozen=True)
class Target:
  """One reachable object whose retention is explained."""
  object_id: int
  class_name: str
  holder_groups: tuple[HolderGroup, ...] = ()

  def title(self) -> str:
    if not self.holder_groups:
      return f'`{self.class_name}` (object {self.object_id})'
    total = sum(g.instances for g in self.holder_groups)
    return (
        f'`{self.class_name}` (object {self.object_id}, from the largest of '
        f'the holder groups of its {total:,} reachable instances)')


class Session:
  """A warm trace_processor session holding a Java heap graph."""

  def __init__(self, name: str, trace_processor: str):
    self._name = name
    self._trace_processor = trace_processor

  def query(self, sql: str) -> list[dict[str, str]]:
    command = [
        self._trace_processor, 'query', '--remote', self._name, '-f', '-'
    ]
    try:
      result = subprocess.run(
          command, input=sql, capture_output=True, text=True, check=True)
    except FileNotFoundError as error:
      raise RetentionError(f'{self._trace_processor} not found; pass '
                           '--trace-processor with its path') from error
    except subprocess.CalledProcessError as error:
      raise RetentionError(
          f'querying session `{self._name}` failed: {error.stderr.strip()}'
      ) from error
    return list(csv.DictReader(io.StringIO(result.stdout)))


def sql_string(value: str) -> str:
  return value.replace("'", "''")


def select_snapshot(session: Session, upid: int | None,
                    ts: int | None) -> Snapshot:
  """Selects the single snapshot matching the filters; fails if ambiguous."""
  snapshots = [
      Snapshot(int(r['upid']), int(r['graph_sample_ts']), r['process_name'])
      for r in session.query(SNAPSHOTS_QUERY)
  ]
  matches = [
      s for s in snapshots if (upid is None or s.upid == upid) and
      (ts is None or s.graph_sample_ts == ts)
  ]
  if len(matches) == 1:
    return matches[0]
  available = ', '.join(s.describe() for s in snapshots) or 'none'
  raise RetentionError(
      f'{len(matches)} heap graph snapshots match upid={upid}, '
      f'graph_sample_ts={ts}; select one with --upid / --ts. '
      f'Available: {available}')


def class_target(session: Session, snapshot: Snapshot, class_name: str,
                 native_size: int | None) -> Target:
  native_size_filter = ('' if native_size is None else
                        f'AND o.native_size = {native_size}')
  rows = session.query(
      HOLDER_GROUPS_QUERY.format(
          upid=snapshot.upid,
          ts=snapshot.graph_sample_ts,
          class_name=sql_string(class_name),
          native_size_filter=native_size_filter))
  if not rows:
    raise RetentionError(
        f'no reachable instance of `{class_name}` in {snapshot.describe()}' +
        ('' if native_size is None else f' with native_size={native_size}'))
  groups = tuple(
      HolderGroup(r['dominator'], r['dominator_of_dominator'],
                  int(r['instances']), int(r['example_object_id']))
      for r in rows)
  return Target(groups[0].example_object_id, class_name, groups)


def object_target(session: Session, object_id: int) -> Target:
  rows = session.query(OBJECT_CLASS_QUERY.format(object_id=object_id))
  if not rows:
    raise RetentionError(f'no heap graph object with id {object_id}')
  return Target(object_id, rows[0]['class_name'])


def cell(row: dict[str, str], column: str) -> str:
  """Returns a CSV value, mapping trace_processor's `[NULL]` to empty."""
  value = row[column]
  return '' if value == NULL_CELL else value


def print_path(title: str, rows: list[dict[str, str]]) -> None:
  print(f'\n#### {title}\n')
  if not rows:
    print('_Not reachable through strong references._')
    return
  extra = 'retained_bytes' in rows[0]
  print('| Step | Object | Class | Root type | Field |' +
        (' Retained bytes |' if extra else ''))
  print('| :--- | :--- | :--- | :--- | :--- |' + (' :--- |' if extra else ''))
  for r in rows:
    retained = f' {int(cell(r, "retained_bytes") or 0):,} |' if extra else ''
    print(f'| {r["step"]} | {r["object_id"]} | `{r["class_name"]}` |'
          f' {cell(r, "root_type")} | {cell(r, "field")} |{retained}')


def print_holder_groups(groups: tuple[HolderGroup, ...]) -> None:
  if len(groups) < 2:
    return
  print('\n#### Holder groups (nearest dominators of each instance)\n')
  print('| Dominator | Its dominator | Instances | Example object |')
  print('| :--- | :--- | :--- | :--- |')
  for g in groups[:MAX_HOLDER_GROUPS_SHOWN]:
    print(
        f'| `{g.dominator}` | `{g.dominator_of_dominator}` | {g.instances:,} |'
        f' {g.example_object_id} |')
  hidden = len(groups) - MAX_HOLDER_GROUPS_SHOWN
  if hidden > 0:
    print(f'\n_{hidden} smaller groups not shown._')


def report(args: argparse.Namespace) -> None:
  if not args.class_name and not args.object_id:
    raise RetentionError('pass at least one --class or --object-id')
  session = Session(args.session, args.trace_processor)
  snapshot = select_snapshot(session, args.upid, args.ts)
  targets = [
      class_target(session, snapshot, c, args.native_size)
      for c in args.class_name
  ]
  targets += [object_target(session, o) for o in args.object_id]
  print(f'- **Snapshot**: {snapshot.describe()}')

  paths: dict[str, list[dict[str, str]]] = {}
  if not args.dominators_only:
    for row in session.query(
        SHORTEST_PATH_QUERY.format(
            upid=snapshot.upid,
            ts=snapshot.graph_sample_ts,
            object_ids=', '.join(str(t.object_id) for t in targets),
            max_depth=args.max_depth)):
      paths.setdefault(row['target_id'], []).append(row)

  for target in targets:
    print(f'\n### {target.title()}')
    print_holder_groups(target.holder_groups)
    print_path(
        'Dominator chain (GC root first)',
        session.query(
            DOMINATOR_CHAIN_QUERY.format(
                object_id=target.object_id, max_depth=args.max_depth)))
    if not args.dominators_only:
      print_path('Shortest strong path from a GC root',
                 paths.get(str(target.object_id), []))


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
  parser.add_argument('session', help='Session name of the heap dump.')
  parser.add_argument(
      '--class',
      dest='class_name',
      action='append',
      default=[],
      help=('Fully qualified class to explain. Its instances are grouped by '
            'their nearest dominators and one instance of the largest group '
            'is traced. Repeat for several classes.'))
  parser.add_argument(
      '--native-size',
      type=int,
      help=('Only consider instances with this native_size for every --class, '
            'e.g. one Bitmap size bucket.'))
  parser.add_argument(
      '--object-id',
      type=int,
      action='append',
      default=[],
      help='heap_graph_object id to explain. Repeatable.')
  parser.add_argument('--upid', type=int)
  parser.add_argument('--ts', type=int, help='graph_sample_ts of the dump.')
  parser.add_argument(
      '--dominators-only',
      action='store_true',
      help=('Skip the shortest-path BFS, which takes up to a few minutes on a '
            'large heap.'))
  parser.add_argument('--max-depth', type=int, default=DEFAULT_MAX_DEPTH)
  parser.add_argument('--trace-processor', default='trace_processor')
  return parser.parse_args()


def main() -> None:
  try:
    report(parse_args())
  except RetentionError as error:
    sys.exit(f'Error: {error}')


if __name__ == '__main__':
  main()

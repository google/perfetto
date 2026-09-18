#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
# SPDX-License-Identifier: Apache-2.0
"""Deterministic tree inputs, equivalent SQL, and an independent integer oracle.

NULL aggregate inputs contribute zero (including all-NULL subtrees), matching
TREE ACCUMULATE rather than the SQL SUM empty/all-NULL result. Rows are deliberately
wide enough to exercise integer, floating, string and NULL result transport.
"""

from dataclasses import dataclass
import math
import random

COLUMNS = ('id', 'parent_id', 'value', 'weight', 'ratio', 'label')
SHAPES = ('balanced', 'star', 'chain', 'forest')
STAGES = {
    'scan': (),
    'scan_int': (),
    'scan_numeric': (),
    'down': (('DOWN', (('value', 'path'),)),),
    'up': (('UP', (('value', 'total'),)),),
    'up_multi': (('UP', (('value', 'total'), ('weight', 'mass'))),),
    'down_up': (('DOWN', (('value', 'path'),)), ('UP', (('path', 'total'),))),
    'up_down': (('UP', (('value', 'total'),)), ('DOWN', (('total', 'path'),))),
    'down_down_up': (
        ('DOWN', (('value', 'path'),)), ('DOWN', (('path', 'path2'),)),
        ('UP', (('path2', 'total'),))),
}
WORKLOADS = tuple(STAGES)


@dataclass(frozen=True)
class DatasetSpec:
  rows: int
  shape: str = 'balanced'
  seed: int = 1729
  shuffled: bool = True
  all_null: bool = False
  dense_ids: bool = False

  def __post_init__(self):
    if self.rows < 0:
      raise ValueError('rows must be non-negative')
    if self.shape not in SHAPES:
      raise ValueError('unknown shape: ' + self.shape)

  def parents(self):
    result = []
    for i in range(self.rows):
      if i == 0 or (self.shape == 'forest' and i % 17 == 0):
        result.append(None)
      elif self.shape == 'chain':
        result.append(i - 1)
      elif self.shape == 'star':
        result.append(0)
      elif self.shape == 'forest':
        root = (i // 17) * 17
        result.append(root + (i - root - 1) // 2)
      else:
        result.append((i - 1) // 2)
    return result

  def data(self):
    # IDs are sparse and unrelated to both insertion and traversal order.
    rng = random.Random(self.seed)
    ids = list(range(self.rows)) if self.dense_ids else [
        1009 + 97 * i for i in range(self.rows)
    ]
    if not self.dense_ids:
      rng.shuffle(ids)
    result = []
    for i, parent in enumerate(self.parents()):
      value = None if self.all_null or i % 11 == 0 else rng.randrange(-100, 101)
      weight = None if self.all_null or i % 7 == 0 else rng.randrange(-5, 8)
      ratio = None if i % 13 == 0 else (i % 37 - 18) / 8.0
      label = None if i % 19 == 0 else ("node ' %d\t\nλ" % (i % 23))
      result.append((ids[i], None if parent is None else ids[parent], value,
                     weight, ratio, label))
    if self.shuffled:
      rng.shuffle(result)
    return result


def _literal(value):
  if value is None:
    return 'NULL'
  if isinstance(value, str):
    return "'" + value.replace("'", "''") + "'"
  return repr(value)


def canonical_row(row):
  """Exact typed TSV encoding: doubles use hexfloat, strings use UTF-8 hex."""
  cells = []
  for value in row:
    if value is None:
      cells.append('N')
    elif isinstance(value, int):
      cells.append('I:' + str(value))
    elif isinstance(value, float):
      if not math.isfinite(value):
        raise ValueError(
            'non-finite values are outside this benchmark contract')
      cells.append('F:' + value.hex())
    elif isinstance(value, str):
      cells.append('S:' + value.encode('utf-8').hex())
    else:
      raise TypeError(type(value))
  return '\t'.join(cells)


def canonical_rows(rows):
  return sorted(canonical_row(row) for row in rows)


@dataclass(frozen=True)
class Workload:
  spec: DatasetSpec
  name: str

  def __post_init__(self):
    if self.name not in STAGES:
      raise ValueError('unknown workload: ' + self.name)

  @property
  def source_columns(self):
    if self.name == 'scan_int':
      return ('value',)
    if self.name == 'scan_numeric':
      return COLUMNS[:4]
    return COLUMNS

  @property
  def columns(self):
    return self.source_columns + tuple(
        out for _, sums in STAGES[self.name] for _, out in sums)

  @property
  def estimated_recursive_rows(self):
    depths = []
    for parent in self.spec.parents():
      depths.append(0 if parent is None else depths[parent] + 1)
    closure = sum(d + 1 for d in depths)
    return sum(closure if direction == 'UP' else self.spec.rows
               for direction, _ in STAGES[self.name])

  def setup_sql(self, backend):
    if backend not in ('sqlite', 'pipeline', 'pipeline_sql_scan', 'dataframe',
                       'duckdb'):
      raise ValueError('unknown backend: ' + backend)
    dataframe = backend in ('pipeline', 'dataframe')
    table = 'staging' if dataframe else 'tree'
    # Explicit affinities ensure NULL/empty inputs have stable SQL column types.
    id_type = 'BIGINT' if backend == 'duckdb' else 'INTEGER'
    types = {
        'id': f'{id_type} PRIMARY KEY',
        'parent_id': 'BIGINT',
        'value': 'BIGINT',
        'weight': 'BIGINT',
        'ratio': 'DOUBLE',
        'label': 'TEXT'
    }
    definitions = [f'{name} {types[name]}' for name in self.source_columns]
    if dataframe:
      definitions.append('insertion_ordinal INTEGER NOT NULL')
    parts = [f'CREATE TABLE {table} (' + ', '.join(definitions) + ');']
    indices = [COLUMNS.index(name) for name in self.source_columns]
    rows = [tuple(row[index] for index in indices) for row in self.spec.data()]
    if dataframe:
      # SQLite INTEGER PRIMARY KEY clusters by id. Preserve the specified
      # dataframe layout explicitly instead of inheriting that staging order.
      rows = [row + (index,) for index, row in enumerate(rows)]
    for begin in range(0, len(rows), 500):
      values = ',\n'.join('(' + ','.join(map(_literal, row)) + ')'
                          for row in rows[begin:begin + 500])
      parts.append(f'INSERT INTO {table} VALUES {values};')
    if dataframe:
      parts += [
          'CREATE PERFETTO TABLE tree AS SELECT ' +
          ', '.join(self.source_columns) +
          ' FROM staging ORDER BY insertion_ordinal;', 'DROP TABLE staging;'
      ]
      if 'id' in self.source_columns:
        parts.append('CREATE PERFETTO INDEX tree_id ON tree(id);')
      if 'parent_id' in self.source_columns:
        parts.append('CREATE PERFETTO INDEX tree_parent ON tree(parent_id);')
    elif 'parent_id' in self.source_columns:
      parts.append('CREATE INDEX tree_parent ON tree(parent_id);')
    return '\n'.join(parts)

  def query_sql(self, backend):
    if backend in ('pipeline', 'pipeline_sql_scan'):
      result = 'FROM tree' if backend == 'pipeline' else 'FROM (SELECT * FROM tree)'
      for direction, sums in STAGES[self.name]:
        result += ' |> TREE ACCUMULATE ' + direction + ' ' + ', '.join(
            f'SUM({source}) AS {out}' for source, out in sums)
      return result + ';'
    if backend not in ('sqlite', 'dataframe', 'duckdb'):
      raise ValueError('unknown backend: ' + backend)
    if not STAGES[self.name]:
      return 'SELECT ' + ', '.join(self.source_columns) + ' FROM tree;'
    ctes = []
    previous = 'tree'
    columns = list(COLUMNS)
    for index, (direction, sums) in enumerate(STAGES[self.name]):
      walk, stage = f'walk{index}', f'stage{index}'
      output = [out for _, out in sums]
      names = ', '.join(['id'] + output)
      if direction == 'DOWN':
        roots = ', '.join(['id'] +
                          [f'COALESCE({source}, 0)' for source, _ in sums])
        children = ', '.join(
            ['c.id'] +
            [f'p.{out} + COALESCE(c.{source}, 0)' for source, out in sums])
        # SQLite can otherwise put the dataframe scan outside the recursive
        # queue and rescan every row per node. Keep the queue outermost so
        # parent_id lookups use its index; leave DuckDB's join order optimizer
        # free to choose. Final joins remain free to avoid repeated CTE work.
        recursive_join = (
            f'FROM {previous} c JOIN {walk} p ON c.parent_id = p.id'
            if backend == 'duckdb' else
            f'FROM {walk} p CROSS JOIN {previous} c WHERE c.parent_id = p.id')
        ctes.append(f'{walk}({names}) AS (SELECT {roots} FROM {previous} '
                    f'WHERE parent_id IS NULL UNION ALL SELECT {children} '
                    f'{recursive_join})')
        result = ', '.join('a.' + name for name in output)
        ctes.append(f'{stage} AS (SELECT t.*, {result} FROM {previous} t '
                    f'JOIN {walk} a ON t.id = a.id)')
      else:
        # Propagate each descendant's values towards its ancestors; group once.
        # A rooted DOWN query is linear; this natural UP baseline is O(N*depth).
        names = ', '.join(['id', 'parent_id'] + output)
        start = ', '.join(['id', 'parent_id'] +
                          [f'COALESCE({source}, 0)' for source, _ in sums])
        recurse = ', '.join(['p.id', 'p.parent_id'] +
                            ['w.' + out for out in output])
        ctes.append(f'{walk}({names}) AS (SELECT {start} FROM {previous} '
                    f'UNION ALL SELECT {recurse} FROM {walk} w '
                    f'JOIN {previous} p ON w.parent_id = p.id)')
        aggregates = ', '.join(
            f'CAST(SUM({out}) AS BIGINT) AS {out}' for out in output)
        ctes.append(
            f'agg{index} AS (SELECT id, {aggregates} FROM {walk} GROUP BY id)')
        result = ', '.join('a.' + name for name in output)
        ctes.append(f'{stage} AS (SELECT t.*, {result} FROM {previous} t '
                    f'JOIN agg{index} a ON t.id = a.id)')
      columns.extend(output)
      previous = stage
    return 'WITH RECURSIVE\n' + ',\n'.join(ctes) + '\nSELECT ' + ', '.join(
        columns) + f' FROM {previous};'

  def expected_rows(self):
    if not STAGES[self.name]:
      indices = [COLUMNS.index(name) for name in self.source_columns]
      return [
          tuple(row[index] for index in indices) for row in self.spec.data()
      ]
    rows = {row[0]: list(row) for row in self.spec.data()}
    children = {node: [] for node in rows}
    roots = []
    for node, row in rows.items():
      if row[1] is None:
        roots.append(node)
      else:
        children[row[1]].append(node)
    order = []
    pending = list(roots)
    while pending:
      node = pending.pop()
      order.append(node)
      pending.extend(children[node])
    columns = list(COLUMNS)
    for direction, sums in STAGES[self.name]:
      # Compute all sums using the incoming stage's schema, then publish outputs.
      results = []
      for source, _ in sums:
        source_index = columns.index(source)
        totals = {node: row[source_index] or 0 for node, row in rows.items()}
        if direction == 'UP':
          for node in reversed(order):
            parent = rows[node][1]
            if parent is not None:
              totals[parent] += totals[node]
        else:
          for node in order:
            parent = rows[node][1]
            if parent is not None:
              totals[node] += totals[parent]
        if any(
            not -(1 << 63) <= total < (1 << 63) for total in totals.values()):
          raise OverflowError('generated workload exceeds signed Int64')
        results.append(totals)
      for node, row in rows.items():
        row.extend(totals[node] for totals in results)
      columns.extend(out for _, out in sums)
    return [tuple(row) for row in rows.values()]

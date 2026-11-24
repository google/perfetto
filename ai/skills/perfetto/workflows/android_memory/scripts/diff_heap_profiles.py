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
"""Diffs the heap allocation profiles of two warm trace_processor sessions."""

import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass, field
import io
import subprocess
import sys

# Lists every (process, heap_name) profile recorded in the trace with its
# whole-trace totals. In heap_profile_allocation, allocations have positive
# size/count and frees have negative size/count, so SUM(size) is net unreleased
# bytes while SUM(MAX(size, 0)) sums only positive allocation records.
PROFILE_LIST_QUERY = """
SELECT
  a.upid,
  COALESCE(p.name, 'pid=' || p.pid) AS process_name,
  a.heap_name,
  SUM(a.size) AS unreleased_bytes,
  SUM(MAX(a.size, 0)) AS alloc_bytes,
  SUM(a.count) AS unreleased_count,
  SUM(MAX(a.count, 0)) AS alloc_count
FROM heap_profile_allocation AS a
JOIN process AS p USING (upid)
GROUP BY a.upid, process_name, a.heap_name
ORDER BY alloc_bytes DESC, a.upid, a.heap_name;
"""

# Expands the sampled callsites of one (upid, heap_name) profile into a
# prefix tree of stack frames (id -> parent_id) with per-node self metrics.
CALLSTACK_TREE_QUERY_TEMPLATE = """
INCLUDE PERFETTO MODULE android.memory.heap_profile.callstacks;

SELECT
  id,
  parent_id,
  COALESCE(name, '[unknown]') AS function_name,
  COALESCE(mapping_name, '') AS mapping_name,
  self_size AS self_unreleased_bytes,
  self_alloc_size AS self_alloc_bytes,
  self_count AS self_unreleased_count,
  self_alloc_count
FROM _android_heap_profile_callstacks_for_allocations!((
  SELECT
    callsite_id,
    size,
    count,
    MAX(size, 0) AS alloc_size,
    MAX(count, 0) AS alloc_count
  FROM heap_profile_allocation
  WHERE upid = {upid} AND heap_name = '{heap_name}'
));
"""

ART_HEAP = 'com.android.art'
DEFAULT_TOP_FUNCTIONS = 10
DEFAULT_TOP_CALLSTACKS = 10

# ART runtime (libart.so, core-libart.jar) and Bionic libc live under /apex/,
# and JIT-compiled trampolines live in anonymous dalvik-jit-code-cache VMA
# regions. Excluding these from the function table keeps application, framework,
# and native library frames at the top instead of runtime allocator internals.
SYSTEM_RUNTIME_MAPPINGS = (
    '/apex/com.android.art/',
    '/apex/com.android.runtime/',
    '/[anon_shmem:dalvik-jit-code-cache]',
)

# JIT-compiled Java frames reside in anonymous shared-memory VMAs such as
# "/[anon:dalvik-jit-code-cache]" rather than a real .so/.jar/.apk file, so
# omitting the VMA name avoids cluttering every JITted Java method label.
ANON_MAPPING_PREFIX = '/[anon'


class HeapProfileDiffError(Exception):
  """Raised when diffing heap allocation profiles fails."""


@dataclass(frozen=True, order=True)
class ProfileId:
  """Identifies one (process, heap) allocation profile in a trace."""
  upid: int
  heap_name: str


@dataclass(frozen=True)
class Metrics:
  unreleased_bytes: int = 0
  alloc_bytes: int = 0
  unreleased_count: int = 0
  alloc_count: int = 0

  def __add__(self, other: 'Metrics') -> 'Metrics':
    return Metrics(
        self.unreleased_bytes + other.unreleased_bytes,
        self.alloc_bytes + other.alloc_bytes,
        self.unreleased_count + other.unreleased_count,
        self.alloc_count + other.alloc_count,
    )


@dataclass(frozen=True)
class FrameNode:
  node_id: int
  parent_id: int | None
  function_name: str
  mapping_name: str
  self_metrics: Metrics


@dataclass
class Profile:
  process_name: str
  summary: Metrics
  functions: dict[str, Metrics] = field(default_factory=dict)
  callstacks: dict[str, Metrics] = field(default_factory=dict)


@dataclass(frozen=True)
class Growth:
  """Before/after metrics of a function or callstack."""
  name: str
  before: Metrics
  after: Metrics

  @property
  def alloc_bytes_delta(self) -> int:
    return self.after.alloc_bytes - self.before.alloc_bytes

  @property
  def unreleased_bytes_delta(self) -> int:
    return self.after.unreleased_bytes - self.before.unreleased_bytes

  @property
  def alloc_count_delta(self) -> int:
    return self.after.alloc_count - self.before.alloc_count


def is_system_runtime_mapping(mapping_name: str) -> bool:
  return mapping_name.startswith(SYSTEM_RUNTIME_MAPPINGS)


def format_frame(function_name: str, mapping_name: str) -> str:
  if not mapping_name or mapping_name.startswith(ANON_MAPPING_PREFIX):
    return function_name
  short_map = mapping_name.rsplit('/', 1)[-1]
  return f'{function_name} ({short_map})'


def compact_callstack(chain: list[FrameNode]) -> str:
  """Strips intermediate /apex/ runtime frames, keeping root, app/lib, and leaf."""
  if len(chain) <= 2:
    return ' -> '.join(
        format_frame(f.function_name, f.mapping_name) for f in chain)
  kept = [
      f for i, f in enumerate(chain)
      if i in (0,
               len(chain) - 1) or not is_system_runtime_mapping(f.mapping_name)
  ]
  return ' -> '.join(
      format_frame(f.function_name, f.mapping_name) for f in kept)


def query(session: str, trace_processor: str, sql: str) -> list[dict[str, str]]:
  command = [trace_processor, 'query', '--remote', session, '-f', '-']
  try:
    result = subprocess.run(
        command, input=sql, capture_output=True, text=True, check=True)
  except FileNotFoundError as error:
    raise HeapProfileDiffError(
        f'{trace_processor} not found; pass --trace-processor with its path'
    ) from error
  except subprocess.CalledProcessError as error:
    raise HeapProfileDiffError(
        f'querying session `{session}` failed: {error.stderr.strip()}'
    ) from error
  return list(csv.DictReader(io.StringIO(result.stdout)))


def select_profile_row(rows: list[dict[str,
                                       str]], session: str, upid: int | None,
                       heap_name: str | None) -> dict[str, str]:
  if not rows:
    raise HeapProfileDiffError(
        f'`{session}` holds no heap_profile_allocation rows')
  matches = [
      r for r in rows if (upid is None or int(r['upid']) == upid) and
      (heap_name is None or r['heap_name'] == heap_name)
  ]
  available = ', '.join(
      f"upid={r['upid']} ({r['process_name']}) heap={r['heap_name']}"
      for r in rows)
  if not matches:
    raise HeapProfileDiffError(
        f'`{session}`: no profile with upid={upid}, heap_name={heap_name}. '
        f'Available: {available}')
  if len(matches) > 1:
    raise HeapProfileDiffError(
        f'`{session}`: multiple (process, heap) profiles found; select one '
        f'with --before-upid / --after-upid and --heap-name. '
        f'Available: {available}')
  return matches[0]


def load_profile(session: str, trace_processor: str, upid: int | None,
                 heap_name: str | None) -> tuple[ProfileId, Profile]:
  row = select_profile_row(
      query(session, trace_processor, PROFILE_LIST_QUERY), session, upid,
      heap_name)
  profile_id = ProfileId(int(row['upid']), row['heap_name'])
  sql = CALLSTACK_TREE_QUERY_TEMPLATE.format(
      upid=profile_id.upid, heap_name=profile_id.heap_name)

  nodes: dict[int, FrameNode] = {}
  for r in query(session, trace_processor, sql):
    parent_raw = r['parent_id']
    parent_id = None if parent_raw in ('', '[NULL]') else int(parent_raw)
    node_id = int(r['id'])
    nodes[node_id] = FrameNode(
        node_id=node_id,
        parent_id=parent_id,
        function_name=r['function_name'],
        mapping_name=r['mapping_name'],
        self_metrics=Metrics(
            int(r['self_unreleased_bytes']),
            int(r['self_alloc_bytes']),
            int(r['self_unreleased_count']),
            int(r['self_alloc_count']),
        ),
    )

  summary = Metrics()
  functions: dict[str, Metrics] = {}
  callstacks: dict[str, Metrics] = {}
  children_by_label: dict[str, set[str]] = defaultdict(set)

  for node in nodes.values():
    m = node.self_metrics
    if m.alloc_bytes == 0 and m.unreleased_bytes == 0:
      continue
    summary = summary + m
    chain: list[FrameNode] = []
    curr: FrameNode | None = node
    while curr is not None:
      chain.append(curr)
      curr = nodes.get(curr.parent_id) if curr.parent_id is not None else None
    chain.reverse()

    stack_key = compact_callstack(chain)
    callstacks[stack_key] = callstacks.get(stack_key, Metrics()) + m

    non_apex = [
        format_frame(f.function_name, f.mapping_name)
        for f in chain
        if not is_system_runtime_mapping(f.mapping_name)
    ]
    for prev_label, next_label in zip(non_apex, non_apex[1:]):
      if prev_label != next_label:
        children_by_label[prev_label].add(next_label)

    # dict.fromkeys deduplicates recursive frames within the same callstack so
    # a recursive function's cumulative total is counted once per allocation.
    for label in dict.fromkeys(non_apex):
      functions[label] = functions.get(label, Metrics()) + m

  # Collapse pass-through wrapper frames (e.g. main -> ZygoteInit -> Looper)
  # whose cumulative metrics are 100% accounted for by a single child frame.
  specific_functions = {
      label: metrics for label, metrics in functions.items() if not any(
          functions.get(child) == metrics
          for child in children_by_label.get(label, ()))
  }

  return profile_id, Profile(
      process_name=row['process_name'],
      summary=summary,
      functions=specific_functions,
      callstacks=callstacks,
  )


def growths_between(before_map: dict[str, Metrics],
                    after_map: dict[str, Metrics]) -> list[Growth]:
  return [
      Growth(name, before_map.get(name, Metrics()),
             after_map.get(name, Metrics()))
      for name in before_map.keys() | after_map.keys()
  ]


def format_delta(before: int, after: int) -> str:
  delta = after - before
  if delta == 0:
    return f'{before:,} (unchanged)'
  if before == 0:
    return f'0 -> {after:,} (new)'
  return f'{before:,} -> {after:,} ({delta:+,}, {delta / before:+.1%})'


def print_summary(before: Metrics, after: Metrics, is_art: bool) -> None:
  rows = [
      ('Total allocated bytes', before.alloc_bytes, after.alloc_bytes),
      ('Total allocations', before.alloc_count, after.alloc_count),
  ]
  if not is_art:
    rows.extend([
        ('Unreleased bytes', before.unreleased_bytes, after.unreleased_bytes),
        ('Unreleased allocations', before.unreleased_count,
         after.unreleased_count),
    ])
  print('\n### Profile Summary\n')
  print('| Metric | Before -> after |')
  print('| :--- | :--- |')
  for label, b_val, a_val in rows:
    print(f'| {label} | {format_delta(b_val, a_val)} |')


def print_growth_table(title: str, item_header: str, growths: list[Growth],
                       is_art: bool) -> None:
  print(f'\n### {title}\n')
  if not growths:
    print('_No growth._')
    return
  if is_art:
    print(f'| {item_header} | Allocated bytes (before -> after) |'
          ' Allocations (before -> after) |')
    print('| :--- | :--- | :--- |')
    for g in growths:
      print(f'| `{g.name}` |'
            f' {format_delta(g.before.alloc_bytes, g.after.alloc_bytes)} |'
            f' {format_delta(g.before.alloc_count, g.after.alloc_count)} |')
    return

  print(f'| {item_header} | Unreleased bytes (before -> after) |'
        ' Allocated bytes (before -> after) |'
        ' Allocations (before -> after) |')
  print('| :--- | :--- | :--- | :--- |')
  for g in growths:
    print(
        f'| `{g.name}` |'
        f' {format_delta(g.before.unreleased_bytes, g.after.unreleased_bytes)} |'
        f' {format_delta(g.before.alloc_bytes, g.after.alloc_bytes)} |'
        f' {format_delta(g.before.alloc_count, g.after.alloc_count)} |')


def report(args: argparse.Namespace) -> None:
  if args.before_session == args.after_session:
    raise HeapProfileDiffError(
        'before_session and after_session must be two distinct sessions')
  before_id, before = load_profile(args.before_session, args.trace_processor,
                                   args.before_upid, args.heap_name)
  after_id, after = load_profile(args.after_session, args.trace_processor,
                                 args.after_upid, args.heap_name or
                                 before_id.heap_name)
  print(f'- **Before**: `upid={before_id.upid}` (`{before.process_name}`), '
        f'`heap_name={before_id.heap_name}`')
  print(f'- **After**: `upid={after_id.upid}` (`{after.process_name}`), '
        f'`heap_name={after_id.heap_name}`')

  is_art = after_id.heap_name == ART_HEAP
  print_summary(before.summary, after.summary, is_art)

  fn_growths = growths_between(before.functions, after.functions)
  stack_growths = growths_between(before.callstacks, after.callstacks)

  if not is_art:
    top_unrel_fn = sorted(
        (g for g in fn_growths if g.unreleased_bytes_delta > 0),
        key=lambda g: (g.unreleased_bytes_delta, g.alloc_bytes_delta),
        reverse=True)[:args.top_functions]
    print_growth_table('Top Functions by Cumulative Unreleased Bytes Growth',
                       'Function (mapping)', top_unrel_fn, is_art)

  top_alloc_fn = sorted((g for g in fn_growths if g.alloc_bytes_delta > 0),
                        key=lambda g:
                        (g.alloc_bytes_delta, g.alloc_count_delta),
                        reverse=True)[:args.top_functions]
  print_growth_table('Top Functions by Cumulative Allocated Bytes Growth',
                     'Function (mapping)', top_alloc_fn, is_art)

  top_stacks = sorted((g for g in stack_growths if g.alloc_bytes_delta > 0),
                      key=lambda g: (
                          g.unreleased_bytes_delta
                          if not is_art else g.alloc_bytes_delta,
                          g.alloc_bytes_delta,
                      ),
                      reverse=True)[:args.top_callstacks]
  print_growth_table('Top Callstacks by Growth', 'Callstack (root -> leaf)',
                     top_stacks, is_art)


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      'before_session', help='Session name of the before trace.')
  parser.add_argument('after_session', help='Session name of the after trace.')
  parser.add_argument('--before-upid', type=int)
  parser.add_argument('--after-upid', type=int)
  parser.add_argument('--heap-name')
  parser.add_argument(
      '--top-functions', type=int, default=DEFAULT_TOP_FUNCTIONS)
  parser.add_argument(
      '--top-callstacks', type=int, default=DEFAULT_TOP_CALLSTACKS)
  parser.add_argument('--trace-processor', default='trace_processor')
  return parser.parse_args()


def main() -> None:
  try:
    report(parse_args())
  except HeapProfileDiffError as error:
    sys.exit(f'Error: {error}')


if __name__ == '__main__':
  main()

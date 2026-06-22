// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Cross-trace flamegraph diff — runs purely in the UI.
//
// The same-trace flamegraph diff JOINs cur and base class-tree nodes in SQL
// via _graph_scan path hashes (see ../views/flamegraph_view.ts buildDiffMetric).
// Cross-trace can't JOIN across two trace_processor engines, so this module
// queries both engines, pairs by stable name-path in JS, and writes the
// paired rows back into the cur engine as a TEMP TABLE the flamegraph SQL
// can SELECT from. The downstream value / colour-hint SQL is identical to
// the same-trace path — only the source of `joined` differs.

import type {Engine} from '../../../trace_processor/engine';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../../trace_processor/query_result';

export interface RawTreeRow {
  readonly id: number;
  readonly parent_id: number | null;
  readonly name: string;
  readonly root_type: string | null;
  readonly heap_type: string | null;
  readonly self_size: number;
  readonly self_count: number;
}

export interface PairedRow extends RawTreeRow {
  readonly c_self_size: number;
  readonly c_self_count: number;
  readonly b_self_size: number;
  readonly b_self_count: number;
  readonly delta_size: number;
  readonly delta_count: number;
  readonly is_new: 0 | 1;
  readonly path_hash_stable: string;
}

// Build the chunk of the stable path key contributed by one node.
function nodeKeyPart(name: string, heapType: string | null): string {
  // Use \x00 between (name, heap_type) and \x01 between levels — both are
  // illegal inside JVM class names and heap-type names, so collisions are
  // impossible.
  return `${name}\x00${heapType ?? ''}`;
}

// BFS over a single tree, assigning each node a stable name-path key.
// The key has the same shape as the _graph_scan path hash used in the
// same-trace SQL path, but kept as a string so pairing is exact.
export function computePathKeys(
  rows: ReadonlyArray<RawTreeRow>,
): Map<number, string> {
  const childrenByParent = new Map<number | null, RawTreeRow[]>();
  for (const r of rows) {
    const arr = childrenByParent.get(r.parent_id);
    if (arr === undefined) {
      childrenByParent.set(r.parent_id, [r]);
    } else {
      arr.push(r);
    }
  }
  const keys = new Map<number, string>();
  const queue: Array<readonly [number, string]> = [];
  for (const root of childrenByParent.get(null) ?? []) {
    // Roots also fold in `root_type` so e.g. ROOT_STICKY_CLASS vs
    // ROOT_JNI_GLOBAL subtrees with the same name/heap_type get distinct
    // path identities — without this they collapse and the same-trace
    // LEFT JOIN ON path_h cross-products them.
    const k = `${nodeKeyPart(root.name, root.heap_type)}\x00${root.root_type ?? ''}`;
    keys.set(root.id, k);
    queue.push([root.id, k]);
  }
  let head = 0;
  while (head < queue.length) {
    const [id, parentKey] = queue[head++];
    const kids = childrenByParent.get(id);
    if (kids === undefined) continue;
    for (const k of kids) {
      const ck = `${parentKey}\x01${nodeKeyPart(k.name, k.heap_type)}`;
      keys.set(k.id, ck);
      queue.push([k.id, ck]);
    }
  }
  return keys;
}

// Pair cur rows against base rows by stable name-path. The output mirrors
// the `joined` CTE of the same-trace SQL: every cur node appears once;
// nodes only in base are dropped (matching same-trace semantics — REMOVED
// nodes don't render as boxes on the current heap).
export function pairTrees(
  cur: ReadonlyArray<RawTreeRow>,
  base: ReadonlyArray<RawTreeRow>,
): PairedRow[] {
  const curKeys = computePathKeys(cur);
  const baseKeys = computePathKeys(base);
  // Aggregate base sizes by path key so cur nodes sharing a path with
  // multiple base tree nodes still pair against a single, summed entry
  // (mirrors the GROUP BY in the same-trace base_nodes CTE — both paths
  // avoid the cross-product hazard).
  const baseByKey = new Map<string, {selfSize: number; selfCount: number}>();
  for (const b of base) {
    const k = baseKeys.get(b.id);
    if (k === undefined) continue;
    const prev = baseByKey.get(k);
    if (prev === undefined) {
      baseByKey.set(k, {selfSize: b.self_size, selfCount: b.self_count});
    } else {
      prev.selfSize += b.self_size;
      prev.selfCount += b.self_count;
    }
  }
  // Assign each distinct path key a small dense integer id, used as
  // path_hash_stable. The raw name-path key embeds \x00 / \x01 separators,
  // and path_hash_stable is injected into an inline SQL string literal (see
  // injectPairedTable). Standard SQLite — and the native trace_processor —
  // reject an embedded NUL there (it terminates the literal at parse time);
  // only the Wasm engine happens to tolerate it, so injecting the raw key is
  // both non-portable and fragile. A dense id is NUL-free and collision-free,
  // and equal name-paths still map to equal ids — matching the same-trace
  // path's integer path_h identity (see flamegraph_view.ts buildDiffMetric).
  const pathIds = new Map<string, number>();
  const out: PairedRow[] = [];
  for (const c of cur) {
    const k = curKeys.get(c.id);
    if (k === undefined) continue;
    let pathId = pathIds.get(k);
    if (pathId === undefined) {
      pathId = pathIds.size;
      pathIds.set(k, pathId);
    }
    const b = baseByKey.get(k);
    const bSelfSize = b?.selfSize ?? 0;
    const bSelfCount = b?.selfCount ?? 0;
    out.push({
      ...c,
      c_self_size: c.self_size,
      c_self_count: c.self_count,
      b_self_size: bSelfSize,
      b_self_count: bSelfCount,
      delta_size: c.self_size - bSelfSize,
      delta_count: c.self_count - bSelfCount,
      is_new: b === undefined ? 1 : 0,
      path_hash_stable: String(pathId),
    });
  }
  return out;
}

const TREE_QUERY = (tree: string, upid: number, ts: bigint) => `
    SELECT id, parent_id,
           coalesce(name, '<' || coalesce(replace(heap_type, 'HEAP_TYPE_', ''), root_type, 'unnamed') || '>') AS name,
           root_type, heap_type, self_size, self_count
    FROM ${tree}
    WHERE upid = ${upid} AND graph_sample_ts = ${ts}
  `;

// Pull a class-tree dump (cur or base) from an engine into JS rows.
export async function fetchClassTreeRows(
  engine: Engine,
  treeTable: string,
  upid: number,
  ts: bigint,
): Promise<RawTreeRow[]> {
  const res = await engine.query(TREE_QUERY(treeTable, upid, ts));
  const out: RawTreeRow[] = [];
  for (
    const it = res.iter({
      id: NUM,
      parent_id: NUM_NULL,
      name: STR,
      root_type: STR_NULL,
      heap_type: STR_NULL,
      self_size: NUM,
      self_count: NUM,
    });
    it.valid();
    it.next()
  ) {
    out.push({
      id: it.id,
      parent_id: it.parent_id,
      name: it.name,
      root_type: it.root_type,
      heap_type: it.heap_type,
      self_size: it.self_size,
      self_count: it.self_count,
    });
  }
  return out;
}

// Quote a string for inline-VALUES SQL. The only strings injected here are
// class names (from the engine) and the numeric path_hash_stable, neither of
// which contains an embedded NUL — important because a NUL truncates a SQLite
// string literal at parse time. We only need to escape single quotes (class
// names like `a'b`).
function sqlString(s: string | null): string {
  if (s === null) return 'NULL';
  return `'${s.replace(/'/g, "''")}'`;
}

// Inject paired rows into `engine` as a temp table the flamegraph SQL can
// SELECT from. Drops any pre-existing table with the same name first so
// repeated re-pairs (different baseline picks) are safe.
//
// The column shape matches the `joined` CTE of the same-trace path so the
// downstream `value` / `color_hint` SQL is identical.
export async function injectPairedTable(
  engine: Engine,
  tableName: string,
  rows: ReadonlyArray<PairedRow>,
): Promise<void> {
  await engine.query(`DROP TABLE IF EXISTS ${tableName}`);
  await engine.query(`
    CREATE TEMP TABLE ${tableName} (
      id INTEGER,
      parent_id INTEGER,
      name TEXT,
      root_type TEXT,
      heap_type TEXT,
      c_self_size INTEGER,
      c_self_count INTEGER,
      b_self_size INTEGER,
      b_self_count INTEGER,
      delta_size INTEGER,
      delta_count INTEGER,
      is_new INTEGER,
      path_hash_stable TEXT
    )
  `);
  if (rows.length === 0) return;
  // Batched VALUES — SQLite's default SQLITE_MAX_COMPOUND_SELECT is 500;
  // 250 keeps a safe margin for very wide rows.
  const BATCH = 250;
  for (let i = 0; i < rows.length; i += BATCH) {
    const end = Math.min(i + BATCH, rows.length);
    const parts: string[] = [];
    for (let j = i; j < end; j++) {
      const r = rows[j];
      parts.push(
        `(${r.id},${r.parent_id === null ? 'NULL' : r.parent_id},` +
          `${sqlString(r.name)},${sqlString(r.root_type)},` +
          `${sqlString(r.heap_type)},` +
          `${r.c_self_size},${r.c_self_count},` +
          `${r.b_self_size},${r.b_self_count},` +
          `${r.delta_size},${r.delta_count},` +
          `${r.is_new},${sqlString(r.path_hash_stable)})`,
      );
    }
    await engine.query(
      `INSERT INTO ${tableName} (id,parent_id,name,root_type,heap_type,` +
        `c_self_size,c_self_count,b_self_size,b_self_count,delta_size,` +
        `delta_count,is_new,path_hash_stable) VALUES ${parts.join(',')}`,
    );
  }
}

// Full prep step: pull both trees from their engines, JS-pair, inject as
// a temp table in `curEngine`. Returns the temp table name. Throws on
// engine errors.
export async function prepareCrossTraceDiff(
  tableName: string,
  curEngine: Engine,
  cur: {upid: number; ts: bigint},
  baseEngine: Engine,
  base: {upid: number; ts: bigint},
  treeTable: string,
): Promise<void> {
  const [curRows, baseRows] = await Promise.all([
    fetchClassTreeRows(curEngine, treeTable, cur.upid, cur.ts),
    fetchClassTreeRows(baseEngine, treeTable, base.upid, base.ts),
  ]);
  const paired = pairTrees(curRows, baseRows);
  await injectPairedTable(curEngine, tableName, paired);
}

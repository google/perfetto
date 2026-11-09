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

// Serializes the graph upstream of an output node into one Standard SQL query.
//
// Folding: a node's clause fragment folds into its input's SELECT block when
// every clause it adds comes strictly later in SQL clause order than the
// block's latest clause (WHERE + WHERE is ANDed), and nothing else reads that
// input. Otherwise the input is emitted as a CTE and a new block reads from it.

import {ensureDefined} from '../../base/assert';
import {type GraphIndex, resolveInputs, tryGetManifest} from './graph_utils';
import type {SqlFragment, SqlInput} from './node_types';
import {
  indent,
  makeContext,
  renderOrder,
  type SerializeOptions,
  type SerializeResult,
} from './serialize_common';

interface Block {
  readonly from: string;
  /** Set when `from` is a table (or CTE) name, so a bare block is that name. */
  readonly table?: string;
  readonly where?: string;
  readonly groupBy?: readonly string[];
  readonly select?: readonly string[];
  readonly distinct?: boolean;
  readonly orderBy?: readonly string[];
  readonly limit?: number;
  readonly offset?: number;
}

// A node's output while rendering: an open SELECT block that later nodes may
// still fold into, or a name to read it by (a table or an emitted CTE).
type State =
  | {readonly kind: 'block'; readonly block: Block}
  | {readonly kind: 'name'; readonly name: string};

// SQL evaluation order of each clause.
const CLAUSE_POS = {
  from: 0,
  where: 1,
  groupBy: 2,
  select: 3,
  distinct: 4,
  orderBy: 5,
  limit: 6,
  offset: 6,
} as const;

type Clause = keyof typeof CLAUSE_POS;

/** Serializes everything upstream of `outputId` as one SQL query. */
export function serializeSql(
  index: GraphIndex,
  outputId: string,
  options: SerializeOptions = {},
): SerializeResult {
  const tables = options.tables ?? new Map<string, string>();
  const order = renderOrder(index, outputId, tables);
  if (!Array.isArray(order)) return order;

  // How many rendered nodes read each node: only a node read once can be
  // folded into its reader.
  const readers = new Map<string, number>();
  for (const nodeId of order) {
    for (const inp of resolveInputs(index, nodeId)) {
      readers.set(inp.upstreamId, (readers.get(inp.upstreamId) ?? 0) + 1);
    }
  }

  const states = new Map<string, State>();
  const ctes: Array<{readonly name: string; readonly sql: string}> = [];
  const typeCounters = new Map<string, number>();
  const includes = new Set<string>();

  function stateOf(nodeId: string): State {
    const table = tables.get(nodeId);
    if (table !== undefined) return {kind: 'name', name: table};
    const state = states.get(nodeId);
    if (state === undefined) throw new Error(`No state for ${nodeId}`);
    return state;
  }

  // Makes a node's output readable by name, emitting its block as a CTE if
  // it's more than a bare table reference.
  function nameOf(nodeId: string): string {
    const state = stateOf(nodeId);
    if (state.kind === 'name') return state.name;
    if (isBareRelation(state.block)) return state.block.from;
    const type = index.nodes[nodeId]?.type ?? 'node';
    const n = (typeCounters.get(type) ?? 0) + 1;
    typeCounters.set(type, n);
    const name = `${type}_${n}`;
    ctes.push({name, sql: renderBlock(state.block)});
    states.set(nodeId, {kind: 'name', name});
    return name;
  }

  // The name a node is read by right now, without emitting anything. Only
  // meaningful once `nameOf` has been called for it.
  function refOf(nodeId: string): string {
    const state = stateOf(nodeId);
    if (state.kind === 'name') return state.name;
    return isBareRelation(state.block) ? state.block.from : '<unnamed>';
  }

  for (const nodeId of order) {
    // Checked during the walk.
    const node = ensureDefined(index.nodes[nodeId]);
    const manifest = ensureDefined(tryGetManifest(node.type));
    const inputs = resolveInputs(index, nodeId);
    const primary = inputs[0]?.upstreamId;
    const primaryState = primary !== undefined ? stateOf(primary) : undefined;

    // Secondary inputs are always read by name. The primary is too, unless
    // this node can fold into its open block (nothing else reads it).
    for (const inp of inputs.slice(1)) nameOf(inp.upstreamId);
    const primaryFoldable =
      primary !== undefined &&
      primaryState?.kind === 'block' &&
      readers.get(primary) === 1;
    if (primary !== undefined && !primaryFoldable) nameOf(primary);

    const generate = () =>
      manifest.generators.sql(
        node.config,
        makeContext(
          index,
          inputs,
          (id): SqlInput => ({ref: refOf(id)}),
          options.sqlModules,
        ),
      );
    let frag = generate();
    // A fragment with its own `from` reads the primary by name, so it can't
    // fold: name the primary and generate again with the real name.
    if (
      primaryFoldable &&
      frag !== undefined &&
      !('error' in frag) &&
      (frag.from !== undefined || frag.table !== undefined)
    ) {
      nameOf(primary);
      frag = generate();
    }
    if (frag === undefined) {
      return {ok: false, error: `${manifest.title}: incomplete node`, nodeId};
    }
    if ('error' in frag) return {ok: false, error: frag.error, nodeId};
    for (const inc of frag.includes ?? []) includes.add(inc);

    // Empty fragment: the node passes its primary input through.
    if (isEmpty(frag)) {
      if (primary === undefined) {
        return {ok: false, error: `${manifest.title}: no input`, nodeId};
      }
      states.set(nodeId, stateOf(primary));
      continue;
    }

    const current = primary !== undefined ? stateOf(primary) : undefined;
    if (
      frag.from === undefined &&
      frag.table === undefined &&
      primaryFoldable &&
      current?.kind === 'block' &&
      canFold(current.block, frag)
    ) {
      states.set(nodeId, {kind: 'block', block: foldInto(current.block, frag)});
      continue;
    }

    let source: Block;
    if (frag.table !== undefined) {
      source = {from: frag.table, table: frag.table};
    } else if (frag.from !== undefined) {
      source = {from: frag.from};
    } else {
      if (primary === undefined) {
        return {ok: false, error: `${manifest.title}: no input`, nodeId};
      }
      const name = nameOf(primary);
      source = {from: name, table: name};
    }
    states.set(nodeId, {
      kind: 'block',
      block: foldInto(source, {...frag, from: undefined, table: undefined}),
    });
  }

  const root = stateOf(outputId);
  const relation =
    root.kind === 'name'
      ? root.name
      : isBareRelation(root.block)
        ? root.block.from
        : undefined;
  const body =
    root.kind === 'name'
      ? `SELECT *\nFROM ${root.name}`
      : renderBlock(root.block);
  const sql =
    ctes.length === 0
      ? body
      : `WITH\n${ctes.map((c) => `${c.name} AS (\n${indent(c.sql)}\n)`).join(',\n')}\n${body}`;
  return {
    ok: true,
    sql,
    includes: [...includes],
    relation: ctes.length === 0 ? relation : undefined,
  };
}

function isEmpty(frag: SqlFragment): boolean {
  return Object.entries(frag).every(
    ([key, value]) => key === 'includes' || value === undefined,
  );
}

function clausesOf(x: Block | SqlFragment): Clause[] {
  return (Object.keys(CLAUSE_POS) as Clause[]).filter(
    (c) => x[c] !== undefined,
  );
}

function canFold(block: Block, frag: SqlFragment): boolean {
  const latest = Math.max(...clausesOf(block).map((c) => CLAUSE_POS[c]));
  return clausesOf(frag).every(
    (c) =>
      CLAUSE_POS[c] > latest || (c === 'where' && latest <= CLAUSE_POS.where),
  );
}

function foldInto(block: Block, frag: SqlFragment): Block {
  const where =
    block.where !== undefined && frag.where !== undefined
      ? `(${block.where}) AND (${frag.where})`
      : (block.where ?? frag.where);
  const clauses = Object.fromEntries(
    Object.entries(frag).filter(
      ([key, value]) => key !== 'includes' && value !== undefined,
    ),
  );
  return {...block, ...clauses, where};
}

function isBareRelation(block: Block): boolean {
  return block.table !== undefined && clausesOf(block).length === 1;
}

function renderBlock(block: Block): string {
  const cols = block.select ?? ['*'];
  const head = block.distinct ? 'SELECT DISTINCT' : 'SELECT';
  const lines = [
    cols.length > 1 ? `${head}\n  ${cols.join(',\n  ')}` : `${head} ${cols[0]}`,
    `FROM ${block.from}`,
  ];
  if (block.where !== undefined) lines.push(`WHERE ${block.where}`);
  if (block.groupBy !== undefined && block.groupBy.length > 0) {
    lines.push(`GROUP BY ${block.groupBy.join(', ')}`);
  }
  if (block.orderBy !== undefined && block.orderBy.length > 0) {
    lines.push(`ORDER BY ${block.orderBy.join(', ')}`);
  }
  if (block.limit !== undefined) {
    lines.push(
      block.offset !== undefined && block.offset > 0
        ? `LIMIT ${block.limit} OFFSET ${block.offset}`
        : `LIMIT ${block.limit}`,
    );
  }
  return lines.join('\n');
}

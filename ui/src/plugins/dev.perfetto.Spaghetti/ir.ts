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

import {Connection} from '../../widgets/nodegraph';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {
  collectUpstream,
  findConnectedInputs,
  fnvHash,
  getManifest,
  getOutputColumnsForNode,
  getPrimaryInput,
  isNodeValid,
} from './graph_utils';
import {NodeData, IrContext, SqlStatement} from './node_types';
import {FromConfig} from './nodes/from';

// --- IR (Intermediate Representation) ---
//
// Content-addressable: the hash IS the CTE/table name (_qb_<8hex>).
// SQL in each entry references deps by their hash directly — no rewriting.

export interface IrEntry {
  // Content+dependency hash, also used as CTE/table name: _qb_<8hex>.
  readonly hash: string;
  // The SQL for this statement. References deps by hash and FROM tables by name.
  readonly sql: string;
  // The hashes of IR entries this depends on.
  readonly deps: readonly string[];
  // Module include keys required by this entry (from `from` nodes).
  readonly includes: readonly string[];
  // The node IDs that were folded into this entry.
  readonly nodeIds: readonly string[];
}

// Compute a content-addressable hash for an IR entry.
function irHash(sql: string, depHashes: readonly string[]): string {
  const sorted = [...depHashes].sort();
  return `_qb_${fnvHash(sql + sorted.join(','))}`;
}

// --- SQL Statement folding ---

// Split column list on top-level commas only (not inside parentheses).
function splitTopLevelCommas(s: string): string[] {
  const parts: string[] = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < s.length; i++) {
    if (s[i] === '(') depth++;
    else if (s[i] === ')') depth--;
    else if (s[i] === ',' && depth === 0) {
      parts.push(s.slice(start, i).trim());
      start = i + 1;
    }
  }
  parts.push(s.slice(start).trim());
  return parts;
}

function formatColumns(columns: string): string {
  if (columns === '*') return columns;
  const cols = splitTopLevelCommas(columns);
  if (cols.length <= 1) return columns;
  return '\n  ' + cols.join(',\n  ');
}

function statementToSql(s: SqlStatement): string {
  const selectKw = s.distinct ? 'SELECT DISTINCT' : 'SELECT';
  const parts = [`${selectKw} ${formatColumns(s.columns)}`, `FROM ${s.from}`];
  if (s.where) parts.push(`WHERE ${s.where}`);
  if (s.groupBy) parts.push(`GROUP BY ${s.groupBy}`);
  if (s.orderBy) parts.push(`ORDER BY ${s.orderBy}`);
  if (s.limit !== undefined) parts.push(`LIMIT ${s.limit}`);
  return parts.join('\n');
}

// Try to fold a node into the current SQL statement via its manifest.
function tryFold(stmt: SqlStatement, node: NodeData): boolean {
  const manifest = getManifest(node.type);
  if (!manifest?.tryFold) return false;
  return manifest.tryFold(stmt, node.config);
}

// --- IR Builder ---

// Build an IR for a specific node's upstream chain.
// Returns entries in topological order, or undefined if invalid.
// FROM nodes produce no entries — their real table names appear in SQL directly.
export function buildIR(
  nodes: Map<string, NodeData>,
  connections: Connection[],
  nodeId: string,
  sqlModules: SqlModules | undefined,
): IrEntry[] | undefined {
  const targetNode = nodes.get(nodeId);
  if (!targetNode) return undefined;

  // Collect upstream nodes in topological order.
  const visited = new Set<string>();
  const order: NodeData[] = [];
  collectUpstream(nodes, connections, nodeId, visited, order);

  if (order.length === 0) return undefined;

  // Validate all nodes.
  for (const n of order) {
    if (!isNodeValid(n, nodes, connections)) return undefined;
  }

  // Count references for fold safety.
  const refCount = new Map<string, number>();
  for (const n of order) {
    const primary = getPrimaryInput(nodes, connections, n.id);
    if (primary) {
      refCount.set(primary.id, (refCount.get(primary.id) ?? 0) + 1);
    }
    // Multi-input nodes: count the right (port 1) input too.
    const manifest = getManifest(n.type);
    if ((manifest.inputs?.length ?? 0) >= 2) {
      const ci = findConnectedInputs(nodes, connections, n.id);
      const right = ci.get(1);
      if (right) {
        refCount.set(right.id, (refCount.get(right.id) ?? 0) + 1);
      }
    }
  }

  const entries: IrEntry[] = [];
  const entryByHash = new Set<string>();
  // Maps node ID → how to reference it in SQL (hash for entries, table name for FROM).
  const emittedAs = new Map<string, string>();

  let current: SqlStatement | undefined;
  let currentTailId: string | undefined;
  let currentDeps: string[] = [];
  let currentIncludes = new Set<string>();
  let currentNodeIds: string[] = [];

  // Collect includes from a node's inputs into a set.
  function collectIncludes(
    ...inputNodes: (NodeData | undefined)[]
  ): Set<string> {
    const includes = new Set<string>();
    for (const input of inputNodes) {
      if (!input) continue;
      const parentIncludes = nodeIncludes.get(input.id);
      if (parentIncludes) {
        for (const inc of parentIncludes) includes.add(inc);
      }
    }
    return includes;
  }

  // Maps node ID → set of module include keys required by that node's chain.
  const nodeIncludes = new Map<string, Set<string>>();

  function emitEntry(
    sql: string,
    deps: string[],
    includes?: ReadonlySet<string>,
    nodeIds?: string[],
  ): string {
    const hash = irHash(sql, deps);
    if (!entryByHash.has(hash)) {
      entryByHash.add(hash);
      entries.push({
        hash,
        sql,
        deps: [...deps],
        includes: includes ? [...includes] : [],
        nodeIds: nodeIds ?? [],
      });
    }
    return hash;
  }

  function emitCurrent() {
    if (current && currentTailId) {
      const sql = statementToSql(current);
      const hash = emitEntry(sql, currentDeps, currentIncludes, currentNodeIds);
      emittedAs.set(currentTailId, hash);
    }
    current = undefined;
    currentTailId = undefined;
    currentDeps = [];
    currentIncludes = new Set<string>();
    currentNodeIds = [];
  }

  function resolveRef(node: NodeData): string {
    return emittedAs.get(node.id) ?? '';
  }

  function resolveDep(node: NodeData): string | undefined {
    const ref = emittedAs.get(node.id);
    if (!ref) return undefined;
    // FROM nodes use real table names, not hashes — no dep needed.
    if (node.type === 'from') return undefined;
    return ref;
  }

  for (const n of order) {
    const manifest = getManifest(n.type);
    const primaryInput = getPrimaryInput(nodes, connections, n.id);
    const inputIsCurrentTail =
      primaryInput !== undefined && primaryInput.id === currentTailId;
    const inputSingleRef =
      primaryInput !== undefined && (refCount.get(primaryInput.id) ?? 0) <= 1;
    const canFold =
      inputIsCurrentTail && inputSingleRef && current !== undefined;

    // FROM nodes are special: they register their table name directly
    // (not a CTE hash) and resolve module includes.
    if (n.type === 'from') {
      const cfg = n.config as FromConfig;
      emitCurrent();
      emittedAs.set(n.id, cfg.table);
      if (sqlModules) {
        const mod = sqlModules.getModuleForTable(cfg.table);
        if (mod && !mod.includeKey.startsWith('prelude')) {
          nodeIncludes.set(n.id, new Set([mod.includeKey]));
        }
      }
      continue;
    }

    // Nodes with emitIr produce standalone SQL entries.
    if (manifest.emitIr) {
      emitCurrent();

      // Build a map from port label → connected input node.
      const portInputs = new Map<string, NodeData>();
      const connected = findConnectedInputs(nodes, connections, n.id);
      const ports = manifest.inputs ?? [];
      for (let i = 0; i < ports.length; i++) {
        const input = i === 0 && primaryInput ? primaryInput : connected.get(i);
        if (input) {
          portInputs.set(ports[i].name, input);
        }
      }

      const irCtx: IrContext = {
        getInputRef(portLabel: string): string {
          const input = portInputs.get(portLabel);
          return input ? resolveRef(input) : '';
        },
        getInputColumns(portLabel: string) {
          const input = portInputs.get(portLabel);
          if (!input) return undefined;
          return getOutputColumnsForNode(
            nodes,
            connections,
            input.id,
            sqlModules,
          );
        },
      };
      const result = manifest.emitIr(n.config, irCtx);

      const deps: string[] = [];
      const allInputs = [...portInputs.values()];
      for (const input of allInputs) {
        const d = resolveDep(input);
        if (d) deps.push(d);
      }

      const includes = collectIncludes(...allInputs);
      if (result.includes) {
        for (const inc of result.includes) includes.add(inc);
      }
      const hash = emitEntry(result.sql, deps, includes, [n.id]);
      nodeIncludes.set(n.id, includes);
      emittedAs.set(n.id, hash);
      continue;
    }

    // Inherit includes from input.
    const inherited = collectIncludes(primaryInput);

    // Try to fold into the current statement.
    if (canFold && tryFold(current!, n)) {
      currentTailId = n.id;
      currentNodeIds.push(n.id);
      for (const inc of inherited) currentIncludes.add(inc);
      nodeIncludes.set(n.id, currentIncludes);
      continue;
    }

    // Can't fold — emit current and start a new statement.
    emitCurrent();
    const inputRef = primaryInput ? resolveRef(primaryInput) : '';
    if (!inputRef) return undefined;
    current = {columns: '*', from: inputRef};
    tryFold(current, n);
    currentTailId = n.id;
    currentNodeIds = [n.id];
    currentIncludes = inherited;
    if (primaryInput) {
      const d = resolveDep(primaryInput);
      if (d) currentDeps.push(d);
    }
    nodeIncludes.set(n.id, currentIncludes);
  }

  emitCurrent();

  // If the target is a FROM node, emit SELECT * FROM <table> as its entry.
  if (targetNode.type === 'from') {
    const manifest = getManifest('from');
    const irCtx: IrContext = {
      getInputRef: () => '',
      getInputColumns: () => undefined,
    };
    const result = manifest.emitIr!(targetNode.config, irCtx);
    const includes = nodeIncludes.get(targetNode.id) ?? new Set();
    emitEntry(result.sql, [], includes, [targetNode.id]);
  }

  return entries;
}

// --- Display SQL ---

// Format IR entries as a SQL query with CTEs.
// The last entry is the main query; everything before it is a CTE.
export function buildDisplaySql(
  entries: readonly IrEntry[],
): string | undefined {
  if (entries.length === 0) return undefined;

  // Collect all unique includes across all entries.
  const allIncludes = new Set<string>();
  for (const e of entries) {
    for (const inc of e.includes) allIncludes.add(inc);
  }

  const lastEntry = entries[entries.length - 1];
  const cteEntries = entries.slice(0, -1);

  const parts: string[] = [];

  // Prepend INCLUDE PERFETTO MODULE statements.
  for (const inc of allIncludes) {
    parts.push(`INCLUDE PERFETTO MODULE ${inc};`);
  }

  // Build a mapping from content-addressable hashes to readable names.
  const nameMap = new Map<string, string>();
  for (let i = 0; i < cteEntries.length; i++) {
    nameMap.set(cteEntries[i].hash, `step_${i + 1}`);
  }

  function renameSql(sql: string): string {
    let result = sql;
    for (const [hash, name] of nameMap) {
      result = result.replaceAll(hash, name);
    }
    return result;
  }

  if (cteEntries.length === 0) {
    parts.push(lastEntry.sql);
  } else {
    const cteParts = cteEntries.map((e) => {
      const indented = renameSql(e.sql).split('\n').join('\n  ');
      return `${nameMap.get(e.hash)} AS (\n  ${indented}\n)`;
    });
    parts.push(`WITH\n${cteParts.join(',\n')}\n${renameSql(lastEntry.sql)}`);
  }

  return parts.join('\n');
}

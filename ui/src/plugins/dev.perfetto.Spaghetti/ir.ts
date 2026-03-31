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
import {conditionsToSql} from './filter';
import {getSortConditions, sortConditionsToSql} from './sort';
import {
  collectUpstream,
  findConnectedInputs,
  fnvHash,
  getOutputColumnsForNode,
  getPrimaryInput,
  isNodeValid,
} from './graph_utils';
import {getExtendColumnAliases} from './extend';
import {NodeData} from './node_types';
import {selectionNodeSql} from './selection';

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

interface SqlStatement {
  distinct?: boolean;
  columns: string;
  from: string;
  where?: string;
  groupBy?: string;
  orderBy?: string;
  limit?: number;
}

function formatColumns(columns: string): string {
  if (columns === '*') return columns;
  const cols = columns.split(', ');
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

function tryFold(stmt: SqlStatement, node: NodeData): boolean {
  switch (node.type) {
    case 'select': {
      if (stmt.columns !== '*') return false;
      const selectedCols = Object.entries(node.columns)
        .filter(([_, checked]) => checked)
        .map(([col]) => col);
      const exprParts = node.expressions
        .filter((e) => e.expression && e.alias)
        .map((e) => `${e.expression} AS ${e.alias}`);
      if (selectedCols.length > 0) {
        stmt.columns = [...selectedCols, ...exprParts].join(', ');
      } else if (exprParts.length > 0) {
        stmt.columns = ['*', ...exprParts].join(', ');
      }
      return true;
    }
    case 'filter': {
      if (
        stmt.groupBy !== undefined ||
        stmt.orderBy !== undefined ||
        stmt.limit !== undefined
      ) {
        return false;
      }
      const expr =
        node.conditions.length > 0
          ? conditionsToSql(node.conditions, node.conjunction)
          : node.filterExpression;
      if (expr) {
        stmt.where = stmt.where ? `(${stmt.where}) AND (${expr})` : expr;
      }
      return true;
    }
    case 'sort': {
      if (stmt.orderBy !== undefined || stmt.limit !== undefined) return false;
      const sortConds = getSortConditions(node);
      const orderBy = sortConditionsToSql(sortConds);
      if (orderBy) {
        stmt.orderBy = orderBy;
      }
      return true;
    }
    case 'limit': {
      if (stmt.limit !== undefined) return false;
      stmt.limit = parseInt(node.limitCount) || 100;
      return true;
    }
    case 'groupby': {
      if (
        stmt.columns !== '*' ||
        stmt.groupBy !== undefined ||
        stmt.orderBy !== undefined ||
        stmt.limit !== undefined
      ) {
        return false;
      }
      const groupCols = node.groupColumns.filter((c) => c);
      const aggExprs = node.aggregations
        .filter((a) => a.alias)
        .map((a) => `${a.func}(${a.column}) AS ${a.alias}`);
      const selectParts = [...groupCols, ...aggExprs];
      if (selectParts.length > 0) {
        stmt.columns = selectParts.join(', ');
      }
      if (groupCols.length > 0) {
        stmt.groupBy = groupCols.join(', ');
      }
      return true;
    }
    case 'extract_arg': {
      if (stmt.columns !== '*') return false;
      const exprParts = node.extractions
        .filter((e) => e.column && e.argName && e.alias)
        .map((e) => `extract_arg(${e.column}, '${e.argName}') AS ${e.alias}`);
      if (exprParts.length > 0) {
        stmt.columns = ['*', ...exprParts].join(', ');
      }
      return true;
    }
    default:
      return false;
  }
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
    if (
      n.type === 'extend' ||
      n.type === 'interval_intersect' ||
      n.type === 'union_all'
    ) {
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
    const primaryInput = getPrimaryInput(nodes, connections, n.id);
    const inputIsCurrentTail =
      primaryInput !== undefined && primaryInput.id === currentTailId;
    const inputSingleRef =
      primaryInput !== undefined && (refCount.get(primaryInput.id) ?? 0) <= 1;
    const canFold =
      inputIsCurrentTail && inputSingleRef && current !== undefined;

    if (n.type === 'from') {
      emitCurrent();
      emittedAs.set(n.id, n.table);
      // Look up the module for this table and record the include.
      if (sqlModules) {
        const mod = sqlModules.getModuleForTable(n.table);
        if (mod && !mod.includeKey.startsWith('prelude')) {
          nodeIncludes.set(n.id, new Set([mod.includeKey]));
        }
      }
      continue;
    }

    if (n.type === 'selection') {
      emitCurrent();
      const sql = selectionNodeSql(n);
      const hash = emitEntry(sql, [], new Set(), [n.id]);
      emittedAs.set(n.id, hash);
      continue;
    }

    if (n.type === 'extend') {
      emitCurrent();
      const leftRef = primaryInput ? resolveRef(primaryInput) : '';
      const ci = findConnectedInputs(nodes, connections, n.id);
      const rightInput = ci.get(1);
      const rightRef = rightInput ? resolveRef(rightInput) : '';

      const leftAvail = primaryInput
        ? getOutputColumnsForNode(
            nodes,
            connections,
            primaryInput.id,
            sqlModules,
          )
        : undefined;

      // All left columns pass through.
      const selectCols: string[] = ['l.*'];

      // Add selected right columns with aliases.
      const aliases = getExtendColumnAliases(
        n,
        (leftAvail ?? []).map((c) => c.name),
      );
      for (const a of aliases) {
        const expr = `r.${a.column}`;
        selectCols.push(a.alias !== a.column ? `${expr} AS ${a.alias}` : expr);
      }

      let selectClause: string;
      if (selectCols.length > 1) {
        selectClause = '\n  ' + selectCols.join(',\n  ');
      } else {
        selectClause = selectCols[0];
      }

      const condition = `ON l.${n.leftColumn} = r.${n.rightColumn}`;
      const sql = `SELECT ${selectClause}\nFROM ${leftRef} AS l\nLEFT JOIN ${rightRef} AS r ${condition}`;

      const deps: string[] = [];
      if (primaryInput) {
        const d = resolveDep(primaryInput);
        if (d) deps.push(d);
      }
      if (rightInput) {
        const d = resolveDep(rightInput);
        if (d) deps.push(d);
      }

      const includes = collectIncludes(primaryInput, rightInput);
      const hash = emitEntry(sql, deps, includes, [n.id]);
      nodeIncludes.set(n.id, includes);
      emittedAs.set(n.id, hash);
      continue;
    }

    if (n.type === 'interval_intersect') {
      emitCurrent();
      const leftRef = primaryInput ? resolveRef(primaryInput) : '';
      const ci = findConnectedInputs(nodes, connections, n.id);
      const rightInput = ci.get(1);
      const rightRef = rightInput ? resolveRef(rightInput) : '';

      const deps: string[] = [];
      if (primaryInput) {
        const d = resolveDep(primaryInput);
        if (d) deps.push(d);
      }
      if (rightInput) {
        const d = resolveDep(rightInput);
        if (d) deps.push(d);
      }

      // Wrap refs in subqueries to filter negative durations if needed.
      const leftArg = n.filterNegativeDur
        ? `(SELECT * FROM ${leftRef} WHERE dur >= 0)`
        : leftRef;
      const rightArg = n.filterNegativeDur
        ? `(SELECT * FROM ${rightRef} WHERE dur >= 0)`
        : rightRef;

      const partitionCols = n.partitionColumns.filter((c) => c);
      const partitionClause =
        partitionCols.length > 0 ? partitionCols.join(', ') : '';

      const sql = `SELECT *\nFROM _interval_intersect!(\n  (${leftArg},\n   ${rightArg}),\n  (${partitionClause})\n)`;
      const includes = collectIncludes(primaryInput, rightInput);
      includes.add('intervals.intersect');
      const hash = emitEntry(sql, deps, includes, [n.id]);
      nodeIncludes.set(n.id, includes);
      emittedAs.set(n.id, hash);
      continue;
    }

    if (n.type === 'union_all') {
      emitCurrent();
      const leftRef = primaryInput ? resolveRef(primaryInput) : '';
      const ci = findConnectedInputs(nodes, connections, n.id);
      const rightInput = ci.get(1);
      const rightRef = rightInput ? resolveRef(rightInput) : '';

      const deps: string[] = [];
      if (primaryInput) {
        const d = resolveDep(primaryInput);
        if (d) deps.push(d);
      }
      if (rightInput) {
        const d = resolveDep(rightInput);
        if (d) deps.push(d);
      }

      const unionKw = n.distinct ? 'UNION' : 'UNION ALL';
      const sql = `SELECT *\nFROM ${leftRef}\n${unionKw}\nSELECT *\nFROM ${rightRef}`;
      const includes = collectIncludes(primaryInput, rightInput);
      const hash = emitEntry(sql, deps, includes, [n.id]);
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
    const sql = `SELECT *\nFROM ${targetNode.table}`;
    const includes = nodeIncludes.get(targetNode.id) ?? new Set();
    emitEntry(sql, [], includes, [targetNode.id]);
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

  if (cteEntries.length === 0) {
    parts.push(lastEntry.sql);
  } else {
    const cteParts = cteEntries.map((e) => {
      const indented = e.sql.split('\n').join('\n  ');
      return `${e.hash} AS (\n  ${indented}\n)`;
    });
    parts.push(`WITH\n${cteParts.join(',\n')}\n${lastEntry.sql}`);
  }

  return parts.join('\n');
}

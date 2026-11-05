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

import {assertUnreachable} from '../../base/assert';
import {fnvHash} from './graph_utils';
import {
  type AggregateOp,
  type DropOp,
  type ExtendOp,
  type ExtractArgOp,
  type FilterLogicalOp,
  formatExtractArgExpr,
  formatFilterConditions,
  type IntervalIntersectOp,
  type JoinOp,
  type LimitOp,
  type LogicalNode,
  type LogicalPlan,
  type PassthroughOp,
  type ProjectOp,
  type RawSqlOp,
  type SortOp,
  type TimeRangeOp,
  type UnionOp,
} from './logical_ir';

// Content-addressable physical IR entry: the hash IS the CTE/table name
// (`_qb_<8hex>`). SQL in each entry references deps by their hash directly.
export interface IrEntry {
  // Content+dependency hash, also used as CTE/table name: _qb_<8hex>.
  readonly hash: string;
  // The query for this statement. References deps by hash and FROM tables by name.
  readonly sql: string;
  // The hashes of IR entries this depends on.
  readonly deps: readonly string[];
  // Module include keys required by this entry.
  readonly includes: readonly string[];
  // The node IDs that were folded into this entry.
  readonly nodeIds: readonly string[];
}

export function computeIrHash(
  sql: string,
  depHashes: readonly string[],
): string {
  const sorted = [...depHashes].sort();
  return `_qb_${fnvHash(sql + sorted.join(','))}`;
}

// --- Standard SQL statement folding ---

interface SqlStatement {
  distinct?: boolean;
  columns: string;
  from: string;
  where?: string;
  groupBy?: string;
  orderBy?: string;
  limit?: number;
}

type FoldableOp =
  | PassthroughOp
  | FilterLogicalOp
  | ProjectOp
  | ExtendOp
  | ExtractArgOp
  | AggregateOp
  | SortOp
  | LimitOp;

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
  if (s.where !== undefined && s.where !== '') parts.push(`WHERE ${s.where}`);
  if (s.groupBy !== undefined && s.groupBy !== '') {
    parts.push(`GROUP BY ${s.groupBy}`);
  }
  if (s.orderBy !== undefined && s.orderBy !== '') {
    parts.push(`ORDER BY ${s.orderBy}`);
  }
  if (s.limit !== undefined) parts.push(`LIMIT ${s.limit}`);
  return parts.join('\n');
}

function tryFoldOp(stmt: SqlStatement, op: FoldableOp): boolean {
  switch (op.kind) {
    case 'passthrough':
      return true;

    case 'filter': {
      if (
        stmt.groupBy !== undefined ||
        stmt.orderBy !== undefined ||
        stmt.limit !== undefined
      ) {
        return false;
      }
      const expr = formatFilterConditions(op.conditions, op.conjunction);
      if (expr !== '') {
        stmt.where =
          stmt.where !== undefined && stmt.where !== ''
            ? `(${stmt.where}) AND (${expr})`
            : expr;
      }
      return true;
    }

    case 'project': {
      if (stmt.columns !== '*') return false;
      const entryParts = op.columns.map((c) =>
        c.alias !== undefined && c.alias !== ''
          ? `${c.column} AS ${c.alias}`
          : c.column,
      );
      const exprParts = op.expressions.map(
        (e) => `${e.expression} AS ${e.alias}`,
      );
      if (entryParts.length === 0) {
        if (exprParts.length > 0) {
          stmt.columns = `*, ${exprParts.join(', ')}`;
        }
        return true;
      }
      stmt.columns = [...entryParts, ...exprParts].join(', ');
      return true;
    }

    case 'extend': {
      if (stmt.groupBy !== undefined) return false;
      const exprParts = op.expressions.map(
        (e) => `${e.expression} AS ${e.alias}`,
      );
      if (exprParts.length > 0) {
        stmt.columns = [stmt.columns, ...exprParts].join(', ');
      }
      return true;
    }

    case 'extract_arg': {
      if (stmt.groupBy !== undefined) return false;
      const exprParts = op.extractions.map(
        (e) =>
          `${formatExtractArgExpr(op.argSetIdCol, e.argName)} AS ${e.alias}`,
      );
      if (exprParts.length > 0) {
        stmt.columns = [stmt.columns, ...exprParts].join(', ');
      }
      return true;
    }

    case 'aggregate': {
      if (
        stmt.columns !== '*' ||
        stmt.groupBy !== undefined ||
        stmt.orderBy !== undefined ||
        stmt.limit !== undefined
      ) {
        return false;
      }
      const aggExprs = op.aggregations.map(
        (a) => `${a.func}(${a.column}) AS ${a.alias}`,
      );
      const selectParts = [...op.groupColumns, ...aggExprs];
      if (selectParts.length > 0) stmt.columns = selectParts.join(', ');
      if (op.groupColumns.length > 0) {
        stmt.groupBy = op.groupColumns.join(', ');
      }
      return true;
    }

    case 'sort': {
      if (stmt.orderBy !== undefined || stmt.limit !== undefined) return false;
      const orderBy = op.keys.map((k) => `${k.column} ${k.order}`).join(', ');
      if (orderBy !== '') {
        stmt.orderBy = orderBy;
      }
      return true;
    }

    case 'limit': {
      if (stmt.limit !== undefined) return false;
      stmt.limit = op.count;
      return true;
    }

    default:
      return assertUnreachable(op);
  }
}

// --- Standalone SQL emitters ---

function emitTimeRangeSql(op: TimeRangeOp): string {
  return `SELECT 0 AS id, ${op.ts} AS ts, ${op.dur} AS dur`;
}

function emitDropSql(op: DropOp, ref: string): string {
  const kept = op.remainingColumns ?? [];
  if (op.dropColumns.length === 0 || kept.length === 0) {
    return `SELECT *\nFROM ${ref}`;
  }
  const selectClause = kept.length > 1 ? '\n  ' + kept.join(',\n  ') : kept[0];
  return `SELECT ${selectClause}\nFROM ${ref}`;
}

function emitJoinSql(op: JoinOp, leftRef: string, rightRef: string): string {
  const selectCols: [string, ...string[]] = ['l.*'];
  for (const a of op.rightColumns) {
    const expr = `r.${a.column}`;
    selectCols.push(a.alias !== a.column ? `${expr} AS ${a.alias}` : expr);
  }
  const selectClause =
    selectCols.length > 1 ? '\n  ' + selectCols.join(',\n  ') : selectCols[0];
  const joinKw = op.joinType === 'INNER' ? 'JOIN' : 'LEFT JOIN';
  const condition = `ON l.${op.leftColumn} = r.${op.rightColumn}`;
  return `SELECT ${selectClause}\nFROM ${leftRef} AS l\n${joinKw} ${rightRef} AS r ${condition}`;
}

function emitUnionSql(op: UnionOp, refs: readonly string[]): string {
  const kw = op.distinct ? 'UNION' : 'UNION ALL';
  return refs.map((r) => `SELECT *\nFROM ${r}`).join(`\n${kw}\n`);
}

function emitIntervalIntersectSql(
  op: IntervalIntersectOp,
  refs: readonly string[],
): string {
  const partitionClause =
    op.partitionColumns.length > 0 ? op.partitionColumns.join(', ') : '';
  const args = refs.map((ref) =>
    op.filterNegativeDur ? `(SELECT * FROM ${ref} WHERE dur >= 0)` : ref,
  );
  const n = op.inputs.length;
  const selectParts = ['ii.ts', 'ii.dur'];
  for (let i = 0; i < n; i++) {
    selectParts.push(`ii.id_${i}`);
  }
  for (const [i, inp] of op.inputs.entries()) {
    for (const col of inp.columns) {
      const expr = `t${i + 1}.${col.name}`;
      selectParts.push(
        col.alias !== col.name ? `${expr} AS ${col.alias}` : expr,
      );
    }
  }

  const hasSourceCols = op.inputs.some((inp) => inp.columns.length > 0);
  const argsStr = args.join(',\n   ');

  if (hasSourceCols) {
    const joins = refs
      .map((ref, i) => `JOIN ${ref} t${i + 1} ON ii.id_${i} = t${i + 1}.id`)
      .join('\n');
    return (
      `SELECT\n${selectParts.map((s) => `  ${s}`).join(',\n')}` +
      `\nFROM _interval_intersect!(\n  (${argsStr}),\n  (${partitionClause})\n) ii` +
      `\n${joins}`
    );
  }
  return `SELECT *\nFROM _interval_intersect!(\n  (${argsStr}),\n  (${partitionClause})\n)`;
}

function emitRawSql(
  op: RawSqlOp,
  resolveRef: (nodeId: string) => string,
): string {
  const withClauses = op.inputs.map(
    (inp) => `${inp.alias} AS (\n  SELECT * FROM ${resolveRef(inp.nodeId)}\n)`,
  );
  if (withClauses.length === 0) {
    return op.sql;
  }
  const trimmed = op.sql.trimStart();
  const withMatch = trimmed.match(/^with\s/i);
  const body = withMatch ? trimmed.slice(withMatch[0].length) : trimmed;
  const separator = withMatch ? ',\n' : '\n';
  return `WITH ${withClauses.join(',\n')}${separator}${body}`;
}

// --- Lower LogicalPlan -> Standard SQL IrEntry[] ---

export function lowerToSql(plan: LogicalPlan): IrEntry[] {
  const nodeById = new Map<string, LogicalNode>();
  for (const node of plan.nodes) {
    nodeById.set(node.nodeId, node);
  }

  const entries: IrEntry[] = [];
  const entryByHash = new Set<string>();
  const emittedAs = new Map<string, string>();
  const isScanNode = new Set<string>();
  const nodeIncludes = new Map<string, Set<string>>();

  let current: SqlStatement | undefined;
  let currentTailId: string | undefined;
  let currentDeps: string[] = [];
  let currentIncludes = new Set<string>();
  let currentNodeIds: string[] = [];

  function collectIncludes(inputIds: readonly string[]): Set<string> {
    const includes = new Set<string>();
    for (const id of inputIds) {
      const parentIncludes = nodeIncludes.get(id);
      if (parentIncludes) {
        for (const inc of parentIncludes) includes.add(inc);
      }
    }
    return includes;
  }

  function emitEntry(
    sql: string,
    deps: readonly string[],
    includes: ReadonlySet<string>,
    nodeIds: readonly string[],
  ): string {
    const hash = computeIrHash(sql, deps);
    if (!entryByHash.has(hash)) {
      entryByHash.add(hash);
      entries.push({
        hash,
        sql,
        deps: [...deps],
        includes: [...includes],
        nodeIds: [...nodeIds],
      });
    }
    return hash;
  }

  function emitCurrent(): void {
    if (current !== undefined && currentTailId !== undefined) {
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

  function resolveRef(id: string): string {
    return emittedAs.get(id) ?? '';
  }

  function resolveDep(id: string): string | undefined {
    if (isScanNode.has(id)) return undefined;
    return emittedAs.get(id);
  }

  function resolveDeps(inputIds: readonly string[]): string[] {
    const seen = new Set<string>();
    const deps: string[] = [];
    for (const id of inputIds) {
      const dep = resolveDep(id);
      if (dep !== undefined && !seen.has(dep)) {
        seen.add(dep);
        deps.push(dep);
      }
    }
    return deps;
  }

  function emitStandalone(
    nodeId: string,
    inputIds: readonly string[],
    buildSql: () => string,
    extraIncludes?: readonly string[],
  ): boolean {
    emitCurrent();
    if (inputIds.some((id) => resolveRef(id) === '')) return false;
    const sql = buildSql();
    const deps = resolveDeps(inputIds);
    const includes = collectIncludes(inputIds);
    if (extraIncludes) {
      for (const inc of extraIncludes) includes.add(inc);
    }
    const hash = emitEntry(sql, deps, includes, [nodeId]);
    nodeIncludes.set(nodeId, includes);
    emittedAs.set(nodeId, hash);
    return true;
  }

  function emitFoldable(nodeId: string, op: FoldableOp): boolean {
    const inputIsCurrentTail = op.input === currentTailId;
    const inputSingleRef = (nodeById.get(op.input)?.refCount ?? 0) <= 1;
    const inherited = collectIncludes([op.input]);

    if (
      inputIsCurrentTail &&
      inputSingleRef &&
      current !== undefined &&
      tryFoldOp(current, op)
    ) {
      currentTailId = nodeId;
      currentNodeIds.push(nodeId);
      for (const inc of inherited) currentIncludes.add(inc);
      nodeIncludes.set(nodeId, currentIncludes);
      return true;
    }

    emitCurrent();
    const inputRef = resolveRef(op.input);
    if (inputRef === '') return false;
    current = {columns: '*', from: inputRef};
    tryFoldOp(current, op);
    currentTailId = nodeId;
    currentNodeIds = [nodeId];
    currentIncludes = inherited;
    const dep = resolveDep(op.input);
    if (dep !== undefined) currentDeps.push(dep);
    nodeIncludes.set(nodeId, currentIncludes);
    return true;
  }

  for (const node of plan.nodes) {
    const {nodeId, op} = node;
    switch (op.kind) {
      case 'scan': {
        emitCurrent();
        emittedAs.set(nodeId, op.table);
        isScanNode.add(nodeId);
        if (op.includes && op.includes.length > 0) {
          nodeIncludes.set(nodeId, new Set(op.includes));
        }
        break;
      }

      case 'time_range': {
        emitStandalone(nodeId, [], () => emitTimeRangeSql(op));
        break;
      }

      case 'drop': {
        if (
          !emitStandalone(nodeId, [op.input], () =>
            emitDropSql(op, resolveRef(op.input)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'join': {
        if (
          !emitStandalone(nodeId, [op.left, op.right], () =>
            emitJoinSql(op, resolveRef(op.left), resolveRef(op.right)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'union': {
        if (op.inputs.length === 0) return [];
        if (
          !emitStandalone(nodeId, op.inputs, () =>
            emitUnionSql(op, op.inputs.map(resolveRef)),
          )
        ) {
          return [];
        }
        break;
      }

      case 'interval_intersect': {
        const inputIds = op.inputs.map((inp) => inp.nodeId);
        if (inputIds.length === 0) return [];
        if (
          !emitStandalone(
            nodeId,
            inputIds,
            () => emitIntervalIntersectSql(op, inputIds.map(resolveRef)),
            ['intervals.intersect'],
          )
        ) {
          return [];
        }
        break;
      }

      case 'raw_sql': {
        const inputIds = op.inputs.map((inp) => inp.nodeId);
        if (
          !emitStandalone(nodeId, inputIds, () => emitRawSql(op, resolveRef))
        ) {
          return [];
        }
        break;
      }

      case 'passthrough':
      case 'filter':
      case 'project':
      case 'extend':
      case 'extract_arg':
      case 'aggregate':
      case 'sort':
      case 'limit': {
        if (!emitFoldable(nodeId, op)) return [];
        break;
      }

      default:
        assertUnreachable(op);
    }
  }

  emitCurrent();

  // If the target node itself is a scan (`FROM`), emit `SELECT * FROM <table>`.
  const targetNode = nodeById.get(plan.targetNodeId);
  if (targetNode?.op.kind === 'scan') {
    const includes = nodeIncludes.get(targetNode.nodeId) ?? new Set<string>();
    emitEntry(`SELECT *\nFROM ${targetNode.op.table}`, [], includes, [
      targetNode.nodeId,
    ]);
  }

  return entries;
}

// --- Display SQL ---

// Format Standard SQL IR entries as a single query using `WITH` CTEs.
export function buildDisplaySql(
  entries: readonly IrEntry[],
): string | undefined {
  if (entries.length === 0) return undefined;

  const allIncludes = new Set<string>();
  for (const e of entries) {
    for (const inc of e.includes) allIncludes.add(inc);
  }

  const lastEntry = entries[entries.length - 1];
  if (!lastEntry) return undefined;
  const cteEntries = entries.slice(0, -1);

  const parts: string[] = [];

  for (const inc of allIncludes) {
    parts.push(`INCLUDE PERFETTO MODULE ${inc};`);
  }

  const nameMap = new Map<string, string>();
  for (const [i, cteEntry] of cteEntries.entries()) {
    nameMap.set(cteEntry.hash, `step_${i + 1}`);
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

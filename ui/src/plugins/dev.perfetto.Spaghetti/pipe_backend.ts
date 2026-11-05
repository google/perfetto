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
import {computeIrHash, type IrEntry} from './sql_backend';

// ---------------------------------------------------------------------------
// PerfettoSQL Next (Pipe Syntax) Backend
//
// Lowers a language-agnostic `LogicalPlan` into PerfettoSQL Next pipelines
// (`FROM ... |> ...` and `INTERVAL INTERSECTION OF (...) |> ...`) matching
// `src/perfetto_sql/syntaqlite/perfetto.synq` and RFC-0041.
// ---------------------------------------------------------------------------

interface PipeStatement {
  source: string;
  sourceAlias?: string;
  // When true, `source` is already a full source clause such as
  // `INTERVAL INTERSECTION OF (...)` rather than a `FROM <source>` relation.
  rawSourceClause?: boolean;
  stages: string[];
}

type PipeStageOp =
  | PassthroughOp
  | FilterLogicalOp
  | ProjectOp
  | ExtendOp
  | ExtractArgOp
  | DropOp
  | AggregateOp
  | SortOp
  | LimitOp;

function computePipeIrHash(sql: string, deps: readonly string[]): string {
  return computeIrHash(`pipe:${sql}`, deps);
}

function pipeStatementToSql(p: PipeStatement): string {
  let head: string;
  if (p.rawSourceClause) {
    head = p.source;
  } else if (p.sourceAlias !== undefined && p.sourceAlias !== '') {
    head = `FROM ${p.source} AS ${p.sourceAlias}`;
  } else {
    head = `FROM ${p.source}`;
  }
  if (p.stages.length === 0) {
    return head;
  }
  return [head, ...p.stages.map((s) => `|> ${s}`)].join('\n');
}

function appendStageOp(stmt: PipeStatement, op: PipeStageOp): void {
  switch (op.kind) {
    case 'passthrough':
      return;

    case 'filter': {
      const expr = formatFilterConditions(op.conditions, op.conjunction);
      if (expr !== '') {
        stmt.stages.push(`WHERE ${expr}`);
      }
      return;
    }

    case 'project': {
      const colParts = op.columns.map((c) =>
        c.alias !== undefined && c.alias !== ''
          ? `${c.column} AS ${c.alias}`
          : c.column,
      );
      const exprParts = op.expressions.map(
        (e) => `${e.expression} AS ${e.alias}`,
      );
      if (colParts.length === 0) {
        if (exprParts.length > 0) {
          stmt.stages.push(`EXTEND ${exprParts.join(', ')}`);
        }
        return;
      }
      stmt.stages.push(`SELECT ${[...colParts, ...exprParts].join(', ')}`);
      return;
    }

    case 'extend': {
      if (op.expressions.length === 0) return;
      const exprParts = op.expressions.map(
        (e) => `${e.expression} AS ${e.alias}`,
      );
      stmt.stages.push(`EXTEND ${exprParts.join(', ')}`);
      return;
    }

    case 'extract_arg': {
      if (op.extractions.length === 0) return;
      const exprParts = op.extractions.map(
        (e) =>
          `${formatExtractArgExpr(op.argSetIdCol, e.argName)} AS ${e.alias}`,
      );
      stmt.stages.push(`EXTEND ${exprParts.join(', ')}`);
      return;
    }

    case 'drop': {
      if (op.dropColumns.length === 0) return;
      stmt.stages.push(`DROP ${op.dropColumns.join(', ')}`);
      return;
    }

    case 'aggregate': {
      if (op.aggregations.length === 0) {
        if (op.groupColumns.length > 0) {
          stmt.stages.push(`SELECT ${op.groupColumns.join(', ')}`);
          stmt.stages.push('DISTINCT');
        }
        return;
      }
      const aggStr = op.aggregations
        .map((a) => `${a.func}(${a.column}) AS ${a.alias}`)
        .join(', ');
      const groupSuffix =
        op.groupColumns.length > 0
          ? ` GROUP BY ${op.groupColumns.join(', ')}`
          : '';
      stmt.stages.push(`AGGREGATE ${aggStr}${groupSuffix}`);
      return;
    }

    case 'sort': {
      if (op.keys.length === 0) return;
      const terms = op.keys.map((k) => `${k.column} ${k.order}`).join(', ');
      stmt.stages.push(`ORDER BY ${terms}`);
      return;
    }

    case 'limit': {
      stmt.stages.push(`LIMIT ${op.count}`);
      return;
    }

    default:
      assertUnreachable(op);
  }
}

function appendJoinStages(
  stmt: PipeStatement,
  op: JoinOp,
  rightRef: string,
): void {
  if (
    stmt.stages.length === 0 &&
    !stmt.rawSourceClause &&
    stmt.sourceAlias === undefined
  ) {
    stmt.sourceAlias = 'l';
  } else {
    stmt.stages.push('AS l');
  }
  const joinKw = op.joinType === 'INNER' ? 'JOIN' : 'LEFT JOIN';
  stmt.stages.push(
    `${joinKw} ${rightRef} AS r ON l.${op.leftColumn} = r.${op.rightColumn}`,
  );
  const selectCols: [string, ...string[]] = ['l.*'];
  for (const a of op.rightColumns) {
    const expr = `r.${a.column}`;
    selectCols.push(a.alias !== a.column ? `${expr} AS ${a.alias}` : expr);
  }
  stmt.stages.push(`SELECT ${selectCols.join(', ')}`);
}

function makeTimeRangePipe(op: TimeRangeOp): PipeStatement {
  return {
    source: `(SELECT 0 AS id, ${op.ts} AS ts, ${op.dur} AS dur)`,
    stages: [],
  };
}

function makeUnionPipe(op: UnionOp, refs: readonly string[]): PipeStatement {
  const first = refs[0];
  if (refs.length === 1 && first !== undefined) {
    return {source: first, stages: []};
  }
  const kw = op.distinct ? 'UNION' : 'UNION ALL';
  const body = refs.map((r) => `SELECT * FROM ${r}`).join(`\n  ${kw}\n  `);
  return {
    source: `(\n  ${body}\n)`,
    stages: [],
  };
}

function makeIntervalIntersectPipe(
  op: IntervalIntersectOp,
  refs: readonly string[],
): PipeStatement {
  const firstRef = refs[0];
  if (refs.length === 1 && firstRef !== undefined) {
    const stages: string[] = [];
    if (op.filterNegativeDur) {
      stages.push('WHERE dur >= 0');
    }
    return {source: firstRef, stages};
  }

  const operands = refs.map((ref, i) =>
    op.filterNegativeDur
      ? `(FROM ${ref} |> WHERE dur >= 0) AS t${i + 1}`
      : `${ref} AS t${i + 1}`,
  );
  const perSuffix =
    op.partitionColumns.length > 0
      ? ` PER ${op.partitionColumns.join(', ')}`
      : '';
  const source = `INTERVAL INTERSECTION OF (\n  ${operands.join(',\n  ')}\n)${perSuffix}`;

  // Project unique output column names (`ts`, `dur`, `id_0..id_{n-1}`, plus
  // collision-resolved source columns) so `CREATE PERFETTO TABLE` never sees
  // duplicate `id`/`ts`/`dur` columns across operands.
  const selectParts: string[] = ['ts', 'dur'];
  for (let i = 0; i < op.inputs.length; i++) {
    selectParts.push(`t${i + 1}.id AS id_${i}`);
  }
  for (const [i, inp] of op.inputs.entries()) {
    for (const col of inp.columns) {
      const expr = `t${i + 1}.${col.name}`;
      selectParts.push(
        col.alias !== col.name ? `${expr} AS ${col.alias}` : expr,
      );
    }
  }

  return {
    source,
    rawSourceClause: true,
    stages: [`SELECT ${selectParts.join(', ')}`],
  };
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

// --- Lower LogicalPlan -> PerfettoSQL Next IrEntry[] ---

export function lowerToPipe(plan: LogicalPlan): IrEntry[] {
  const nodeById = new Map<string, LogicalNode>();
  for (const node of plan.nodes) {
    nodeById.set(node.nodeId, node);
  }

  const entries: IrEntry[] = [];
  const entryByHash = new Set<string>();
  const emittedAs = new Map<string, string>();
  const isScanNode = new Set<string>();
  const nodeIncludes = new Map<string, Set<string>>();

  let current: PipeStatement | undefined;
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

  function addDep(dep: string | undefined): void {
    if (dep !== undefined && !currentDeps.includes(dep)) {
      currentDeps.push(dep);
    }
  }

  function emitEntry(
    sql: string,
    deps: readonly string[],
    includes: ReadonlySet<string>,
    nodeIds: readonly string[],
  ): string {
    const hash = computePipeIrHash(sql, deps);
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
      const sql = pipeStatementToSql(current);
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

  function startPipeline(
    nodeId: string,
    stmt: PipeStatement,
    inputIds: readonly string[],
  ): void {
    emitCurrent();
    current = stmt;
    currentTailId = nodeId;
    currentNodeIds = [nodeId];
    currentDeps = resolveDeps(inputIds);
    currentIncludes = collectIncludes(inputIds);
    nodeIncludes.set(nodeId, currentIncludes);
  }

  function emitStage(nodeId: string, op: PipeStageOp): boolean {
    const inputIsCurrentTail = op.input === currentTailId;
    const inputSingleRef = (nodeById.get(op.input)?.refCount ?? 0) <= 1;
    const inherited = collectIncludes([op.input]);

    if (inputIsCurrentTail && inputSingleRef && current !== undefined) {
      appendStageOp(current, op);
      currentTailId = nodeId;
      currentNodeIds.push(nodeId);
      for (const inc of inherited) currentIncludes.add(inc);
      nodeIncludes.set(nodeId, currentIncludes);
      return true;
    }

    emitCurrent();
    const inputRef = resolveRef(op.input);
    if (inputRef === '') return false;
    current = {source: inputRef, stages: []};
    appendStageOp(current, op);
    currentTailId = nodeId;
    currentNodeIds = [nodeId];
    currentIncludes = inherited;
    addDep(resolveDep(op.input));
    nodeIncludes.set(nodeId, currentIncludes);
    return true;
  }

  for (const node of plan.nodes) {
    const {nodeId, op} = node;
    switch (op.kind) {
      case 'scan': {
        // Scans bind table names directly without disturbing an unrelated
        // active pipeline branch on a multi-input graph.
        emittedAs.set(nodeId, op.table);
        isScanNode.add(nodeId);
        if (op.includes && op.includes.length > 0) {
          nodeIncludes.set(nodeId, new Set(op.includes));
        }
        break;
      }

      case 'time_range': {
        startPipeline(nodeId, makeTimeRangePipe(op), []);
        break;
      }

      case 'join': {
        const leftIsCurrentTail =
          op.left === currentTailId && op.right !== currentTailId;
        const leftSingleRef = (nodeById.get(op.left)?.refCount ?? 0) <= 1;
        const rightRefIfFolded = resolveRef(op.right);

        if (
          leftIsCurrentTail &&
          leftSingleRef &&
          current !== undefined &&
          rightRefIfFolded !== ''
        ) {
          appendJoinStages(current, op, rightRefIfFolded);
          currentTailId = nodeId;
          currentNodeIds.push(nodeId);
          addDep(resolveDep(op.right));
          for (const inc of collectIncludes([op.left, op.right])) {
            currentIncludes.add(inc);
          }
          nodeIncludes.set(nodeId, currentIncludes);
          break;
        }

        emitCurrent();
        const leftRef = resolveRef(op.left);
        const rightRef = resolveRef(op.right);
        if (leftRef === '' || rightRef === '') return [];
        const stmt: PipeStatement = {source: leftRef, stages: []};
        appendJoinStages(stmt, op, rightRef);
        startPipeline(nodeId, stmt, [op.left, op.right]);
        break;
      }

      case 'union': {
        emitCurrent();
        const refs = op.inputs.map(resolveRef);
        if (refs.length === 0 || refs.some((r) => r === '')) return [];
        startPipeline(nodeId, makeUnionPipe(op, refs), op.inputs);
        break;
      }

      case 'interval_intersect': {
        emitCurrent();
        const inputIds = op.inputs.map((inp) => inp.nodeId);
        const refs = inputIds.map(resolveRef);
        if (refs.length === 0 || refs.some((r) => r === '')) return [];
        startPipeline(nodeId, makeIntervalIntersectPipe(op, refs), inputIds);
        break;
      }

      case 'raw_sql': {
        emitCurrent();
        const inputIds = op.inputs.map((inp) => inp.nodeId);
        if (inputIds.some((id) => resolveRef(id) === '')) return [];
        const sql = emitRawSql(op, resolveRef);
        const deps = resolveDeps(inputIds);
        const includes = collectIncludes(inputIds);
        const hash = emitEntry(sql, deps, includes, [nodeId]);
        nodeIncludes.set(nodeId, includes);
        emittedAs.set(nodeId, hash);
        break;
      }

      case 'passthrough':
      case 'filter':
      case 'project':
      case 'extend':
      case 'extract_arg':
      case 'drop':
      case 'aggregate':
      case 'sort':
      case 'limit': {
        if (!emitStage(nodeId, op)) return [];
        break;
      }

      default:
        assertUnreachable(op);
    }
  }

  emitCurrent();

  // If the target node itself is a scan (`FROM`), emit `FROM <table>`.
  const targetNode = nodeById.get(plan.targetNodeId);
  if (targetNode?.op.kind === 'scan') {
    const includes = nodeIncludes.get(targetNode.nodeId) ?? new Set<string>();
    emitEntry(`FROM ${targetNode.op.table}`, [], includes, [targetNode.nodeId]);
  }

  return entries;
}

// --- Display PerfettoSQL Next ---

// Format PerfettoSQL Next IR entries as executable PerfettoSQL Next.
// Single-entry pipelines emit directly (`FROM ... |> ...`); multi-entry DAGs
// materialize shared upstream steps via `CREATE OR REPLACE PERFETTO TABLE`
// statements (since PerfettoSQL Next pipelines are top-level statements).
export function buildDisplayPipeSql(
  entries: readonly IrEntry[],
): string | undefined {
  if (entries.length === 0) return undefined;

  const allIncludes = new Set<string>();
  for (const e of entries) {
    for (const inc of e.includes) allIncludes.add(inc);
  }

  const lastEntry = entries[entries.length - 1];
  if (!lastEntry) return undefined;
  const stepEntries = entries.slice(0, -1);

  const nameMap = new Map<string, string>();
  for (const [i, stepEntry] of stepEntries.entries()) {
    nameMap.set(stepEntry.hash, `step_${i + 1}`);
  }

  function renameSql(sql: string): string {
    let result = sql;
    for (const [hash, name] of nameMap) {
      result = result.replaceAll(hash, name);
    }
    return result;
  }

  const blocks: string[] = [];

  if (allIncludes.size > 0) {
    blocks.push(
      Array.from(allIncludes, (inc) => `INCLUDE PERFETTO MODULE ${inc};`).join(
        '\n',
      ),
    );
  }

  for (const step of stepEntries) {
    const stepName = nameMap.get(step.hash);
    blocks.push(
      `CREATE OR REPLACE PERFETTO TABLE ${stepName} AS\n${renameSql(step.sql)};`,
    );
  }

  blocks.push(renameSql(lastEntry.sql));

  return blocks.join('\n\n');
}

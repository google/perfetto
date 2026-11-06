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
  type AsOp,
  type DataTableOp,
  type DistinctOp,
  type DropOp,
  type ExtendOp,
  type ExtractArgOp,
  type FilterLogicalOp,
  formatDataTableSql,
  formatExtractArgExpr,
  formatFilterConditions,
  type FormatOptions,
  type FormatResult,
  type GraphTreeOp,
  type IntervalFillGapsOp,
  type IntervalFlattenOp,
  type IntervalIntersectOp,
  type IntervalMergeOp,
  type IntervalsFromEventsOp,
  type JoinOp,
  type LimitOp,
  type LogicalNode,
  type LogicalPlan,
  type PassthroughOp,
  type PipelineAggregationSpec,
  type ProjectOp,
  type RawSqlOp,
  type RenameOp,
  type SetOp,
  type SortOp,
  type TimeRangeOp,
  type TreeAccumulateOp,
  type UnionOp,
} from './ir';

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
  | RenameOp
  | SetOp
  | AsOp
  | DistinctOp
  | AggregateOp
  | SortOp
  | LimitOp
  | IntervalFlattenOp
  | IntervalMergeOp
  | TreeAccumulateOp;

function formatPipelineAggs(aggs: readonly PipelineAggregationSpec[]): string {
  return aggs
    .map(
      (a) => `${a.func}(${a.func === 'COUNT' ? '*' : a.column}) AS ${a.alias}`,
    )
    .join(', ');
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

    case 'rename': {
      if (op.renames.length === 0) return;
      const items = op.renames.map((r) => `${r.from} AS ${r.to}`).join(', ');
      stmt.stages.push(`RENAME ${items}`);
      return;
    }

    case 'set': {
      if (op.assignments.length === 0) return;
      const items = op.assignments
        .map((a) => `${a.column} = ${a.expression}`)
        .join(', ');
      stmt.stages.push(`SET ${items}`);
      return;
    }

    case 'as': {
      stmt.stages.push(`AS ${op.alias}`);
      return;
    }

    case 'distinct': {
      stmt.stages.push('DISTINCT');
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
      stmt.stages.push(
        op.offset !== undefined && op.offset > 0
          ? `LIMIT ${op.count} OFFSET ${op.offset}`
          : `LIMIT ${op.count}`,
      );
      return;
    }

    case 'interval_flatten': {
      if (op.filterNegativeDur) {
        stmt.stages.push('WHERE dur >= 0');
      }
      const perClause =
        op.partitionColumns.length > 0
          ? ` PER ${op.partitionColumns.join(', ')}`
          : '';
      if (op.aggregations.length === 0) {
        stmt.stages.push(
          `INTERVAL FLATTEN${perClause} AGGREGATE COUNT(*) AS _pf_cnt`,
        );
        stmt.stages.push('DROP _pf_cnt');
      } else {
        stmt.stages.push(
          `INTERVAL FLATTEN${perClause} AGGREGATE ${formatPipelineAggs(op.aggregations)}`,
        );
      }
      return;
    }

    case 'interval_merge': {
      if (op.filterNegativeDur) {
        stmt.stages.push('WHERE dur >= 0');
      }
      const byClause =
        op.mergeKind === 'CONSECUTIVE' && op.keyColumns.length > 0
          ? ` BY ${op.keyColumns.join(', ')}`
          : '';
      const perClause =
        op.partitionColumns.length > 0
          ? ` PER ${op.partitionColumns.join(', ')}`
          : '';
      if (op.aggregations.length === 0) {
        stmt.stages.push(
          `INTERVAL MERGE ${op.mergeKind}${byClause}${perClause} AGGREGATE COUNT(*) AS _pf_cnt`,
        );
        stmt.stages.push('DROP _pf_cnt');
      } else {
        stmt.stages.push(
          `INTERVAL MERGE ${op.mergeKind}${byClause}${perClause} AGGREGATE ${formatPipelineAggs(op.aggregations)}`,
        );
      }
      return;
    }

    case 'tree_accumulate': {
      if (op.aggregations.length === 0) return;
      const renamesIn: string[] = [];
      const renamesOut: string[] = [];
      if (op.idColumn !== 'id') {
        renamesIn.push(`${op.idColumn} AS id`);
        renamesOut.push(`id AS ${op.idColumn}`);
      }
      if (op.parentIdColumn !== 'parent_id') {
        renamesIn.push(`${op.parentIdColumn} AS parent_id`);
        renamesOut.push(`parent_id AS ${op.parentIdColumn}`);
      }
      if (renamesIn.length > 0) {
        stmt.stages.push(`RENAME ${renamesIn.join(', ')}`);
      }
      stmt.stages.push(
        `TREE ACCUMULATE ${op.direction} ${formatPipelineAggs(op.aggregations)}`,
      );
      if (renamesOut.length > 0) {
        stmt.stages.push(`RENAME ${renamesOut.join(', ')}`);
      }
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

function makeDataTablePipe(op: DataTableOp): PipeStatement {
  const body = formatDataTableSql(op)
    .split('\n')
    .map((line) => `  ${line}`)
    .join('\n');
  return {
    source: `(\n${body}\n)`,
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

function appendFillGapsStage(
  stmt: PipeStatement,
  op: IntervalFillGapsOp,
  bgRef: string,
): void {
  let bgSource = bgRef;
  if (op.inputColumns !== undefined && op.backgroundColumns !== undefined) {
    const inputSet = new Set(op.inputColumns);
    const uniquePer = [...new Set(op.partitionColumns)];
    const bgHasAllPer =
      uniquePer.length > 0 &&
      uniquePer.every((p) => op.backgroundColumns?.includes(p));
    const kept = op.backgroundColumns.filter(
      (c) =>
        inputSet.has(c) &&
        (uniquePer.length === 0 || bgHasAllPer || !uniquePer.includes(c)),
    );
    if (kept.length > 0 && kept.length < op.backgroundColumns.length) {
      bgSource = `(FROM ${bgRef} |> SELECT ${kept.join(', ')})`;
    }
  }
  const perSuffix =
    op.partitionColumns.length > 0
      ? ` PER ${op.partitionColumns.join(', ')}`
      : '';
  stmt.stages.push(`INTERVAL FILL GAPS WITH ${bgSource}${perSuffix}`);
}

function appendIntervalsFromEventsStage(
  stmt: PipeStatement,
  op: IntervalsFromEventsOp,
  stopRef: string | undefined,
): void {
  const perClause =
    op.partitionColumns.length > 0
      ? ` PER ${op.partitionColumns.join(', ')}`
      : '';
  const stopClause =
    stopRef !== undefined && stopRef !== '' ? ` STOP ${stopRef}` : '';
  const lastClause =
    op.lastMode === 'KEEP_OPEN'
      ? ' KEEPING LAST OPEN'
      : ` CLOSING LAST AT (${op.closeAtExpr})`;
  stmt.stages.push(
    `INTERVALS FROM EVENTS${perClause}${stopClause}${lastClause}`,
  );
}

function makeGraphTreePipe(
  op: GraphTreeOp,
  nodesRef: string,
  edgesRef: string,
  rootsRef: string,
): PipeStatement {
  const nodesSrc =
    op.nodeIdColumn === 'id'
      ? nodesRef
      : `(FROM ${nodesRef} |> RENAME ${op.nodeIdColumn} AS id)`;
  const edgesSrc =
    op.sourceColumn === 'src' && op.destColumn === 'dst'
      ? edgesRef
      : `(FROM ${edgesRef} |> SELECT ${op.sourceColumn} AS src, ${op.destColumn} AS dst)`;
  const rootsSrc =
    op.rootIdColumn === 'id'
      ? rootsRef
      : `(FROM ${rootsRef} |> SELECT ${op.rootIdColumn} AS id)`;
  return {
    source: [
      `GRAPH ${op.treeKind} TREE`,
      `  NODES ${nodesSrc}`,
      `  EDGES ${edgesSrc}`,
      `  FROM ${rootsSrc}`,
    ].join('\n'),
    rawSourceClause: true,
    stages: [],
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

// --- Format LogicalPlan -> PerfettoSQL Next ---

interface PipeStep {
  readonly token: string;
  readonly sql: string;
}

function indentBlock(text: string, prefix: string): string {
  return text
    .split('\n')
    .map((line) => `${prefix}${line}`)
    .join('\n');
}

// Find the index of the matching `)` for the `(` at `openIdx`, respecting SQL
// single- and double-quoted strings. Returns -1 if unbalanced.
function findMatchingParen(sql: string, openIdx: number): number {
  let depth = 0;
  let inSingle = false;
  let inDouble = false;
  for (let i = openIdx; i < sql.length; i++) {
    const ch = sql[i];
    if (inSingle) {
      if (ch === "'") {
        if (sql[i + 1] === "'") {
          i++;
        } else {
          inSingle = false;
        }
      }
      continue;
    }
    if (inDouble) {
      if (ch === '"') {
        if (sql[i + 1] === '"') {
          i++;
        } else {
          inDouble = false;
        }
      }
      continue;
    }
    if (ch === "'") {
      inSingle = true;
    } else if (ch === '"') {
      inDouble = true;
    } else if (ch === '(') {
      depth++;
    } else if (ch === ')') {
      depth--;
      if (depth === 0) return i;
    }
  }
  return -1;
}

// If `sql` is a pure SQL query (either `FROM (<select-stmt>)` with no trailing
// `|>` stages, or a raw `SELECT` / `WITH` query), return its parenthesized
// `<source>` form `(<select-stmt>)`; otherwise return `undefined`.
function extractPureSqlSource(sql: string): string | undefined {
  if (sql.startsWith('FROM (') && sql.endsWith(')')) {
    const closeIdx = findMatchingParen(sql, 5);
    if (closeIdx === sql.length - 1) {
      return sql.slice(5);
    }
  }
  if (/^(select|with)\b/i.test(sql.trimStart())) {
    return `(\n${indentBlock(sql, '  ')}\n)`;
  }
  return undefined;
}

function countTokenOccurrences(sql: string, token: string): number {
  return sql.split(token).length - 1;
}

function inlineStepIntoPipeSql(
  consumerSql: string,
  token: string,
  stepSql: string,
): string {
  const pureSqlSource = extractPureSqlSource(stepSql);

  // 1. Consumer starts directly with `FROM <token>` at the top of a pipeline.
  const topPrefix = `FROM ${token}`;
  if (
    stepSql.startsWith('FROM ') &&
    (consumerSql === topPrefix || consumerSql.startsWith(`${topPrefix}\n|>`))
  ) {
    return `${stepSql}${consumerSql.slice(topPrefix.length)}`;
  }

  // 2. Consumer wrapped `<token>` in `(FROM <token> |> <stages>)` and `stepSql`
  //    is itself a `FROM ...` pipeline — flatten into one subpipeline.
  const wrappedMarker = `(FROM ${token} |> `;
  const wrappedIdx = consumerSql.indexOf(wrappedMarker);
  if (
    wrappedIdx !== -1 &&
    pureSqlSource === undefined &&
    stepSql.startsWith('FROM ')
  ) {
    const closeIdx = findMatchingParen(consumerSql, wrappedIdx);
    if (closeIdx !== -1) {
      const extraStages = consumerSql.slice(
        wrappedIdx + wrappedMarker.length,
        closeIdx,
      );
      const lineStart = consumerSql.lastIndexOf('\n', wrappedIdx - 1) + 1;
      const lineIndent =
        consumerSql.slice(lineStart, wrappedIdx).match(/^\s*/)?.[0] ?? '';
      const combined = `${stepSql}\n|> ${extraStages}`;
      const replacement = `(\n${indentBlock(combined, `${lineIndent}  `)}\n${lineIndent})`;
      return (
        consumerSql.slice(0, wrappedIdx) +
        replacement +
        consumerSql.slice(closeIdx + 1)
      );
    }
  }

  // 3. General `<source>` slot (`NODES <token>`, `JOIN <token> AS r`, etc.).
  const idx = consumerSql.indexOf(token);
  if (idx === -1) return consumerSql;
  const lineStart = consumerSql.lastIndexOf('\n', idx - 1) + 1;
  const lineIndent = consumerSql.slice(lineStart, idx).match(/^\s*/)?.[0] ?? '';
  const sourceExpr = pureSqlSource ?? `(\n${indentBlock(stepSql, '  ')}\n)`;
  const indented = sourceExpr
    .split('\n')
    .map((line, i) => (i === 0 ? line : `${lineIndent}${line}`))
    .join('\n');
  return (
    consumerSql.slice(0, idx) + indented + consumerSql.slice(idx + token.length)
  );
}

export function formatPipe(
  plan: LogicalPlan,
  options?: FormatOptions,
): FormatResult {
  if (plan.nodes.length === 0) {
    return {ok: false, error: 'Empty logical plan.'};
  }

  const nodeById = new Map<string, LogicalNode>();
  for (const node of plan.nodes) {
    nodeById.set(node.nodeId, node);
  }

  const steps: PipeStep[] = [];
  const emittedAs = new Map<string, string>();
  const allIncludes = new Set<string>();

  let current: PipeStatement | undefined;
  let currentTailId: string | undefined;

  function emitStep(sql: string): string {
    const token = `__spag_step_${steps.length + 1}__`;
    steps.push({token, sql});
    return token;
  }

  function emitCurrent(): void {
    if (current !== undefined && currentTailId !== undefined) {
      const sql = pipeStatementToSql(current);
      const token = emitStep(sql);
      emittedAs.set(currentTailId, token);
    }
    current = undefined;
    currentTailId = undefined;
  }

  function resolveRef(id: string): string {
    return emittedAs.get(id) ?? '';
  }

  function startPipeline(nodeId: string, stmt: PipeStatement): void {
    emitCurrent();
    current = stmt;
    currentTailId = nodeId;
  }

  function emitStage(nodeId: string, op: PipeStageOp): boolean {
    const inputIsCurrentTail = op.input === currentTailId;
    const inputSingleRef = (nodeById.get(op.input)?.refCount ?? 0) <= 1;

    if (inputIsCurrentTail && inputSingleRef && current !== undefined) {
      appendStageOp(current, op);
      currentTailId = nodeId;
      return true;
    }

    emitCurrent();
    const inputRef = resolveRef(op.input);
    if (inputRef === '') return false;
    current = {source: inputRef, stages: []};
    appendStageOp(current, op);
    currentTailId = nodeId;
    return true;
  }

  for (const node of plan.nodes) {
    const {nodeId, op} = node;
    switch (op.kind) {
      case 'scan': {
        // Scans bind table names directly without disturbing an unrelated
        // active pipeline branch on a multi-input graph.
        emittedAs.set(nodeId, op.table);
        if (op.includes) {
          for (const inc of op.includes) allIncludes.add(inc);
        }
        break;
      }

      case 'time_range': {
        startPipeline(nodeId, makeTimeRangePipe(op));
        break;
      }

      case 'data_table': {
        startPipeline(nodeId, makeDataTablePipe(op));
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
          break;
        }

        emitCurrent();
        const leftRef = resolveRef(op.left);
        const rightRef = resolveRef(op.right);
        if (leftRef === '' || rightRef === '') {
          return {ok: false, error: 'Failed to resolve Join inputs.'};
        }
        const stmt: PipeStatement = {source: leftRef, stages: []};
        appendJoinStages(stmt, op, rightRef);
        startPipeline(nodeId, stmt);
        break;
      }

      case 'union': {
        emitCurrent();
        const refs = op.inputs.map(resolveRef);
        if (refs.length === 0 || refs.some((r) => r === '')) {
          return {ok: false, error: 'Failed to resolve Union inputs.'};
        }
        startPipeline(nodeId, makeUnionPipe(op, refs));
        break;
      }

      case 'interval_intersect': {
        emitCurrent();
        const inputIds = op.inputs.map((inp) => inp.nodeId);
        const refs = inputIds.map(resolveRef);
        if (refs.length === 0 || refs.some((r) => r === '')) {
          return {
            ok: false,
            error: 'Failed to resolve Interval Intersect inputs.',
          };
        }
        startPipeline(nodeId, makeIntervalIntersectPipe(op, refs));
        break;
      }

      case 'interval_fill_gaps': {
        const fgIsCurrentTail =
          op.input === currentTailId && op.background !== currentTailId;
        const fgSingleRef = (nodeById.get(op.input)?.refCount ?? 0) <= 1;
        const bgRefIfFolded = resolveRef(op.background);

        if (
          fgIsCurrentTail &&
          fgSingleRef &&
          current !== undefined &&
          bgRefIfFolded !== ''
        ) {
          appendFillGapsStage(current, op, bgRefIfFolded);
          currentTailId = nodeId;
          break;
        }

        emitCurrent();
        const fgRef = resolveRef(op.input);
        const bgRef = resolveRef(op.background);
        if (fgRef === '' || bgRef === '') {
          return {
            ok: false,
            error: 'Failed to resolve Interval Fill Gaps inputs.',
          };
        }
        const stmt: PipeStatement = {source: fgRef, stages: []};
        appendFillGapsStage(stmt, op, bgRef);
        startPipeline(nodeId, stmt);
        break;
      }

      case 'intervals_from_events': {
        const inSingleRef = (nodeById.get(op.input)?.refCount ?? 0) <= 1;
        const canFold =
          op.input === currentTailId &&
          op.stop !== currentTailId &&
          inSingleRef &&
          current !== undefined &&
          (op.stop === undefined || resolveRef(op.stop) !== '');

        if (canFold && current !== undefined) {
          const stopRef =
            op.stop !== undefined ? resolveRef(op.stop) : undefined;
          appendIntervalsFromEventsStage(current, op, stopRef);
          currentTailId = nodeId;
          break;
        }

        emitCurrent();
        const inRef = resolveRef(op.input);
        const stopRef = op.stop !== undefined ? resolveRef(op.stop) : undefined;
        if (inRef === '' || (op.stop !== undefined && stopRef === '')) {
          return {
            ok: false,
            error: 'Failed to resolve Intervals From Events inputs.',
          };
        }
        const stmt: PipeStatement = {source: inRef, stages: []};
        appendIntervalsFromEventsStage(stmt, op, stopRef);
        startPipeline(nodeId, stmt);
        break;
      }

      case 'graph_tree': {
        emitCurrent();
        const nodesRef = resolveRef(op.nodes);
        const edgesRef = resolveRef(op.edges);
        const rootsRef = resolveRef(op.roots);
        if (nodesRef === '' || edgesRef === '' || rootsRef === '') {
          return {ok: false, error: 'Failed to resolve Graph Tree inputs.'};
        }
        startPipeline(
          nodeId,
          makeGraphTreePipe(op, nodesRef, edgesRef, rootsRef),
        );
        break;
      }

      case 'raw_sql': {
        emitCurrent();
        const inputIds = op.inputs.map((inp) => inp.nodeId);
        if (inputIds.some((id) => resolveRef(id) === '')) {
          return {ok: false, error: 'Failed to resolve SQL inputs.'};
        }
        const token = emitStep(emitRawSql(op, resolveRef));
        emittedAs.set(nodeId, token);
        break;
      }

      case 'passthrough':
      case 'filter':
      case 'project':
      case 'extend':
      case 'extract_arg':
      case 'drop':
      case 'rename':
      case 'set':
      case 'as':
      case 'distinct':
      case 'aggregate':
      case 'sort':
      case 'limit':
      case 'interval_flatten':
      case 'interval_merge':
      case 'tree_accumulate': {
        if (!emitStage(nodeId, op)) {
          return {ok: false, error: `Failed to resolve ${op.kind} input.`};
        }
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
    emitStep(`FROM ${targetNode.op.table}`);
  }

  const lastStep = steps[steps.length - 1];
  if (!lastStep) {
    return {ok: false, error: 'Empty PerfettoSQL Next output.'};
  }

  const stepEntries = steps.slice(0, -1);
  const sqlByToken = new Map<string, string>();
  for (const s of steps) {
    sqlByToken.set(s.token, s.sql);
  }

  const inlinedTokens = new Set<string>();
  for (const [idx, step] of stepEntries.entries()) {
    const stepSql = sqlByToken.get(step.token) ?? step.sql;
    const downstream = steps
      .slice(idx + 1)
      .filter((s) => !inlinedTokens.has(s.token));

    let totalRefs = 0;
    let consumerToken: string | undefined;
    let usedInPlainSqlFrom = false;
    for (const d of downstream) {
      const dSql = sqlByToken.get(d.token) ?? d.sql;
      const count = countTokenOccurrences(dSql, step.token);
      if (count > 0) {
        totalRefs += count;
        consumerToken = d.token;
        if (dSql.includes(`SELECT * FROM ${step.token}`)) {
          usedInPlainSqlFrom = true;
        }
      }
    }

    if (totalRefs === 0) {
      inlinedTokens.add(step.token);
      continue;
    }

    const isPureSql = extractPureSqlSource(stepSql) !== undefined;
    if (
      totalRefs === 1 &&
      consumerToken !== undefined &&
      (isPureSql || !usedInPlainSqlFrom)
    ) {
      const consumerSql = sqlByToken.get(consumerToken) ?? '';
      sqlByToken.set(
        consumerToken,
        inlineStepIntoPipeSql(consumerSql, step.token, stepSql),
      );
      inlinedTokens.add(step.token);
    }
  }

  const remainingSteps = stepEntries.filter((s) => !inlinedTokens.has(s.token));
  const nameMap = new Map<string, string>();
  for (const [i, step] of remainingSteps.entries()) {
    nameMap.set(step.token, `step_${i + 1}`);
  }

  function renameTokens(sql: string): string {
    let result = sql;
    for (const [token, name] of nameMap) {
      result = result.replaceAll(token, name);
    }
    return result;
  }

  const includes = [...allIncludes];
  const includeHeaders = options?.includeModuleHeaders ?? true;
  const blocks: string[] = [];

  if (includeHeaders && includes.length > 0) {
    blocks.push(
      includes.map((inc) => `INCLUDE PERFETTO MODULE ${inc};`).join('\n'),
    );
  }

  for (const step of remainingSteps) {
    const stepName = nameMap.get(step.token);
    const stepSql = sqlByToken.get(step.token) ?? step.sql;
    blocks.push(
      `CREATE OR REPLACE PERFETTO TABLE ${stepName} AS\n${renameTokens(stepSql)};`,
    );
  }

  const finalSql = sqlByToken.get(lastStep.token) ?? lastStep.sql;
  blocks.push(renameTokens(finalSql));

  return {
    ok: true,
    sql: blocks.join('\n\n'),
    includes,
  };
}

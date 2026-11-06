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
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {NodeData} from './graph_model';
import {
  collectUpstream,
  findConnectedInputs,
  findDockedParent,
  fnvHash,
  getManifest,
  getManifestInputs,
  getOutputColumnsForNode,
  type GraphIndex,
  isNodeValid,
} from './graph_utils';
import type {LogicalOpContext} from './node_types';
import {formatPipe} from './pipe_backend';
import {formatSql} from './sql_backend';

export {formatPipe, formatSql};

export type SqlDialect = 'sql' | 'pipe';

// ---------------------------------------------------------------------------
// Language-agnostic Logical IR for Spaghetti query graphs.
// ---------------------------------------------------------------------------

export type FilterOp =
  | '='
  | '!='
  | '>'
  | '>='
  | '<'
  | '<='
  | 'LIKE'
  | 'NOT LIKE'
  | 'GLOB'
  | 'IS NULL'
  | 'IS NOT NULL';

export type FilterConjunction = 'AND' | 'OR';

export interface LogicalFilterCondition {
  readonly column: string;
  readonly op: FilterOp;
  readonly value: string;
}

export interface ProjectedColumn {
  readonly column: string;
  readonly alias?: string;
}

export interface ComputedColumn {
  readonly expression: string;
  readonly alias: string;
}

export interface ArgExtraction {
  readonly argName: string;
  readonly alias: string;
}

export type AggregationFunc = 'COUNT' | 'SUM' | 'AVG' | 'MIN' | 'MAX';

export interface AggregationSpec {
  readonly func: AggregationFunc;
  readonly column: string;
  readonly alias: string;
}

export type PipelineAggFunc = 'COUNT' | 'SUM' | 'MIN' | 'MAX';

export interface PipelineAggregationSpec {
  readonly func: PipelineAggFunc;
  readonly column: string;
  readonly alias: string;
}

export interface RenameMapping {
  readonly from: string;
  readonly to: string;
}

export interface SetAssignment {
  readonly column: string;
  readonly expression: string;
}

export interface SortKey {
  readonly column: string;
  readonly order: 'ASC' | 'DESC';
}

export interface JoinColumnSpec {
  readonly column: string;
  readonly alias: string;
}

export interface IntervalIntersectSourceCol {
  readonly name: string;
  readonly alias: string;
}

export interface IntervalIntersectInput {
  readonly nodeId: string;
  readonly columns: readonly IntervalIntersectSourceCol[];
}

export interface RawSqlInput {
  readonly nodeId: string;
  readonly alias: string;
}

// --- Logical relational operators ---

export interface ScanOp {
  readonly kind: 'scan';
  readonly table: string;
  readonly includes?: readonly string[];
}

export interface TimeRangeOp {
  readonly kind: 'time_range';
  readonly ts: string;
  readonly dur: string;
}

export type DataTableColumnType =
  'int' | 'double' | 'string' | 'timestamp' | 'duration' | 'boolean';

export interface DataTableColumn {
  readonly name: string;
  readonly type: DataTableColumnType;
}

export interface DataTableOp {
  readonly kind: 'data_table';
  readonly columns: readonly DataTableColumn[];
  readonly rows: readonly (readonly string[])[];
}

export interface PassthroughOp {
  readonly kind: 'passthrough';
  readonly input: string;
}

export interface FilterLogicalOp {
  readonly kind: 'filter';
  readonly input: string;
  readonly conditions: readonly LogicalFilterCondition[];
  readonly conjunction: FilterConjunction;
}

export interface ProjectOp {
  readonly kind: 'project';
  readonly input: string;
  readonly columns: readonly ProjectedColumn[];
  readonly expressions: readonly ComputedColumn[];
}

export interface ExtendOp {
  readonly kind: 'extend';
  readonly input: string;
  readonly expressions: readonly ComputedColumn[];
}

export interface ExtractArgOp {
  readonly kind: 'extract_arg';
  readonly input: string;
  readonly argSetIdCol: string;
  readonly extractions: readonly ArgExtraction[];
}

export interface DropOp {
  readonly kind: 'drop';
  readonly input: string;
  readonly dropColumns: readonly string[];
  readonly remainingColumns: readonly string[] | undefined;
}

export interface RenameOp {
  readonly kind: 'rename';
  readonly input: string;
  readonly renames: readonly RenameMapping[];
  readonly inputColumns: readonly string[] | undefined;
}

export interface SetOp {
  readonly kind: 'set';
  readonly input: string;
  readonly assignments: readonly SetAssignment[];
  readonly inputColumns: readonly string[] | undefined;
}

export interface AsOp {
  readonly kind: 'as';
  readonly input: string;
  readonly alias: string;
}

export interface DistinctOp {
  readonly kind: 'distinct';
  readonly input: string;
}

export interface AggregateOp {
  readonly kind: 'aggregate';
  readonly input: string;
  readonly groupColumns: readonly string[];
  readonly aggregations: readonly AggregationSpec[];
}

export interface SortOp {
  readonly kind: 'sort';
  readonly input: string;
  readonly keys: readonly SortKey[];
}

export interface LimitOp {
  readonly kind: 'limit';
  readonly input: string;
  readonly count: number;
  readonly offset?: number;
}

export interface JoinOp {
  readonly kind: 'join';
  readonly left: string;
  readonly right: string;
  readonly joinType: 'LEFT' | 'INNER';
  readonly leftColumn: string;
  readonly rightColumn: string;
  readonly rightColumns: readonly JoinColumnSpec[];
}

export interface UnionOp {
  readonly kind: 'union';
  readonly inputs: readonly string[];
  readonly distinct: boolean;
}

export interface IntervalIntersectOp {
  readonly kind: 'interval_intersect';
  readonly inputs: readonly IntervalIntersectInput[];
  readonly partitionColumns: readonly string[];
  readonly filterNegativeDur: boolean;
}

export interface IntervalFlattenOp {
  readonly kind: 'interval_flatten';
  readonly input: string;
  readonly partitionColumns: readonly string[];
  readonly aggregations: readonly PipelineAggregationSpec[];
  readonly filterNegativeDur: boolean;
}

export interface IntervalFillGapsOp {
  readonly kind: 'interval_fill_gaps';
  readonly input: string;
  readonly background: string;
  readonly partitionColumns: readonly string[];
  readonly inputColumns: readonly string[] | undefined;
  readonly backgroundColumns: readonly string[] | undefined;
}

export type IntervalMergeKind = 'OVERLAPPING' | 'CONSECUTIVE';

export interface IntervalMergeOp {
  readonly kind: 'interval_merge';
  readonly input: string;
  readonly mergeKind: IntervalMergeKind;
  readonly keyColumns: readonly string[];
  readonly partitionColumns: readonly string[];
  readonly aggregations: readonly PipelineAggregationSpec[];
  readonly filterNegativeDur: boolean;
}

export type EventsLastMode = 'KEEP_OPEN' | 'CLOSE_AT';

export interface IntervalsFromEventsOp {
  readonly kind: 'intervals_from_events';
  readonly input: string;
  readonly stop: string | undefined;
  readonly partitionColumns: readonly string[];
  readonly lastMode: EventsLastMode;
  readonly closeAtExpr: string;
  readonly inputColumns: readonly string[] | undefined;
}

export interface TreeAccumulateOp {
  readonly kind: 'tree_accumulate';
  readonly input: string;
  readonly direction: 'UP' | 'DOWN';
  readonly idColumn: string;
  readonly parentIdColumn: string;
  readonly aggregations: readonly PipelineAggregationSpec[];
}

export type GraphTreeKind = 'BFS' | 'DFS' | 'DOMINATOR';

export interface GraphTreeOp {
  readonly kind: 'graph_tree';
  readonly treeKind: GraphTreeKind;
  readonly nodes: string;
  readonly edges: string;
  readonly roots: string;
  readonly nodeIdColumn: string;
  readonly sourceColumn: string;
  readonly destColumn: string;
  readonly rootIdColumn: string;
  readonly nodeColumns: readonly string[] | undefined;
}

export interface RawSqlOp {
  readonly kind: 'raw_sql';
  readonly sql: string;
  readonly inputs: readonly RawSqlInput[];
}

export type LogicalOp =
  | ScanOp
  | TimeRangeOp
  | DataTableOp
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
  | JoinOp
  | UnionOp
  | IntervalIntersectOp
  | IntervalFlattenOp
  | IntervalFillGapsOp
  | IntervalMergeOp
  | IntervalsFromEventsOp
  | TreeAccumulateOp
  | GraphTreeOp
  | RawSqlOp;

export interface LogicalNode {
  readonly nodeId: string;
  readonly op: LogicalOp;
  readonly hash: string;
  readonly refCount: number;
}

export interface LogicalPlan {
  readonly nodes: readonly LogicalNode[];
  readonly targetNodeId: string;
}

export interface FormatOptions {
  readonly includeModuleHeaders?: boolean;
}

export type FormatResult =
  | {
      readonly ok: true;
      readonly sql: string;
      readonly includes: readonly string[];
    }
  | {
      readonly ok: false;
      readonly error: string;
    };

// --- Structural traversal & dialect-agnostic Merkle hashing ---

export function mapLogicalOpInputs(
  op: LogicalOp,
  mapId: (nodeId: string) => string,
): LogicalOp {
  switch (op.kind) {
    case 'scan':
    case 'time_range':
    case 'data_table':
      return op;
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
    case 'tree_accumulate':
      return {...op, input: mapId(op.input)};
    case 'intervals_from_events':
      return {
        ...op,
        input: mapId(op.input),
        stop: op.stop !== undefined ? mapId(op.stop) : undefined,
      };
    case 'join':
      return {...op, left: mapId(op.left), right: mapId(op.right)};
    case 'interval_fill_gaps':
      return {...op, input: mapId(op.input), background: mapId(op.background)};
    case 'graph_tree':
      return {
        ...op,
        nodes: mapId(op.nodes),
        edges: mapId(op.edges),
        roots: mapId(op.roots),
      };
    case 'union':
      return {...op, inputs: op.inputs.map(mapId)};
    case 'interval_intersect':
      return {
        ...op,
        inputs: op.inputs.map((i) => ({...i, nodeId: mapId(i.nodeId)})),
      };
    case 'raw_sql':
      return {
        ...op,
        inputs: op.inputs.map((i) => ({...i, nodeId: mapId(i.nodeId)})),
      };
    default:
      return assertUnreachable(op);
  }
}

export function getLogicalOpInputs(op: LogicalOp): string[] {
  const inputs: string[] = [];
  mapLogicalOpInputs(op, (id) => {
    inputs.push(id);
    return id;
  });
  return inputs;
}

export function computeOpMerkleHash(
  op: LogicalOp,
  getHash: (nodeId: string) => string,
): string {
  if (op.kind === 'passthrough') return getHash(op.input);
  const normalized =
    op.kind === 'scan'
      ? {
          kind: 'scan',
          table: op.table,
          includes: op.includes ? [...op.includes].sort() : [],
        }
      : mapLogicalOpInputs(op, getHash);
  return `_qb_${fnvHash(JSON.stringify(normalized))}`;
}

export function collectLogicalPlanIncludes(plan: LogicalPlan): string[] {
  const includes = new Set<string>();
  for (const {op} of plan.nodes) {
    if (op.kind === 'scan') {
      for (const inc of op.includes ?? []) includes.add(inc);
    } else if (op.kind === 'interval_intersect') {
      includes.add('intervals.intersect');
    } else if (op.kind === 'graph_tree') {
      includes.add('graphs.search');
      if (op.treeKind === 'DOMINATOR') includes.add('graphs.dominator_tree');
    }
  }
  return [...includes];
}

// --- Shared scalar formatting helpers ---

export function isUnaryFilterOp(op: FilterOp): boolean {
  return op === 'IS NULL' || op === 'IS NOT NULL';
}

function isRawValue(value: string): boolean {
  if (value === '') return true;
  if (/^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(value)) return true;
  if (/^null$/i.test(value)) return true;
  if (value.startsWith("'") || value.startsWith('"')) return true;
  if (value.startsWith('(')) return true;
  return false;
}

function quoteValue(value: string): string {
  return isRawValue(value) ? value : `'${value.replace(/'/g, "''")}'`;
}

export function formatFilterConditions(
  conditions: readonly LogicalFilterCondition[],
  conjunction: FilterConjunction = 'AND',
): string {
  return conditions
    .filter((c) => c.column !== '')
    .map((c) =>
      isUnaryFilterOp(c.op)
        ? `${c.column} ${c.op}`
        : `${c.column} ${c.op} ${quoteValue(c.value)}`,
    )
    .join(` ${conjunction} `);
}

export function formatExtractArgExpr(
  argSetIdCol: string,
  argName: string,
): string {
  return `extract_arg(${argSetIdCol}, '${argName.replace(/'/g, "''")}')`;
}

function sqlCastType(type: DataTableColumnType): string {
  switch (type) {
    case 'int':
    case 'timestamp':
    case 'duration':
    case 'boolean':
      return 'INTEGER';
    case 'double':
      return 'REAL';
    case 'string':
      return 'TEXT';
    default:
      return assertUnreachable(type);
  }
}

function formatDataTableCell(raw: string, type: DataTableColumnType): string {
  if (type === 'string') {
    return raw === '' ? 'CAST(NULL AS TEXT)' : `'${raw.replaceAll("'", "''")}'`;
  }
  const trimmed = raw.trim();
  if (trimmed === '') return `CAST(NULL AS ${sqlCastType(type)})`;
  switch (type) {
    case 'int':
    case 'timestamp':
    case 'duration':
      return BigInt(trimmed).toString();
    case 'double': {
      const s = String(Number(trimmed));
      return /^[+-]?\d+$/.test(s) ? `${s}.0` : s;
    }
    case 'boolean':
      return /^(true|1)$/i.test(trimmed) ? '1' : '0';
    default:
      return assertUnreachable(type);
  }
}

export function formatDataTableSql(op: DataTableOp): string {
  if (op.rows.length === 0) {
    const emptyCols = op.columns.map(
      (c) => `CAST(NULL AS ${sqlCastType(c.type)}) AS ${c.name}`,
    );
    return `SELECT\n  ${emptyCols.join(',\n  ')}\nWHERE 0`;
  }
  const colNames = op.columns.map((c) => c.name).join(', ');
  const valueTuples = op.rows.map((row) => {
    const cells = op.columns.map((col, idx) =>
      formatDataTableCell(row[idx] ?? '', col.type),
    );
    return `    (${cells.join(', ')})`;
  });
  return [
    `WITH _data(${colNames}) AS (`,
    '  VALUES',
    valueTuples.join(',\n'),
    ')',
    `SELECT ${colNames} FROM _data`,
  ].join('\n');
}

// --- Graph -> LogicalPlan & whole-canvas analysis ---

function buildNodeLogicalOp(
  index: GraphIndex,
  n: NodeData,
  sqlModules: SqlModules | undefined,
): LogicalOp | undefined {
  const manifest = getManifest(n.type);
  const ports = getManifestInputs(manifest, n);
  const dockedParent = findDockedParent(index, n.id);
  const connected = findConnectedInputs(index, n.id);

  const portInputs = new Map<string, NodeData>();
  for (const [i, port] of ports.entries()) {
    const input = i === 0 && dockedParent ? dockedParent : connected.get(i);
    if (input) portInputs.set(port.name, input);
  }

  const ctx: LogicalOpContext = {
    inputPorts: ports,
    sqlModules,
    getInputNodeId: (portName) => portInputs.get(portName)?.id,
    getInputColumns(portName) {
      const input = portInputs.get(portName);
      return input
        ? getOutputColumnsForNode(index, input.id, sqlModules)
        : undefined;
    },
  };
  return manifest.toLogicalOp(n.config, ctx);
}

export function buildLogicalPlan(
  index: GraphIndex,
  nodeId: string,
  sqlModules: SqlModules | undefined,
): LogicalPlan | undefined {
  if (!index.nodes[nodeId]) return undefined;

  const order: NodeData[] = [];
  collectUpstream(index, nodeId, new Set(), order);
  if (order.length === 0 || order.some((n) => !isNodeValid(n))) {
    return undefined;
  }

  const raw: Array<{nodeId: string; op: LogicalOp; hash: string}> = [];
  const hashes = new Map<string, string>();
  const refCounts = new Map<string, number>();

  for (const n of order) {
    const op = buildNodeLogicalOp(index, n, sqlModules);
    if (!op) return undefined;
    const inputs = getLogicalOpInputs(op);
    if (inputs.some((id) => !hashes.has(id))) return undefined;
    for (const id of inputs) {
      refCounts.set(id, (refCounts.get(id) ?? 0) + 1);
    }
    const hash = computeOpMerkleHash(op, (id) => hashes.get(id)!);
    hashes.set(n.id, hash);
    raw.push({nodeId: n.id, op, hash});
  }

  return {
    nodes: raw.map((r) => ({...r, refCount: refCounts.get(r.nodeId) ?? 0})),
    targetNodeId: nodeId,
  };
}

export function collectLiveHashes(
  index: GraphIndex,
  sqlModules: SqlModules | undefined,
): Set<string> {
  const validOps = new Map<string, LogicalOp>();
  const hashes = new Map<string, string>();
  const visiting = new Set<string>();
  const evaluated = new Set<string>();

  function evalNode(id: string): string | undefined {
    if (evaluated.has(id)) return hashes.get(id);
    if (visiting.has(id)) return undefined;
    const node = index.nodes[id];
    if (!node || !isNodeValid(node)) {
      evaluated.add(id);
      return undefined;
    }
    visiting.add(id);
    const op = buildNodeLogicalOp(index, node, sqlModules);
    const inputsOk =
      op !== undefined &&
      getLogicalOpInputs(op).every((inp) => evalNode(inp) !== undefined);
    visiting.delete(id);
    evaluated.add(id);
    if (!inputsOk || op === undefined) return undefined;
    validOps.set(id, op);
    const hash = computeOpMerkleHash(op, (inp) => hashes.get(inp)!);
    hashes.set(id, hash);
    return hash;
  }

  for (const id of Object.keys(index.nodes)) evalNode(id);

  const liveHashes = new Set<string>();
  for (const [id, op] of validOps) {
    if (op.kind !== 'scan') liveHashes.add(hashes.get(id)!);
  }
  return liveHashes;
}

// --- Query formatting ---

export function formatLogicalPlan(
  plan: LogicalPlan,
  dialect: SqlDialect = 'sql',
  options?: FormatOptions,
): FormatResult {
  return dialect === 'pipe'
    ? formatPipe(plan, options)
    : formatSql(plan, options);
}

export function buildDisplayQuery(
  plan: LogicalPlan | undefined,
  dialect: SqlDialect = 'sql',
): string | undefined {
  if (!plan || plan.nodes.length === 0) return undefined;
  const res = formatLogicalPlan(plan, dialect, {includeModuleHeaders: true});
  return res.ok ? res.sql : `-- Unsupported in Standard SQL: ${res.error}`;
}

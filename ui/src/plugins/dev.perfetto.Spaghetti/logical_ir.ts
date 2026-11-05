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

// ---------------------------------------------------------------------------
// Language-agnostic Logical IR for Spaghetti query graphs.
//
// Each node in the graph lowers its configuration + input port wiring into a
// declarative `LogicalOp`. Backends (Standard SQL, PerfettoSQL Next, etc.)
// consume a topologically sorted `LogicalPlan` to emit target queries and
// content-addressable materialization steps.
// ---------------------------------------------------------------------------

export const FILTER_OPS = [
  '=',
  '!=',
  '>',
  '>=',
  '<',
  '<=',
  'LIKE',
  'NOT LIKE',
  'GLOB',
  'IS NULL',
  'IS NOT NULL',
] as const;

export type FilterOp = (typeof FILTER_OPS)[number];

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
  // Empty `columns` keeps all upstream columns (`*`) and appends `expressions`.
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
  // Resolved remaining columns when upstream schema is known (used by
  // dialects such as SQLite SQL that lack `DROP` / `* EXCEPT`).
  readonly remainingColumns: readonly string[] | undefined;
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

export interface RawSqlOp {
  readonly kind: 'raw_sql';
  readonly sql: string;
  readonly inputs: readonly RawSqlInput[];
}

export type LogicalOp =
  | ScanOp
  | TimeRangeOp
  | PassthroughOp
  | FilterLogicalOp
  | ProjectOp
  | ExtendOp
  | ExtractArgOp
  | DropOp
  | AggregateOp
  | SortOp
  | LimitOp
  | JoinOp
  | UnionOp
  | IntervalIntersectOp
  | RawSqlOp;

export interface LogicalNode {
  readonly nodeId: string;
  readonly op: LogicalOp;
  // Number of downstream consumers within this plan.
  readonly refCount: number;
}

export interface LogicalPlan {
  // Nodes in topological order; the last element is the target node.
  readonly nodes: readonly LogicalNode[];
  readonly targetNodeId: string;
}

// Return all upstream node IDs referenced by `op` (with multiplicity so
// diamond / self-join consumers increment `refCount` per port).
export function getLogicalOpInputs(op: LogicalOp): readonly string[] {
  switch (op.kind) {
    case 'scan':
    case 'time_range':
      return [];
    case 'passthrough':
    case 'filter':
    case 'project':
    case 'extend':
    case 'extract_arg':
    case 'drop':
    case 'aggregate':
    case 'sort':
    case 'limit':
      return [op.input];
    case 'join':
      return [op.left, op.right];
    case 'union':
      return op.inputs;
    case 'interval_intersect':
      return op.inputs.map((i) => i.nodeId);
    case 'raw_sql':
      return op.inputs.map((i) => i.nodeId);
    default:
      return assertUnreachable(op);
  }
}

// --- Shared scalar predicate formatting ---

const UNARY_FILTER_OPS: ReadonlySet<FilterOp> = new Set([
  'IS NULL',
  'IS NOT NULL',
]);

export function isUnaryFilterOp(op: FilterOp): boolean {
  return UNARY_FILTER_OPS.has(op);
}

// Returns true if the value should be emitted as-is (numeric literal, SQL
// NULL keyword, or already quoted/parenthesised by the user).
function isRawValue(value: string): boolean {
  if (value === '') return true;
  if (/^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(value)) return true;
  if (/^null$/i.test(value)) return true;
  if (value.startsWith("'") || value.startsWith('"')) return true;
  if (value.startsWith('(')) return true;
  return false;
}

function quoteValue(value: string): string {
  if (isRawValue(value)) return value;
  return `'${value.replace(/'/g, "''")}'`;
}

export function formatFilterConditions(
  conditions: readonly LogicalFilterCondition[],
  conjunction: FilterConjunction = 'AND',
): string {
  const parts = conditions
    .filter((c) => c.column !== '')
    .map((c) => {
      if (UNARY_FILTER_OPS.has(c.op)) {
        return `${c.column} ${c.op}`;
      }
      return `${c.column} ${c.op} ${quoteValue(c.value)}`;
    });
  return parts.join(` ${conjunction} `);
}

export function formatExtractArgExpr(
  argSetIdCol: string,
  argName: string,
): string {
  const escaped = argName.replace(/'/g, "''");
  return `extract_arg(${argSetIdCol}, '${escaped}')`;
}

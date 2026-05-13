// Copyright (C) 2025 The Android Open Source Project
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

import protos from '../../protos';
import m from 'mithril';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {ColumnInfo} from './query_builder/column_info';
import {NodeIssues} from './query_builder/node_issues';
import {Trace} from '../../public/trace';
import {NodeDetailsAttrs} from './node_types';

let nodeCounter = 0;
export function nextNodeId(): string {
  return (nodeCounter++).toString();
}

/**
 * Ensures the node counter is higher than any existing ID.
 * Call this after deserializing nodes to prevent ID collisions.
 */
export function ensureCounterAbove(ids: string[]): void {
  for (const id of ids) {
    const numId = parseInt(id, 10);
    if (!isNaN(numId) && numId >= nodeCounter) {
      nodeCounter = numId + 1;
    }
  }
}

export enum NodeType {
  // Sources
  kTable = 'table',
  kSimpleSlices = 'simple_slices',
  kSqlSource = 'sql_source',
  kTimeRangeSource = 'time_range_source',

  // Single node operations
  kAggregation = 'aggregation',
  kModifyColumns = 'modify_columns',
  kAddColumns = 'add_columns',
  kFilterDuring = 'filter_during',
  kFilterIn = 'filter_in',
  kLimitAndOffset = 'limit_and_offset',
  kSort = 'sort',
  kFilter = 'filter',
  kCounterToIntervals = 'counter_to_intervals',

  // Multi node operations
  kIntervalIntersect = 'interval_intersect',
  kUnion = 'union',
  kJoin = 'join',
  kCreateSlices = 'create_slices',

  // Visualization
  kVisualisation = 'visualisation',

  // Dashboard
  kDashboard = 'dashboard',

  // Group (encapsulates a subgraph as a single node)
  kGroup = 'group',

  // Deprecated (kept for backward compatibility)
  kMerge = kJoin,
  kMetrics = 'metrics',
  kTraceSummary = 'trace_summary',
}

export function singleNodeOperation(type: NodeType): boolean {
  switch (type) {
    case NodeType.kAggregation:
    case NodeType.kModifyColumns:
    case NodeType.kAddColumns:
    case NodeType.kFilterDuring:
    case NodeType.kFilterIn:
    case NodeType.kLimitAndOffset:
    case NodeType.kSort:
    case NodeType.kFilter:
    case NodeType.kCounterToIntervals:
    case NodeType.kMetrics:
    case NodeType.kVisualisation:
    case NodeType.kDashboard:
      return true;
    default:
      return false;
  }
}

// Actions that can be performed by nodes on the parent graph.
// These are optional callbacks provided by the parent component.
export interface NodeActions {
  // Create and connect a table node to a target node's input port
  onAddAndConnectTable?: (tableName: string, portIndex: number) => void;
  // Insert a ModifyColumns node on an input at a specific port
  onInsertModifyColumnsNode?: (portIndex: number) => void;
  // Insert a CounterToIntervals node on an input at a specific port
  onInsertCounterToIntervalsNode?: (portIndex: number) => void;
}

// Runtime dependencies shared across nodes. Never serialized.
// Injected by the parent component (DataExplorer) after node creation.
export interface NodeContext {
  trace?: Trace;
  sqlModules?: SqlModules;
  onchange?: () => void;
  actions?: NodeActions;
  issues?: NodeIssues;
  getTableNameForNode?: (nodeId: string) => Promise<string | undefined>;
  requestNodeExecution?: (nodeId: string) => Promise<void>;
  hasOperationChanged?: boolean;
  autoExecute?: boolean;
}

// Specification for secondary inputs with clear cardinality requirements
export interface SecondaryInputSpec {
  // The actual connections (no undefined holes - indexed by port number)
  readonly connections: Map<number, QueryNode>;

  // Cardinality requirements for validation
  readonly min: number; // Minimum required (e.g., 2 for IntervalIntersect)
  readonly max: number | 'unbounded'; // Maximum allowed (e.g., 2 for Join, unbounded for IntervalIntersect)

  // Port names for UI display
  // Can be an array of names or a function that generates a name for a given port index
  readonly portNames: string[] | ((portIndex: number) => string);
}

export interface QueryNode {
  readonly nodeId: string;
  readonly type: NodeType;
  nextNodes: QueryNode[];

  // Columns that are available after applying all operations.
  readonly finalCols: ColumnInfo[];

  // Serializable node configuration. Always JSON-serializable.
  readonly attrs: object;

  // Runtime context (trace, callbacks, issues). Never serialized.
  readonly context: NodeContext;

  // Primary input from above (data flows vertically down)
  primaryInput?: QueryNode;

  // Secondary inputs from the side (horizontal connections)
  secondaryInputs?: SecondaryInputSpec;

  // Inner nodes encapsulated by this node (e.g. GroupNode).
  innerNodes?: QueryNode[];

  validate(): boolean;
  getTitle(): string;
  // Returns either NodeModifyAttrs (new structured pattern) or m.Child (legacy pattern)
  nodeSpecificModify(): unknown;
  nodeDetails(): NodeDetailsAttrs;
  nodeInfo(): m.Children;
  clone(): QueryNode;
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined;
  onPrevNodesUpdated?(): void;

  // Optional custom results panel for the bottom drawer panel.
  // If provided, Builder renders this instead of the standard ResultsPanel.
  customResultsPanel?(): m.Children;
}

export interface Query {
  sql: string;
  textproto: string;
  standaloneSql: string;
}

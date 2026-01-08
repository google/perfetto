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
import {SqlModules, SqlTable} from '../dev.perfetto.SqlModules/sql_modules';
import {ColumnInfo} from './query_builder/column_info';
import {UIFilter} from './query_builder/operations/filter';
import {NodeIssues} from './query_builder/node_issues';
import {Trace} from '../../public/trace';
import {NodeDetailsAttrs} from './query_builder/node_explorer_types';

let nodeCounter = 0;
export function nextNodeId(): string {
  return (nodeCounter++).toString();
}

export enum NodeType {
  // Sources
  kTable,
  kSimpleSlices,
  kSqlSource,
  kTimeRangeSource,

  // Single node operations
  kAggregation,
  kModifyColumns,
  kAddColumns,
  kFilterDuring,
  kLimitAndOffset,
  kSort,
  kFilter,

  // Multi node operations
  kIntervalIntersect,
  kUnion,
  kJoin,
  kCreateSlices,

  // Deprecated (kept for backward compatibility)
  kMerge = kJoin,
}

export function singleNodeOperation(type: NodeType): boolean {
  switch (type) {
    case NodeType.kAggregation:
    case NodeType.kModifyColumns:
    case NodeType.kAddColumns:
    case NodeType.kFilterDuring:
    case NodeType.kLimitAndOffset:
    case NodeType.kSort:
    case NodeType.kFilter:
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

// All information required to create a new node.
export interface QueryNodeState {
  trace?: Trace;
  sqlModules?: SqlModules;
  sqlTable?: SqlTable;

  // Operations
  // Filters can be partial during editing (similar to how Aggregation works)
  filters?: Partial<UIFilter>[];
  filterOperator?: 'AND' | 'OR'; // How to combine filters (default: AND)

  issues?: NodeIssues;

  onchange?: () => void;

  // Actions that can be performed on the parent graph
  actions?: NodeActions;

  // Caching
  hasOperationChanged?: boolean;

  // Whether queries should automatically execute when this node changes.
  // If false, the user must manually click "Run" to execute queries.
  // Set by the node registry when the node is created.
  autoExecute?: boolean;

  // Materialization state
  materialized?: boolean;
  materializationTableName?: string;
  // Hash of the query that was materialized (for detecting query changes)
  materializedQueryHash?: string;
}

export interface QueryNode {
  readonly nodeId: string;
  readonly type: NodeType;
  nextNodes: QueryNode[];

  // Columns that are available after applying all operations.
  readonly finalCols: ColumnInfo[];

  // State of the node. This is used to store the user's input and can be used
  // to fully recover the node.
  readonly state: QueryNodeState;

  // Primary input from above (data flows vertically down)
  // Used by single-input operations (Filter, Sort, Aggregation, etc.)
  primaryInput?: QueryNode;

  // Secondary inputs from the side (horizontal connections)
  // Used by multi-input operations (Union, Join, IntervalIntersect) and
  // for side joins (AddColumns)
  secondaryInputs?: SecondaryInputSpec;

  validate(): boolean;
  getTitle(): string;
  // Returns either NodeModifyAttrs (new structured pattern) or m.Child (legacy pattern)
  // NodeModifyAttrs allows nodes to declaratively specify sections and corner buttons,
  // while m.Child allows direct rendering for backwards compatibility
  nodeSpecificModify(): unknown;
  nodeDetails(): NodeDetailsAttrs;
  nodeInfo(): m.Children;
  clone(): QueryNode;
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined;
  serializeState(): object;
  onPrevNodesUpdated?(): void;
}

export interface Query {
  sql: string;
  textproto: string;
  modules: string[];
  preambles: string[];
  columns: string[];
}

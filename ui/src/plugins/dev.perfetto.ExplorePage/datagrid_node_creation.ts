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

import {Trace} from '../../public/trace';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {NodeActionHandlers} from './node_actions';
import {createNodeActions} from './node_actions';
import {QueryNode, NodeType} from './query_node';
import {UIFilter} from './query_builder/operations/filter';
import {FilterNode} from './query_builder/nodes/filter_node';
import {AddColumnsNode} from './query_builder/nodes/add_columns_node';
import {Column} from '../../components/widgets/datagrid/model';
import {nodeRegistry} from './query_builder/node_registry';
import {
  insertNodeBetween,
  addConnection,
  removeConnection,
} from './query_builder/graph_utils';
import {ExplorePageState} from './explore_page';

// Dependencies needed by datagrid-triggered node creation operations.
export interface DatagridNodeCreationDeps {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;
  readonly onStateUpdate: (
    update:
      | ExplorePageState
      | ((currentState: ExplorePageState) => ExplorePageState),
  ) => void;
  readonly initializedNodes: Set<string>;
  readonly nodeActionHandlers: NodeActionHandlers;
}

// Sets filters on a node and optionally sets the filter operator.
function setFiltersOnNode(
  node: QueryNode,
  filters: UIFilter[],
  filterOperator?: 'AND' | 'OR',
): void {
  node.state.filters = filters;
  if (filterOperator) {
    node.state.filterOperator = filterOperator;
  }
}

// Parses a column field to extract joinid information.
// Returns undefined if the field is not a joinid column reference.
export function parseJoinidColumnField(
  field: string,
  sourceNode: QueryNode,
):
  | {
      joinidColumnName: string;
      targetColumnName: string;
      targetTable: string;
      targetJoinColumn: string;
    }
  | undefined {
  // Parse the field to extract joinid column name and target column
  // Expected format: "joinidColumnName.targetColumnName"
  const dotIndex = field.indexOf('.');
  if (dotIndex === -1) {
    return undefined;
  }

  const joinidColumnName = field.substring(0, dotIndex);
  const targetColumnName = field.substring(dotIndex + 1);

  // Find the joinid column in the source node's finalCols
  const joinidColumnInfo = sourceNode.finalCols.find(
    (col) => col.name === joinidColumnName,
  );

  if (
    joinidColumnInfo === undefined ||
    joinidColumnInfo.column.type?.kind !== 'joinid'
  ) {
    return undefined;
  }

  return {
    joinidColumnName,
    targetColumnName,
    targetTable: joinidColumnInfo.column.type.source.table,
    targetJoinColumn: joinidColumnInfo.column.type.source.column,
  };
}

// Finds an AddColumnsNode that matches the given join configuration.
// Checks both the source node itself and its immediate child.
export function findMatchingAddColumnsNode(
  sourceNode: QueryNode,
  joinidColumnName: string,
  targetJoinColumn: string,
): AddColumnsNode | undefined {
  // Check if the source node is already an AddColumnsNode with the same join
  if (sourceNode.type === NodeType.kAddColumns) {
    const addColumnsNode = sourceNode as AddColumnsNode;
    if (
      addColumnsNode.state.leftColumn === joinidColumnName &&
      addColumnsNode.state.rightColumn === targetJoinColumn
    ) {
      return addColumnsNode;
    }
  }

  // Check if the source node has exactly one child that's an AddColumnsNode with same join
  if (
    sourceNode.nextNodes.length === 1 &&
    sourceNode.nextNodes[0].type === NodeType.kAddColumns
  ) {
    const existingAddColumnsNode = sourceNode.nextNodes[0] as AddColumnsNode;
    if (
      existingAddColumnsNode.state.leftColumn === joinidColumnName &&
      existingAddColumnsNode.state.rightColumn === targetJoinColumn
    ) {
      return existingAddColumnsNode;
    }
  }

  return undefined;
}

export async function addFilter(
  deps: DatagridNodeCreationDeps,
  sourceNode: QueryNode,
  filter: UIFilter | UIFilter[],
  filterOperator?: 'AND' | 'OR',
): Promise<void> {
  // Normalize to array for uniform handling (single filter → [filter])
  const filters: UIFilter[] = Array.isArray(filter) ? filter : [filter];

  // If the source node is already a FilterNode, just add the filter(s) to it
  if (sourceNode.type === NodeType.kFilter) {
    setFiltersOnNode(
      sourceNode,
      [...(sourceNode.state.filters ?? []), ...filters] as UIFilter[],
      filterOperator,
    );
    deps.onStateUpdate((currentState) => ({...currentState}));
    return;
  }

  // If the source node has exactly one child and it's a FilterNode, add to that
  if (
    sourceNode.nextNodes.length === 1 &&
    sourceNode.nextNodes[0].type === NodeType.kFilter
  ) {
    const existingFilterNode = sourceNode.nextNodes[0];
    setFiltersOnNode(
      existingFilterNode,
      [...(existingFilterNode.state.filters ?? []), ...filters] as UIFilter[],
      filterOperator,
    );
    deps.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNodes: new Set([existingFilterNode.nodeId]),
    }));
    return;
  }

  // Otherwise, create a new FilterNode after the source node
  // Create it with filters already configured to avoid multiple undo points
  const newFilterNode = new FilterNode({
    filters,
    filterOperator,
    sqlModules: deps.sqlModules,
  });

  // Mark as initialized
  deps.initializedNodes.add(newFilterNode.nodeId);

  // Insert between source node and its children
  insertNodeBetween(sourceNode, newFilterNode, addConnection, removeConnection);

  // Single state update records the entire operation (node + filters)
  deps.onStateUpdate((currentState) => ({
    ...currentState,
    selectedNodes: new Set([newFilterNode.nodeId]),
  }));
}

// Handles adding a column from a joinid table by creating an AddColumnsNode.
// The column field is expected to be in the format "joinidColumn.targetColumnName"
// where joinidColumn is a column with joinid type in the source node.
export function addColumnFromJoinid(
  deps: DatagridNodeCreationDeps,
  state: ExplorePageState,
  sourceNode: QueryNode,
  column: Column,
): void {
  const parsed = parseJoinidColumnField(column.field, sourceNode);
  if (parsed === undefined) {
    // Not a joinid column reference - nothing to do
    return;
  }

  const {joinidColumnName, targetColumnName, targetTable, targetJoinColumn} =
    parsed;

  // Check if this column name already exists in the source node's schema
  const existingColumnNames = new Set(
    sourceNode.finalCols.map((col) => col.name),
  );
  if (existingColumnNames.has(targetColumnName)) {
    console.warn(
      `Cannot add column: "${targetColumnName}" already exists in the schema`,
    );
    return;
  }

  // Try to find an existing AddColumnsNode with the same join configuration
  const existingNode = findMatchingAddColumnsNode(
    sourceNode,
    joinidColumnName,
    targetJoinColumn,
  );

  if (existingNode !== undefined) {
    // Check if the column is already added
    if (existingNode.state.selectedColumns?.includes(targetColumnName)) {
      console.warn(`Cannot add column: "${targetColumnName}" is already added`);
      return;
    }

    // Add the column to the existing AddColumnsNode
    existingNode.state.selectedColumns = [
      ...(existingNode.state.selectedColumns ?? []),
      targetColumnName,
    ];
    existingNode.state.onchange?.();
    if (existingNode !== sourceNode) {
      deps.onStateUpdate((currentState) => ({
        ...currentState,
        selectedNodes: new Set([existingNode.nodeId]),
      }));
    }
    return;
  }

  // Create a new AddColumnsNode with the join configuration
  // Note: selectedColumns is set after connecting the table node because
  // onPrevNodesUpdated() resets selectedColumns when rightNode is not connected
  const newAddColumnsNode = new AddColumnsNode({
    leftColumn: joinidColumnName,
    rightColumn: targetJoinColumn,
    isGuidedConnection: true,
    sqlModules: deps.sqlModules,
    trace: deps.trace,
  });

  // Set actions now that the node is created
  newAddColumnsNode.state.actions = createNodeActions(
    newAddColumnsNode,
    deps.nodeActionHandlers,
  );

  // Mark as initialized
  deps.initializedNodes.add(newAddColumnsNode.nodeId);

  // Insert between source node and its children
  insertNodeBetween(
    sourceNode,
    newAddColumnsNode,
    addConnection,
    removeConnection,
  );

  // Now create and connect the table source node
  const descriptor = nodeRegistry.get('table');
  if (descriptor === undefined) {
    console.warn("Cannot add table: 'table' node type not found in registry");
    return;
  }

  const sqlTable = deps.sqlModules
    .listTables()
    .find((t) => t.name === targetTable);
  if (sqlTable === undefined) {
    console.warn(`Table ${targetTable} not found in SQL modules`);
    return;
  }

  // Create the table node with the specific table
  const tableNode = descriptor.factory(
    {
      sqlTable,
      sqlModules: deps.sqlModules,
      trace: deps.trace,
    },
    {allNodes: state.rootNodes},
  );

  // Connect table node to AddColumnsNode's secondary input (port 0)
  addConnection(tableNode, newAddColumnsNode, 0);

  // Now that rightNode is connected, set the selected column
  // (must be done after connection because onPrevNodesUpdated resets it otherwise)
  newAddColumnsNode.state.selectedColumns = [targetColumnName];

  // Update state with both new nodes
  deps.onStateUpdate((currentState) => ({
    ...currentState,
    rootNodes: [...currentState.rootNodes, tableNode],
    selectedNodes: new Set([newAddColumnsNode.nodeId]),
  }));
}

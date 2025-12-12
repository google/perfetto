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

/**
 * Test utilities for the Explore Page query builder tests.
 *
 * This module provides common factories and helpers for creating mock nodes,
 * columns, and assertions used across all node unit tests.
 */

import {QueryNode, NodeType} from '../../query_node';
import {ColumnInfo} from '../column_info';
import {NodeDetailsAttrs} from '../node_explorer_types';
import {NodeIssues} from '../node_issues';
import protos from '../../../../protos';

// ============================================================================
// TYPES
// ============================================================================

/** Column type strings supported by the test utilities */
export type ColumnType =
  | 'int'
  | 'double'
  | 'boolean'
  | 'string'
  | 'bytes'
  | 'timestamp'
  | 'duration'
  | 'arg_set_id';

/** Options for creating a mock node */
export interface MockNodeOptions {
  /** Node ID (default: 'mock-node') */
  nodeId?: string;
  /** Node type (default: NodeType.kTable) */
  type?: NodeType;
  /** Columns for finalCols (default: empty array) */
  columns?: ColumnInfo[];
  /** Custom validate function (default: returns true) */
  validate?: () => boolean;
  /** Custom getTitle function (default: returns 'Mock Node') */
  getTitle?: () => string;
  /** Custom getStructuredQuery function (default: returns undefined) */
  getStructuredQuery?: () => protos.PerfettoSqlStructuredQuery | undefined;
  /** Custom state to merge with default state */
  state?: Partial<QueryNode['state']>;
}

/** Options for creating a column info */
export interface ColumnInfoOptions {
  /** Whether the column is checked (default: true) */
  checked?: boolean;
  /** Optional alias for the column */
  alias?: string;
}

// ============================================================================
// MOCK NODE FACTORY
// ============================================================================

/**
 * Creates a mock QueryNode for testing purposes.
 *
 * This is the primary factory for creating mock nodes in tests. It provides
 * sensible defaults while allowing customization of any aspect of the node.
 *
 * @example
 * // Simple mock node with default columns
 * const node = createMockNode();
 *
 * @example
 * // Mock node with custom columns
 * const node = createMockNode({
 *   nodeId: 'source',
 *   columns: [
 *     createColumnInfo('id', 'int'),
 *     createColumnInfo('name', 'string'),
 *   ],
 * });
 *
 * @example
 * // Mock node with custom validation
 * const invalidNode = createMockNode({
 *   validate: () => false,
 *   state: { issues: { queryError: new Error('test error') } },
 * });
 */
export function createMockNode(options: MockNodeOptions = {}): QueryNode {
  const {
    nodeId = 'mock-node',
    type = NodeType.kTable,
    columns = [],
    validate = () => true,
    getTitle = () => 'Mock Node',
    getStructuredQuery = () => undefined,
    state = {},
  } = options;

  const node: QueryNode = {
    nodeId,
    type,
    nextNodes: [],
    finalCols: columns,
    state: {...state},
    validate,
    getTitle,
    nodeSpecificModify: () => null,
    nodeDetails: (): NodeDetailsAttrs => ({content: null}),
    nodeInfo: () => null,
    clone: () => createMockNode(options),
    getStructuredQuery,
    serializeState: () => ({}),
  };

  return node;
}

/**
 * Creates a mock source node with standard table columns (id, name, value).
 *
 * This is a convenience function for the most common test scenario where
 * you need a source node with basic integer and string columns.
 *
 * @param nodeId - Optional node ID (default: 'source')
 */
export function createMockSourceNode(nodeId = 'source'): QueryNode {
  return createMockNode({
    nodeId,
    columns: STANDARD_TABLE_COLUMNS(),
  });
}

/**
 * Creates a mock node with interval columns (id, ts, dur).
 *
 * Useful for testing nodes that work with time intervals like
 * IntervalIntersect or CreateSlices.
 *
 * @param nodeId - Optional node ID (default: 'interval-source')
 */
export function createMockIntervalNode(nodeId = 'interval-source'): QueryNode {
  return createMockNode({
    nodeId,
    columns: INTERVAL_COLUMNS(),
  });
}

// ============================================================================
// COLUMN INFO FACTORIES
// ============================================================================

/**
 * Creates a ColumnInfo object for testing.
 *
 * @param name - Column name
 * @param type - Column type (e.g., 'int', 'string', 'timestamp')
 * @param options - Additional options (checked, alias)
 *
 * @example
 * const col = createColumnInfo('id', 'int');
 *
 * @example
 * const aliasedCol = createColumnInfo('name', 'string', { alias: 'full_name' });
 *
 * @example
 * const uncheckedCol = createColumnInfo('hidden', 'int', { checked: false });
 */
export function createColumnInfo(
  name: string,
  type: ColumnType,
  options: ColumnInfoOptions = {},
): ColumnInfo {
  const {checked = true, alias} = options;

  return {
    name,
    type: type.toUpperCase(),
    checked,
    column: {name, type: {kind: type}},
    alias,
  };
}

/**
 * Creates a ColumnInfo with type string format matching the perfetto SQL types.
 *
 * This is useful when you need the type string to match exactly what
 * perfettoSqlTypeToString returns (e.g., 'INT', 'STRING', 'TIMESTAMP').
 *
 * @param name - Column name
 * @param type - Column type in uppercase format
 * @param options - Additional options (checked, alias)
 */
export function createColumnInfoWithTypeString(
  name: string,
  type: string,
  options: ColumnInfoOptions = {},
): ColumnInfo {
  const {checked = true, alias} = options;
  const kind = type.toLowerCase() as ColumnType;

  return {
    name,
    type,
    checked,
    column: {name, type: {kind}},
    alias,
  };
}

// ============================================================================
// STANDARD COLUMN PRESETS
// ============================================================================

/**
 * Returns standard table columns: id (int), name (string), value (int).
 *
 * These are the most commonly used columns in tests for basic operations
 * like filtering, grouping, and column selection.
 */
export function STANDARD_TABLE_COLUMNS(): ColumnInfo[] {
  return [
    createColumnInfo('id', 'int'),
    createColumnInfo('name', 'string'),
    createColumnInfo('value', 'int'),
  ];
}

/**
 * Returns interval columns: id (int), ts (timestamp), dur (duration).
 *
 * These columns are used for testing interval-based operations like
 * IntervalIntersect, CreateSlices, and time-based filtering.
 */
export function INTERVAL_COLUMNS(): ColumnInfo[] {
  return [
    createColumnInfo('id', 'int'),
    createColumnInfo('ts', 'timestamp'),
    createColumnInfo('dur', 'duration'),
  ];
}

/**
 * Returns extended interval columns: id, ts, dur, name, track_id.
 *
 * These columns represent a typical slice table with all common fields.
 */
export function SLICE_COLUMNS(): ColumnInfo[] {
  return [
    createColumnInfo('id', 'int'),
    createColumnInfo('ts', 'timestamp'),
    createColumnInfo('dur', 'duration'),
    createColumnInfo('name', 'string'),
    createColumnInfo('track_id', 'int'),
  ];
}

// ============================================================================
// NODE CONNECTION HELPERS
// ============================================================================

/**
 * Connects a source node to a target node's primary input.
 *
 * This helper manages the bidirectional relationship between nodes:
 * - Adds target to source's nextNodes
 * - Sets source as target's primaryInput
 *
 * @param source - The node providing data (upstream)
 * @param target - The node receiving data (downstream)
 *
 * @example
 * const sourceNode = createMockSourceNode();
 * const filterNode = new FilterNode({ filters: [] });
 * connectNodes(sourceNode, filterNode);
 */
export function connectNodes(source: QueryNode, target: QueryNode): void {
  source.nextNodes.push(target);
  target.primaryInput = source;
}

/**
 * Connects a source node to a target node's secondary input at a specific port.
 *
 * Used for multi-input nodes like Union, Join, and IntervalIntersect.
 *
 * @param source - The node providing data
 * @param target - The multi-input node receiving data
 * @param port - The port index (default: 0)
 *
 * @example
 * const leftNode = createMockSourceNode('left');
 * const rightNode = createMockSourceNode('right');
 * const joinNode = new JoinNode({ ... });
 * connectSecondary(leftNode, joinNode, 0);
 * connectSecondary(rightNode, joinNode, 1);
 */
export function connectSecondary(
  source: QueryNode,
  target: QueryNode,
  port = 0,
): void {
  source.nextNodes.push(target);
  if (target.secondaryInputs) {
    target.secondaryInputs.connections.set(port, source);
  }
}

/**
 * Initializes a node chain by calling onPrevNodesUpdated on all nodes.
 *
 * This simulates the initialization that happens when nodes are added
 * to the graph in the actual application.
 *
 * @param nodes - Array of nodes in order from source to sink
 *
 * @example
 * const source = createMockSourceNode();
 * const modify = new ModifyColumnsNode({ selectedColumns: [] });
 * const filter = new FilterNode({ filters: [] });
 * connectNodes(source, modify);
 * connectNodes(modify, filter);
 * initializeNodeChain([modify, filter]);
 */
export function initializeNodeChain(nodes: QueryNode[]): void {
  for (const node of nodes) {
    node.onPrevNodesUpdated?.();
  }
}

// ============================================================================
// ASSERTION HELPERS
// ============================================================================

/**
 * Asserts that a node's finalCols have the expected column names.
 *
 * @param node - The node to check
 * @param expectedNames - Array of expected column names in order
 *
 * @example
 * expectColumnNames(modifyNode, ['id', 'renamed_column', 'value']);
 */
export function expectColumnNames(
  node: QueryNode,
  expectedNames: string[],
): void {
  const actualNames = node.finalCols.map((c) => c.name);
  expect(actualNames).toEqual(expectedNames);
}

/**
 * Asserts that a node's validation fails with a specific queryError message.
 *
 * Note: This specifically checks for queryError, which is what validate()
 * methods set via setValidationError(). Other error types (responseError,
 * dataError, executionError) are set by different code paths (query execution).
 *
 * @param node - The node to validate
 * @param expectedMessage - Substring expected in the queryError message
 *
 * @example
 * expectValidationError(joinNode, 'exactly two sources');
 */
export function expectValidationError(
  node: QueryNode,
  expectedMessage: string,
): void {
  expect(node.validate()).toBe(false);
  expect(node.state.issues?.queryError?.message).toContain(expectedMessage);
}

/**
 * Asserts that a node validates successfully with no issues.
 *
 * Checks that validate() returns true and that no issues exist
 * (queryError, responseError, dataError, executionError, warnings).
 *
 * @param node - The node to validate
 */
export function expectValidationSuccess(node: QueryNode): void {
  expect(node.validate()).toBe(true);
  expect(node.state.issues?.hasIssues() ?? false).toBe(false);
}

/**
 * Asserts that a node has no issues of any type.
 *
 * @param node - The node to check
 */
export function expectNoIssues(node: QueryNode): void {
  expect(node.state.issues?.hasIssues() ?? false).toBe(false);
}

// ============================================================================
// NODE ISSUES HELPERS
// ============================================================================

/**
 * Creates a NodeIssues instance with a query error.
 *
 * Use this for testing validation failures. For other error types,
 * use createNodeIssues() with appropriate options.
 *
 * @param message - The error message
 */
export function createNodeIssuesWithQueryError(message: string): NodeIssues {
  const issues = new NodeIssues();
  issues.queryError = new Error(message);
  return issues;
}

/** Options for creating NodeIssues */
export interface NodeIssuesOptions {
  queryError?: string;
  responseError?: string;
  dataError?: string;
  executionError?: string;
  warnings?: string[];
}

/**
 * Creates a NodeIssues instance with specified errors.
 *
 * @param options - Error messages for each error type
 *
 * @example
 * // Create issues with multiple error types
 * const issues = createNodeIssues({
 *   queryError: 'Invalid query',
 *   warnings: ['Column may be slow', 'Missing index'],
 * });
 */
export function createNodeIssues(options: NodeIssuesOptions = {}): NodeIssues {
  const issues = new NodeIssues();
  if (options.queryError) {
    issues.queryError = new Error(options.queryError);
  }
  if (options.responseError) {
    issues.responseError = new Error(options.responseError);
  }
  if (options.dataError) {
    issues.dataError = new Error(options.dataError);
  }
  if (options.executionError) {
    issues.executionError = new Error(options.executionError);
  }
  if (options.warnings) {
    issues.warnings = options.warnings.map((msg) => new Error(msg));
  }
  return issues;
}

// ============================================================================
// STRUCTURED QUERY HELPERS
// ============================================================================

/**
 * Creates a mock structured query for testing getStructuredQuery implementations.
 *
 * @param id - Optional ID for the structured query
 */
export function createMockStructuredQuery(
  id?: string,
): protos.PerfettoSqlStructuredQuery {
  const sq = new protos.PerfettoSqlStructuredQuery();
  if (id) {
    sq.id = id;
  }
  return sq;
}

/**
 * Creates a mock node that returns a structured query.
 *
 * Useful for testing nodes that depend on upstream nodes having
 * valid structured queries.
 *
 * @param nodeId - Node ID
 * @param columns - Columns for the node
 */
export function createMockNodeWithStructuredQuery(
  nodeId: string,
  columns: ColumnInfo[],
): QueryNode {
  const sq = createMockStructuredQuery(nodeId);
  return createMockNode({
    nodeId,
    columns,
    getStructuredQuery: () => sq,
  });
}

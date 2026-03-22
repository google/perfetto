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

import protos from '../../protos';
import {traceSummarySpecToPb} from '../../base/proto_utils_wasm';
import {DataExplorerState} from './data_explorer';
import {
  SerializedGraph,
  SerializedNode,
  deserializeState,
} from './json_handler';
import {NodeType, QueryNode, singleNodeOperation} from './query_node';
import {Trace} from '../../public/trace';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

// Counter for generating unique node IDs during import.
let importNodeCounter = 0;

function nextImportNodeId(): string {
  return `import_${++importNodeCounter}`;
}

// Layout constants for positioning imported graph nodes.
const NODE_GAP_Y = 80;
const METRIC_CHAIN_GAP_X = 350;
const START_X = 50;
const START_Y = 50;

// Accumulator for building up decompose results incrementally.
interface ResultAccumulator {
  nodes: SerializedNode[];
  rootNodeIds: string[];
  layouts: Map<string, {x: number; y: number}>;
}

// Merges a child result into a parent accumulator.
function mergeResult(
  target: ResultAccumulator,
  source: ResultAccumulator,
): void {
  target.nodes.push(...source.nodes);
  target.rootNodeIds.push(...source.rootNodeIds);
  for (const [id, pos] of source.layouts) {
    target.layouts.set(id, pos);
  }
}

// Appends a new node to a chain: wires the previous node's nextNodes,
// adds the node, sets its layout, and returns the new last node ID.
function chainNode(
  acc: ResultAccumulator,
  lastNodeId: string,
  nodeId: string,
  type: NodeType,
  state: Record<string, unknown>,
  x: number,
  y: number,
): void {
  const prevNode = acc.nodes.find((n) => n.nodeId === lastNodeId);
  if (prevNode !== undefined) {
    prevNode.nextNodes.push(nodeId);
  }
  acc.nodes.push({nodeId, type, state, nextNodes: []});
  // Dockable nodes (modifications, metrics, etc.) must NOT have a layout
  // position — the graph renderer docks them to their parent only when
  // they have no entry in nodeLayouts.
  if (!singleNodeOperation(type)) {
    acc.layouts.set(nodeId, {x, y});
  }
}

/**
 * Parses a pbtxt string (TraceSummarySpec or single metric_template_spec)
 * and converts it into a DataExplorerState with proper graph nodes.
 */
export async function parsePbtxtToState(
  pbtxtText: string,
  trace: Trace,
  sqlModules: SqlModules,
): Promise<DataExplorerState> {
  // Detect if this is a single metric_template_spec (no wrapping)
  // vs a full TraceSummarySpec.
  const wrappedText = detectAndWrapPbtxt(pbtxtText);

  const pbResult = await traceSummarySpecToPb(wrappedText);
  if (!pbResult.ok) {
    throw new Error(`Failed to parse pbtxt: ${pbResult.error}`);
  }

  const spec = protos.TraceSummarySpec.decode(pbResult.value);
  const allTemplateSpecs = spec.metricTemplateSpec ?? [];
  const allMetricSpecs = spec.metricSpec ?? [];

  // Build a map of shared queries from the top-level `query` repeated field.
  const sharedQueries = new Map<string, protos.IPerfettoSqlStructuredQuery>();
  for (const q of spec.query ?? []) {
    if (q.id !== undefined && q.id !== null && q.id !== '') {
      sharedQueries.set(q.id, q);
    }
  }

  if (allTemplateSpecs.length === 0 && allMetricSpecs.length === 0) {
    throw new Error(
      'No metric_template_spec or metric_spec found in the pbtxt file.',
    );
  }

  // Reset the import node counter for each import.
  importNodeCounter = 0;

  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };

  // Process metric_template_specs.
  for (let i = 0; i < allTemplateSpecs.length; i++) {
    const chainX = START_X + i * METRIC_CHAIN_GAP_X;
    mergeResult(
      acc,
      buildNodesFromTemplateSpec(
        allTemplateSpecs[i],
        chainX,
        sharedQueries,
        sqlModules,
      ),
    );
  }

  // Process metric_specs (simpler, single-value metrics).
  const metricSpecOffset = allTemplateSpecs.length;
  for (let i = 0; i < allMetricSpecs.length; i++) {
    const chainX = START_X + (metricSpecOffset + i) * METRIC_CHAIN_GAP_X;
    mergeResult(
      acc,
      buildNodesFromMetricSpec(
        allMetricSpecs[i],
        chainX,
        sharedQueries,
        sqlModules,
      ),
    );
  }

  // If there are multiple metrics, create a TraceSummaryNode.
  const metricsNodeIds = acc.nodes
    .filter((n) => n.type === NodeType.kMetrics)
    .map((n) => n.nodeId);

  if (metricsNodeIds.length > 1) {
    const traceSummaryId = nextImportNodeId();
    const maxX = Math.max(...[...acc.layouts.values()].map((p) => p.x));
    const maxY = Math.max(...[...acc.layouts.values()].map((p) => p.y));
    acc.nodes.push({
      nodeId: traceSummaryId,
      type: NodeType.kTraceSummary,
      state: {
        secondaryInputNodeIds: metricsNodeIds,
      },
      nextNodes: [],
    });
    acc.layouts.set(traceSummaryId, {
      x: maxX / 2,
      y: maxY + NODE_GAP_Y,
    });
    acc.rootNodeIds.push(traceSummaryId);
  }

  const serializedGraph: SerializedGraph = {
    nodes: acc.nodes,
    rootNodeIds: acc.rootNodeIds,
    nodeLayouts: Object.fromEntries(acc.layouts),
  };

  const json = JSON.stringify(serializedGraph);
  const state = deserializeState(json, trace, sqlModules);
  applyPostDeserializationFixups(state);
  return state;
}

// Post-deserialization fixups for pbtxt import:
//
// 1. Join nodes: check all left/right columns. postDeserializeLate defaults
//    new columns to unchecked (designed for interactive use). For import we
//    want all columns passed through; the downstream ModifyColumns handles
//    column selection.
//
// 2. Simple modification nodes (filter, aggregation, sort, limit): call
//    onPrevNodesUpdated so they resolve column info from the upstream chain.
//    pbtxt doesn't carry column types like JSON export does.
//
// Skip onPrevNodesUpdated for Join/Metrics/Dashboard (already called by
// postDeserializeLate) and ModifyColumns (state from deserialization is
// correct; rebuilding from upstream would introduce duplicates).
export function applyPostDeserializationFixups(state: DataExplorerState): void {
  const visited = new Set<string>();
  const skipNotify = new Set([
    NodeType.kJoin,
    NodeType.kMetrics,
    NodeType.kTraceSummary,
    NodeType.kModifyColumns,
  ]);
  function walkAndFixup(node: QueryNode): void {
    if (visited.has(node.nodeId)) return;
    visited.add(node.nodeId);

    // Check only the Join columns that the downstream ModifyColumns needs,
    // avoiding name collisions (e.g. utid on both left and right).
    if (node.type === NodeType.kJoin) {
      const joinState = node.state as {
        leftColumns?: Array<{checked: boolean; name: string}>;
        rightColumns?: Array<{checked: boolean; name: string}>;
      };
      // Find the downstream ModifyColumns to know which columns are needed.
      const mc = node.nextNodes.find((n) => n.type === NodeType.kModifyColumns);
      if (mc !== undefined) {
        const needed = new Set(
          (
            mc.state as {
              selectedColumns?: Array<{name: string; checked: boolean}>;
            }
          ).selectedColumns
            ?.filter((c) => c.checked)
            .map((c) => c.name) ?? [],
        );
        const checked = new Set<string>();
        // Check left columns first, then right — first match wins.
        for (const col of joinState.leftColumns ?? []) {
          if (needed.has(col.name) && !checked.has(col.name)) {
            col.checked = true;
            checked.add(col.name);
          }
        }
        for (const col of joinState.rightColumns ?? []) {
          if (needed.has(col.name) && !checked.has(col.name)) {
            col.checked = true;
            checked.add(col.name);
          }
        }
      } else {
        // No downstream ModifyColumns — check all columns.
        for (const col of joinState.leftColumns ?? []) col.checked = true;
        for (const col of joinState.rightColumns ?? []) col.checked = true;
      }
    }

    for (const next of node.nextNodes) {
      if (!skipNotify.has(next.type)) {
        next.onPrevNodesUpdated?.();
      }
      walkAndFixup(next);
    }
  }
  for (const root of state.rootNodes) {
    walkAndFixup(root);
  }

  // Second pass: notify Metrics/TraceSummary nodes AFTER Join columns are
  // checked. During postDeserializeLate the Join columns were not yet checked,
  // so Metrics saw empty upstream columns.
  const visited2 = new Set<string>();
  function notifyMetrics(node: QueryNode): void {
    if (visited2.has(node.nodeId)) return;
    visited2.add(node.nodeId);
    if (
      node.type === NodeType.kMetrics ||
      node.type === NodeType.kTraceSummary
    ) {
      node.onPrevNodesUpdated?.();
    }
    for (const next of node.nextNodes) notifyMetrics(next);
  }
  for (const root of state.rootNodes) notifyMetrics(root);
}

interface BuildResult {
  nodes: SerializedNode[];
  rootNodeIds: string[];
  layouts: Map<string, {x: number; y: number}>;
  /** The final node ID in the chain (the MetricsNode). */
  finalNodeId: string;
}

/**
 * Builds graph nodes from a TraceMetricV2TemplateSpec.
 * Creates: source node → [modification nodes] → MetricsNode
 */
export function buildNodesFromTemplateSpec(
  spec: protos.ITraceMetricV2TemplateSpec,
  startX: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): BuildResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let currentY = START_Y;

  // 1. Decompose the structured query into source + modification nodes.
  const query = spec.query;
  let lastNodeId: string;
  if (query !== undefined && query !== null) {
    const queryResult = decomposeStructuredQuery(
      query,
      startX,
      currentY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, queryResult);
    lastNodeId = queryResult.finalNodeId;
    currentY = queryResult.nextY;
  } else {
    const sqlId = nextImportNodeId();
    acc.nodes.push({
      nodeId: sqlId,
      type: NodeType.kSqlSource,
      state: {sql: '-- No query specified in pbtxt'},
      nextNodes: [],
    });
    acc.rootNodeIds.push(sqlId);
    acc.layouts.set(sqlId, {x: startX, y: currentY});
    lastNodeId = sqlId;
    currentY += NODE_GAP_Y;
  }

  // 2. Create the MetricsNode.
  const metricsId = nextImportNodeId();
  const metricsState = templateSpecToMetricsState(spec);
  metricsState.primaryInputId = lastNodeId;
  chainNode(
    acc,
    lastNodeId,
    metricsId,
    NodeType.kMetrics,
    metricsState,
    startX,
    currentY,
  );

  return {...acc, finalNodeId: metricsId};
}

/**
 * Builds graph nodes from a TraceMetricV2Spec (simple metric spec).
 * Similar to template spec but with a single value column.
 */
function buildNodesFromMetricSpec(
  spec: protos.ITraceMetricV2Spec,
  startX: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): BuildResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let currentY = START_Y;

  // 1. Decompose the structured query.
  const query = spec.query;
  let lastNodeId: string;
  if (query !== undefined && query !== null) {
    const queryResult = decomposeStructuredQuery(
      query,
      startX,
      currentY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, queryResult);
    lastNodeId = queryResult.finalNodeId;
    currentY = queryResult.nextY;
  } else {
    const sqlId = nextImportNodeId();
    acc.nodes.push({
      nodeId: sqlId,
      type: NodeType.kSqlSource,
      state: {sql: '-- No query specified in pbtxt'},
      nextNodes: [],
    });
    acc.rootNodeIds.push(sqlId);
    acc.layouts.set(sqlId, {x: startX, y: currentY});
    lastNodeId = sqlId;
    currentY += NODE_GAP_Y;
  }

  // 2. Create the MetricsNode.
  const metricsId = nextImportNodeId();
  const metricsState = metricSpecToMetricsState(spec);
  metricsState.primaryInputId = lastNodeId;
  chainNode(
    acc,
    lastNodeId,
    metricsId,
    NodeType.kMetrics,
    metricsState,
    startX,
    currentY,
  );

  return {...acc, finalNodeId: metricsId};
}

// ============================================================================
// Structured Query Decomposition
// ============================================================================

export interface DecomposeResult {
  nodes: SerializedNode[];
  rootNodeIds: string[];
  layouts: Map<string, {x: number; y: number}>;
  finalNodeId: string;
  nextY: number;
}

/**
 * Recursively decomposes a PerfettoSqlStructuredQuery into graph nodes.
 * Returns source nodes at the root and modification nodes chained on top.
 */
export function decomposeStructuredQuery(
  sq: protos.IPerfettoSqlStructuredQuery,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let currentY = startY;

  // Helper to add a leaf source node (no children to decompose).
  function addSourceNode(
    type: NodeType,
    state: Record<string, unknown>,
  ): string {
    const id = nextImportNodeId();
    acc.nodes.push({nodeId: id, type, state, nextNodes: []});
    acc.rootNodeIds.push(id);
    acc.layouts.set(id, {x: startX, y: currentY});
    currentY += NODE_GAP_Y;
    return id;
  }

  // Helper to merge a recursive decomposition result and advance currentY.
  function mergeSubQuery(subResult: DecomposeResult): string {
    mergeResult(acc, subResult);
    currentY = subResult.nextY;
    return subResult.finalNodeId;
  }

  // 1. Process the source.
  let sourceNodeId: string;

  if (sq.table !== undefined && sq.table !== null) {
    const tableName = sq.table.tableName || undefined;
    const moduleName = sq.table.moduleName || undefined;

    // The SimpleSlices source node uses thread_or_process_slice internally.
    // Map it back to a kSimpleSlices node for a faithful round-trip.
    // The module may appear in moduleName or in referencedModules.
    const allModules = [moduleName, ...(sq.referencedModules ?? [])].filter(
      (m) => m !== undefined && m !== '',
    );
    if (
      tableName === 'thread_or_process_slice' &&
      allModules.includes('slices.with_context')
    ) {
      sourceNodeId = addSourceNode(NodeType.kSimpleSlices, {});
    } else if (
      sqlModules === undefined ||
      (tableName !== undefined && sqlModules.getTable(tableName) !== undefined)
    ) {
      sourceNodeId = addSourceNode(NodeType.kTable, {
        sqlTable: tableName,
        moduleName,
      });
    } else {
      const parts: string[] = [];
      if (moduleName !== undefined && moduleName !== '') {
        parts.push(`INCLUDE PERFETTO MODULE ${moduleName};`);
      }
      parts.push(`SELECT * FROM ${tableName ?? 'unknown_table'}`);
      sourceNodeId = addSourceNode(NodeType.kSqlSource, {
        sql: parts.join('\n'),
      });
    }
  } else if (sq.simpleSlices !== undefined && sq.simpleSlices !== null) {
    sourceNodeId = addSourceNode(NodeType.kSimpleSlices, {});

    // SimpleSlices glob fields become GLOB filters.
    const globFilters = buildSimpleSlicesFilters(sq.simpleSlices);
    if (globFilters.length > 0) {
      const filterId = nextImportNodeId();
      const filterState: Record<string, unknown> = {
        primaryInputId: sourceNodeId,
        filters: globFilters,
      };
      chainNode(
        acc,
        sourceNodeId,
        filterId,
        NodeType.kFilter,
        filterState,
        startX,
        currentY,
      );
      sourceNodeId = filterId;
      currentY += NODE_GAP_Y;
    }
  } else if (sq.sql !== undefined && sq.sql !== null) {
    sourceNodeId = addSourceNode(NodeType.kSqlSource, {sql: sq.sql.sql ?? ''});
  } else if (sq.innerQuery !== undefined && sq.innerQuery !== null) {
    sourceNodeId = mergeSubQuery(
      decomposeStructuredQuery(
        sq.innerQuery,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.experimentalTimeRange !== undefined &&
    sq.experimentalTimeRange !== null
  ) {
    sourceNodeId = addSourceNode(NodeType.kTimeRangeSource, {
      start: sq.experimentalTimeRange.ts?.toString(),
      end: undefined,
      isDynamic: false,
    });
  } else if (
    sq.intervalIntersect !== undefined &&
    sq.intervalIntersect !== null
  ) {
    sourceNodeId = mergeSubQuery(
      decomposeIntervalIntersect(
        sq.intervalIntersect,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.experimentalJoin !== undefined &&
    sq.experimentalJoin !== null
  ) {
    sourceNodeId = mergeSubQuery(
      decomposeJoin(
        sq.experimentalJoin,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.experimentalUnion !== undefined &&
    sq.experimentalUnion !== null
  ) {
    sourceNodeId = mergeSubQuery(
      decomposeUnion(
        sq.experimentalUnion,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.experimentalAddColumns !== undefined &&
    sq.experimentalAddColumns !== null
  ) {
    sourceNodeId = mergeSubQuery(
      decomposeAddColumns(
        sq.experimentalAddColumns,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.experimentalFilterToIntervals !== undefined &&
    sq.experimentalFilterToIntervals !== null
  ) {
    sourceNodeId = mergeSubQuery(
      decomposeFilterToIntervals(
        sq.experimentalFilterToIntervals,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.experimentalCreateSlices !== undefined &&
    sq.experimentalCreateSlices !== null
  ) {
    sourceNodeId = mergeSubQuery(
      decomposeCreateSlices(
        sq.experimentalCreateSlices,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.experimentalCounterIntervals !== undefined &&
    sq.experimentalCounterIntervals !== null
  ) {
    sourceNodeId = mergeSubQuery(
      decomposeCounterIntervals(
        sq.experimentalCounterIntervals,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.experimentalFilterIn !== undefined &&
    sq.experimentalFilterIn !== null
  ) {
    sourceNodeId = mergeSubQuery(
      decomposeFilterIn(
        sq.experimentalFilterIn,
        startX,
        currentY,
        sharedQueries,
        sqlModules,
      ),
    );
  } else if (
    sq.innerQueryId !== undefined &&
    sq.innerQueryId !== null &&
    sq.innerQueryId !== ''
  ) {
    const sharedQuery = sharedQueries?.get(sq.innerQueryId);
    if (sharedQuery !== undefined) {
      sourceNodeId = mergeSubQuery(
        decomposeStructuredQuery(
          sharedQuery,
          startX,
          currentY,
          sharedQueries,
          sqlModules,
        ),
      );
    } else {
      sourceNodeId = addSourceNode(NodeType.kSqlSource, {
        sql: '-- Imported from pbtxt (unsupported source type)',
      });
    }
  } else {
    sourceNodeId = addSourceNode(NodeType.kSqlSource, {
      sql: '-- Imported from pbtxt (unsupported source type)',
    });
  }

  // 2. Apply operations as modification nodes.
  let lastNodeId = sourceNodeId;

  // Helper to chain a modification node onto the current chain.
  function chainModification(
    type: NodeType,
    state: Record<string, unknown>,
  ): void {
    const id = nextImportNodeId();
    state.primaryInputId = lastNodeId;
    chainNode(acc, lastNodeId, id, type, state, startX, currentY);
    lastNodeId = id;
    currentY += NODE_GAP_Y;
  }

  // Filters. Each proto filter may have multiple RHS values (IN semantics).
  // Filters with single values are grouped into one AND FilterNode.
  // Filters with multiple values each get their own OR FilterNode (chained
  // with AND between them).
  const filters = sq.filters ?? [];
  if (filters.length > 0) {
    const singleValueFilters: protos.PerfettoSqlStructuredQuery.IFilter[] = [];
    const multiValueFilters: protos.PerfettoSqlStructuredQuery.IFilter[] = [];
    for (const f of filters) {
      if (protoFilterRhsCount(f) > 1) {
        multiValueFilters.push(f);
      } else {
        singleValueFilters.push(f);
      }
    }

    if (singleValueFilters.length > 0) {
      chainModification(
        NodeType.kFilter,
        protoFiltersToFilterState(singleValueFilters),
      );
    }

    for (const f of multiValueFilters) {
      chainModification(NodeType.kFilter, protoFilterToExpandedOrState(f));
    }
  }

  // Group by / aggregation.
  if (sq.groupBy !== undefined && sq.groupBy !== null) {
    chainModification(
      NodeType.kAggregation,
      protoGroupByToAggregationState(sq.groupBy),
    );
  }

  // Select columns (modify columns).
  // Skip if the select just re-lists the group_by output (redundant).
  const selectCols = sq.selectColumns ?? [];
  if (selectCols.length > 0 && !isSelectRedundantAfterGroupBy(sq)) {
    chainModification(
      NodeType.kModifyColumns,
      protoSelectColumnsToModifyState(selectCols),
    );
  }

  // Order by (sort).
  if (sq.orderBy !== undefined && sq.orderBy !== null) {
    chainModification(NodeType.kSort, protoOrderByToSortState(sq.orderBy));
  }

  // Limit / offset. Protobufjs with --force-number sets unset numeric
  // fields to 0, so treat 0 as "unset" to avoid creating spurious nodes.
  const limitVal =
    sq.limit !== undefined && sq.limit !== null ? Number(sq.limit) : 0;
  const offsetVal =
    sq.offset !== undefined && sq.offset !== null ? Number(sq.offset) : 0;
  if (limitVal > 0 || offsetVal > 0) {
    const limitState: Record<string, unknown> = {};
    if (limitVal > 0) {
      limitState.limit = limitVal;
    }
    if (offsetVal > 0) {
      limitState.offset = offsetVal;
    }
    chainModification(NodeType.kLimitAndOffset, limitState);
  }

  // Experimental filter group.
  if (
    sq.experimentalFilterGroup !== undefined &&
    sq.experimentalFilterGroup !== null
  ) {
    const filterGroupResult = decomposeFilterGroup(
      sq.experimentalFilterGroup,
      lastNodeId,
    );
    for (const filterNode of filterGroupResult) {
      const prevNode = acc.nodes.find((n) => n.nodeId === lastNodeId);
      if (prevNode !== undefined) {
        prevNode.nextNodes.push(filterNode.nodeId);
      }
      acc.nodes.push(filterNode);
      // Don't set layout for dockable nodes — they dock to their parent.
      if (!singleNodeOperation(filterNode.type)) {
        acc.layouts.set(filterNode.nodeId, {x: startX, y: currentY});
      }
      lastNodeId = filterNode.nodeId;
      currentY += NODE_GAP_Y;
    }
  }

  return {...acc, finalNodeId: lastNodeId, nextY: currentY};
}

// ============================================================================
// Interval Intersect Decomposition
// ============================================================================

function decomposeIntervalIntersect(
  ii: protos.PerfettoSqlStructuredQuery.IIntervalIntersect,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  const inputNodeIds: string[] = [];
  let maxY = startY;

  if (ii.base !== undefined && ii.base !== null) {
    const baseResult = decomposeStructuredQuery(
      ii.base,
      startX,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, baseResult);
    inputNodeIds.push(baseResult.finalNodeId);
    maxY = Math.max(maxY, baseResult.nextY);
  }

  for (let i = 0; i < (ii.intervalIntersect ?? []).length; i++) {
    const secResult = decomposeStructuredQuery(
      (ii.intervalIntersect ?? [])[i],
      startX + (i + 1) * METRIC_CHAIN_GAP_X,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, secResult);
    inputNodeIds.push(secResult.finalNodeId);
    maxY = Math.max(maxY, secResult.nextY);
  }

  const iiId = nextImportNodeId();
  acc.nodes.push({
    nodeId: iiId,
    type: NodeType.kIntervalIntersect,
    state: {inputNodeIds},
    nextNodes: [],
    inputNodeIds,
  });

  for (const inputId of inputNodeIds) {
    const inputNode = acc.nodes.find((n) => n.nodeId === inputId);
    if (inputNode !== undefined) inputNode.nextNodes.push(iiId);
  }

  acc.layouts.set(iiId, {x: startX + METRIC_CHAIN_GAP_X / 2, y: maxY});
  acc.rootNodeIds.push(iiId);

  return {...acc, finalNodeId: iiId, nextY: maxY + NODE_GAP_Y};
}

// ============================================================================
// Join Decomposition
// ============================================================================

function decomposeJoin(
  join: protos.PerfettoSqlStructuredQuery.IExperimentalJoin,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let maxY = startY;

  let leftNodeId: string | undefined;
  if (join.leftQuery !== undefined && join.leftQuery !== null) {
    const r = decomposeStructuredQuery(
      join.leftQuery,
      startX,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    leftNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  let rightNodeId: string | undefined;
  if (join.rightQuery !== undefined && join.rightQuery !== null) {
    const r = decomposeStructuredQuery(
      join.rightQuery,
      startX + METRIC_CHAIN_GAP_X,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    rightNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  let joinType = 'INNER';
  if (
    join.type === protos.PerfettoSqlStructuredQuery.ExperimentalJoin.Type.LEFT
  ) {
    joinType = 'LEFT';
  }

  let conditionType: 'equality' | 'freeform' = 'equality';
  let leftColumn = '';
  let rightColumn = '';
  let sqlExpression = '';
  if (join.equalityColumns !== undefined && join.equalityColumns !== null) {
    leftColumn = join.equalityColumns.leftColumn ?? '';
    rightColumn = join.equalityColumns.rightColumn ?? '';
  } else if (
    join.freeformCondition !== undefined &&
    join.freeformCondition !== null
  ) {
    conditionType = 'freeform';
    sqlExpression = join.freeformCondition.sqlExpression ?? '';
  }

  const joinId = nextImportNodeId();
  acc.nodes.push({
    nodeId: joinId,
    type: NodeType.kJoin,
    state: {
      leftNodeId,
      rightNodeId,
      leftQueryAlias: 'left',
      rightQueryAlias: 'right',
      conditionType,
      joinType,
      leftColumn,
      rightColumn,
      sqlExpression,
    },
    nextNodes: [],
  });

  for (const id of [leftNodeId, rightNodeId]) {
    if (id !== undefined) {
      const node = acc.nodes.find((n) => n.nodeId === id);
      if (node !== undefined) node.nextNodes.push(joinId);
    }
  }

  acc.layouts.set(joinId, {x: startX + METRIC_CHAIN_GAP_X / 2, y: maxY});
  acc.rootNodeIds.push(joinId);

  return {...acc, finalNodeId: joinId, nextY: maxY + NODE_GAP_Y};
}

// ============================================================================
// Union Decomposition
// ============================================================================

function decomposeUnion(
  union: protos.PerfettoSqlStructuredQuery.IExperimentalUnion,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let maxY = startY;
  const inputNodeIds: string[] = [];
  const queries = union.queries ?? [];

  for (let i = 0; i < queries.length; i++) {
    const r = decomposeStructuredQuery(
      queries[i],
      startX + i * METRIC_CHAIN_GAP_X,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    inputNodeIds.push(r.finalNodeId);
    maxY = Math.max(maxY, r.nextY);
  }

  const unionId = nextImportNodeId();
  acc.nodes.push({
    nodeId: unionId,
    type: NodeType.kUnion,
    state: {unionNodes: inputNodeIds, selectedColumns: []},
    nextNodes: [],
    inputNodeIds,
  });

  for (const inputId of inputNodeIds) {
    const inputNode = acc.nodes.find((n) => n.nodeId === inputId);
    if (inputNode !== undefined) inputNode.nextNodes.push(unionId);
  }

  const centerX =
    queries.length > 1
      ? startX + ((queries.length - 1) * METRIC_CHAIN_GAP_X) / 2
      : startX;
  acc.layouts.set(unionId, {x: centerX, y: maxY});
  acc.rootNodeIds.push(unionId);

  return {...acc, finalNodeId: unionId, nextY: maxY + NODE_GAP_Y};
}

// ============================================================================
// AddColumns Decomposition
// ============================================================================

function decomposeAddColumns(
  addCols: protos.PerfettoSqlStructuredQuery.IExperimentalAddColumns,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let maxY = startY;

  let coreNodeId: string | undefined;
  if (addCols.coreQuery !== undefined && addCols.coreQuery !== null) {
    const r = decomposeStructuredQuery(
      addCols.coreQuery,
      startX,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    coreNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  let inputNodeId: string | undefined;
  if (addCols.inputQuery !== undefined && addCols.inputQuery !== null) {
    const r = decomposeStructuredQuery(
      addCols.inputQuery,
      startX + METRIC_CHAIN_GAP_X,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    inputNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  let leftColumn = '';
  let rightColumn = '';
  if (
    addCols.equalityColumns !== undefined &&
    addCols.equalityColumns !== null
  ) {
    leftColumn = addCols.equalityColumns.leftColumn ?? '';
    rightColumn = addCols.equalityColumns.rightColumn ?? '';
  }

  const selectedColumns = (addCols.inputColumns ?? []).map(
    (sc) => sc.columnNameOrExpression ?? sc.columnName ?? sc.alias ?? '',
  );

  const addColId = nextImportNodeId();
  acc.nodes.push({
    nodeId: addColId,
    type: NodeType.kAddColumns,
    state: {
      primaryInputId: coreNodeId,
      secondaryInputNodeId: inputNodeId,
      selectedColumns,
      leftColumn,
      rightColumn,
    },
    nextNodes: [],
  });

  for (const id of [coreNodeId, inputNodeId]) {
    if (id !== undefined) {
      const node = acc.nodes.find((n) => n.nodeId === id);
      if (node !== undefined) node.nextNodes.push(addColId);
    }
  }

  acc.layouts.set(addColId, {x: startX + METRIC_CHAIN_GAP_X / 2, y: maxY});
  acc.rootNodeIds.push(addColId);

  return {...acc, finalNodeId: addColId, nextY: maxY + NODE_GAP_Y};
}

// ============================================================================
// FilterToIntervals (FilterDuring) Decomposition
// ============================================================================

function decomposeFilterToIntervals(
  fti: protos.PerfettoSqlStructuredQuery.IExperimentalFilterToIntervals,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let maxY = startY;

  let baseNodeId: string | undefined;
  if (fti.base !== undefined && fti.base !== null) {
    const r = decomposeStructuredQuery(
      fti.base,
      startX,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    baseNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  let intervalsNodeId: string | undefined;
  if (fti.intervals !== undefined && fti.intervals !== null) {
    const r = decomposeStructuredQuery(
      fti.intervals,
      startX + METRIC_CHAIN_GAP_X,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    intervalsNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  const ftiId = nextImportNodeId();
  const secondaryInputNodeIds =
    intervalsNodeId !== undefined ? [intervalsNodeId] : [];
  acc.nodes.push({
    nodeId: ftiId,
    type: NodeType.kFilterDuring,
    state: {
      primaryInputId: baseNodeId,
      secondaryInputNodeIds,
      clipToIntervals: fti.clipToIntervals ?? true,
    },
    nextNodes: [],
  });

  for (const id of [baseNodeId, intervalsNodeId]) {
    if (id !== undefined) {
      const node = acc.nodes.find((n) => n.nodeId === id);
      if (node !== undefined) node.nextNodes.push(ftiId);
    }
  }

  acc.layouts.set(ftiId, {x: startX + METRIC_CHAIN_GAP_X / 2, y: maxY});
  acc.rootNodeIds.push(ftiId);

  return {...acc, finalNodeId: ftiId, nextY: maxY + NODE_GAP_Y};
}

// ============================================================================
// CreateSlices Decomposition
// ============================================================================

function decomposeCreateSlices(
  cs: protos.PerfettoSqlStructuredQuery.IExperimentalCreateSlices,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let maxY = startY;

  let startsNodeId: string | undefined;
  if (cs.startsQuery !== undefined && cs.startsQuery !== null) {
    const r = decomposeStructuredQuery(
      cs.startsQuery,
      startX,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    startsNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  let endsNodeId: string | undefined;
  if (cs.endsQuery !== undefined && cs.endsQuery !== null) {
    const r = decomposeStructuredQuery(
      cs.endsQuery,
      startX + METRIC_CHAIN_GAP_X,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    endsNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  const csId = nextImportNodeId();
  acc.nodes.push({
    nodeId: csId,
    type: NodeType.kCreateSlices,
    state: {
      startsNodeId,
      endsNodeId,
      startsTsColumn: cs.startsTsColumn ?? 'ts',
      endsTsColumn: cs.endsTsColumn ?? 'ts',
    },
    nextNodes: [],
  });

  for (const id of [startsNodeId, endsNodeId]) {
    if (id !== undefined) {
      const node = acc.nodes.find((n) => n.nodeId === id);
      if (node !== undefined) node.nextNodes.push(csId);
    }
  }

  acc.layouts.set(csId, {x: startX + METRIC_CHAIN_GAP_X / 2, y: maxY});
  acc.rootNodeIds.push(csId);

  return {...acc, finalNodeId: csId, nextY: maxY + NODE_GAP_Y};
}

// ============================================================================
// CounterToIntervals Decomposition
// ============================================================================

function decomposeCounterIntervals(
  ci: protos.PerfettoSqlStructuredQuery.IExperimentalCounterIntervals,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let currentY = startY;

  let inputNodeId: string | undefined;
  if (ci.inputQuery !== undefined && ci.inputQuery !== null) {
    const r = decomposeStructuredQuery(
      ci.inputQuery,
      startX,
      currentY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    inputNodeId = r.finalNodeId;
    currentY = r.nextY;
  }

  const ciId = nextImportNodeId();
  acc.nodes.push({
    nodeId: ciId,
    type: NodeType.kCounterToIntervals,
    state: {primaryInputId: inputNodeId},
    nextNodes: [],
  });

  if (inputNodeId !== undefined) {
    const inputNode = acc.nodes.find((n) => n.nodeId === inputNodeId);
    if (inputNode !== undefined) inputNode.nextNodes.push(ciId);
  }

  acc.layouts.set(ciId, {x: startX, y: currentY});
  acc.rootNodeIds.push(ciId);

  return {...acc, finalNodeId: ciId, nextY: currentY + NODE_GAP_Y};
}

// ============================================================================
// FilterIn Decomposition
// ============================================================================

function decomposeFilterIn(
  fi: protos.PerfettoSqlStructuredQuery.IExperimentalFilterIn,
  startX: number,
  startY: number,
  sharedQueries?: Map<string, protos.IPerfettoSqlStructuredQuery>,
  sqlModules?: SqlModules,
): DecomposeResult {
  const acc: ResultAccumulator = {
    nodes: [],
    rootNodeIds: [],
    layouts: new Map(),
  };
  let maxY = startY;

  let baseNodeId: string | undefined;
  if (fi.base !== undefined && fi.base !== null) {
    const r = decomposeStructuredQuery(
      fi.base,
      startX,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    baseNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  let matchNodeId: string | undefined;
  if (fi.matchValues !== undefined && fi.matchValues !== null) {
    const r = decomposeStructuredQuery(
      fi.matchValues,
      startX + METRIC_CHAIN_GAP_X,
      startY,
      sharedQueries,
      sqlModules,
    );
    mergeResult(acc, r);
    matchNodeId = r.finalNodeId;
    maxY = Math.max(maxY, r.nextY);
  }

  const fiId = nextImportNodeId();
  const secondaryInputNodeIds = matchNodeId !== undefined ? [matchNodeId] : [];
  acc.nodes.push({
    nodeId: fiId,
    type: NodeType.kFilterIn,
    state: {
      primaryInputId: baseNodeId,
      secondaryInputNodeIds,
      baseColumn: fi.baseColumn ?? '',
      matchColumn: fi.matchColumn ?? '',
    },
    nextNodes: [],
  });

  for (const id of [baseNodeId, matchNodeId]) {
    if (id !== undefined) {
      const node = acc.nodes.find((n) => n.nodeId === id);
      if (node !== undefined) node.nextNodes.push(fiId);
    }
  }

  acc.layouts.set(fiId, {x: startX + METRIC_CHAIN_GAP_X / 2, y: maxY});
  acc.rootNodeIds.push(fiId);

  return {...acc, finalNodeId: fiId, nextY: maxY + NODE_GAP_Y};
}

// ============================================================================
// ExperimentalFilterGroup Decomposition
// ============================================================================

/**
 * Checks whether an ExperimentalFilterGroup is "simple" — meaning it contains
 * only `filters` (no nested `groups` and no `sql_expressions`).
 */
function isSimpleFilterGroup(
  group: protos.PerfettoSqlStructuredQuery.IExperimentalFilterGroup,
): boolean {
  const groups = group.groups ?? [];
  const sqlExprs = group.sqlExpressions ?? [];
  const filters = group.filters ?? [];
  return filters.length > 0 && groups.length === 0 && sqlExprs.length === 0;
}

/**
 * Checks whether an ExperimentalFilterGroup contains only nested `groups`
 * (no direct `filters` and no `sql_expressions`).
 */
function isGroupsOnlyFilterGroup(
  group: protos.PerfettoSqlStructuredQuery.IExperimentalFilterGroup,
): boolean {
  const groups = group.groups ?? [];
  const sqlExprs = group.sqlExpressions ?? [];
  const filters = group.filters ?? [];
  return groups.length > 0 && filters.length === 0 && sqlExprs.length === 0;
}

/**
 * Converts a single proto Filter to a SQL expression string.
 */
function protoFilterToSql(
  f: protos.PerfettoSqlStructuredQuery.IFilter,
): string {
  const op = FILTER_OP_MAP[f.op ?? 0] ?? '=';
  const column = f.columnName ?? '';

  // IS NULL / IS NOT NULL don't need a RHS value.
  if (op === 'IS NULL' || op === 'IS NOT NULL') {
    return `${column} ${op}`;
  }

  // Extract value.
  const stringRhs = f.stringRhs ?? [];
  const doubleRhs = f.doubleRhs ?? [];
  const int64Rhs = f.int64Rhs ?? [];
  if (stringRhs.length > 0) {
    // Quote string values.
    return `${column} ${op} '${stringRhs[0]}'`;
  } else if (doubleRhs.length > 0) {
    return `${column} ${op} ${doubleRhs[0]}`;
  } else if (int64Rhs.length > 0) {
    return `${column} ${op} ${int64Rhs[0]}`;
  }
  return `${column} ${op} ''`;
}

/**
 * Recursively converts an ExperimentalFilterGroup into a SQL WHERE expression.
 */
function filterGroupToSql(
  group: protos.PerfettoSqlStructuredQuery.IExperimentalFilterGroup,
): string {
  const opEnum =
    protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator;
  const joiner = group.op === opEnum.OR ? ' OR ' : ' AND ';

  const parts: string[] = [];

  // Convert each filter to SQL.
  for (const f of group.filters ?? []) {
    parts.push(protoFilterToSql(f));
  }

  // Add raw SQL expressions.
  for (const expr of group.sqlExpressions ?? []) {
    parts.push(expr);
  }

  // Recurse into nested groups, wrapping each in parentheses.
  for (const subGroup of group.groups ?? []) {
    const subSql = filterGroupToSql(subGroup);
    if (subSql.length > 0) {
      parts.push(`(${subSql})`);
    }
  }

  return parts.join(joiner);
}

/**
 * Decomposes an ExperimentalFilterGroup into one or more SerializedNode[]
 * (FilterNodes) to chain onto the graph.
 *
 * Strategy:
 * 1. Simple flat AND/OR of filters only → structured FilterNode
 * 2. AND of sub-groups only (no direct filters/sql) → one FilterNode per
 *    sub-group (each sub-group that is simple becomes structured; complex
 *    ones become freeform)
 * 3. Everything else → single freeform FilterNode with the full SQL expression
 */
function decomposeFilterGroup(
  group: protos.PerfettoSqlStructuredQuery.IExperimentalFilterGroup,
  primaryInputId: string,
): SerializedNode[] {
  const opEnum =
    protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator;

  // Case 1: Simple flat group — only filters, no groups, no sql_expressions.
  if (isSimpleFilterGroup(group)) {
    const filterState = protoFiltersToFilterState(group.filters ?? []);
    filterState.primaryInputId = primaryInputId;
    if (group.op === opEnum.OR) {
      filterState.filterOperator = 'OR';
    }

    const nodeId = nextImportNodeId();
    return [
      {
        nodeId,
        type: NodeType.kFilter,
        state: filterState,
        nextNodes: [],
      },
    ];
  }

  // Case 2: AND of sub-groups only — decompose each sub-group into its own
  // FilterNode (chained sequentially, giving implicit AND).
  if (group.op === opEnum.AND && isGroupsOnlyFilterGroup(group)) {
    const result: SerializedNode[] = [];
    let currentInputId = primaryInputId;
    for (const subGroup of group.groups ?? []) {
      const subNodes = decomposeFilterGroup(subGroup, currentInputId);
      result.push(...subNodes);
      // The last node from the sub-group becomes the input for the next.
      if (subNodes.length > 0) {
        currentInputId = subNodes[subNodes.length - 1].nodeId;
      }
    }
    return result;
  }

  // Case 3: Complex / mixed — fall back to freeform SQL.
  const sql = filterGroupToSql(group);
  if (sql.length === 0) {
    return [];
  }

  const nodeId = nextImportNodeId();
  return [
    {
      nodeId,
      type: NodeType.kFilter,
      state: {
        primaryInputId,
        filterMode: 'freeform',
        sqlExpression: sql,
      },
      nextNodes: [],
    },
  ];
}

// ============================================================================
// Proto → Node State Conversion Helpers
// ============================================================================

const FILTER_OP_MAP: Record<number, string> = {
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL]: '=',
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.NOT_EQUAL]: '!=',
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN]: '<',
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN_EQUAL]: '<=',
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN]: '>',
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN_EQUAL]: '>=',
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NULL]: 'IS NULL',
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NOT_NULL]:
    'IS NOT NULL',
  [protos.PerfettoSqlStructuredQuery.Filter.Operator.GLOB]: 'GLOB',
};

/** Returns the total number of RHS values across all RHS fields. */
function protoFilterRhsCount(
  f: protos.PerfettoSqlStructuredQuery.IFilter,
): number {
  return (
    (f.stringRhs ?? []).length +
    (f.doubleRhs ?? []).length +
    (f.int64Rhs ?? []).length
  );
}

/** Extracts the first RHS value as a string. */
function protoFilterFirstRhsValue(
  f: protos.PerfettoSqlStructuredQuery.IFilter,
): string {
  const stringRhs = f.stringRhs ?? [];
  const doubleRhs = f.doubleRhs ?? [];
  const int64Rhs = f.int64Rhs ?? [];
  if (stringRhs.length > 0) return stringRhs[0];
  if (doubleRhs.length > 0) return String(doubleRhs[0]);
  if (int64Rhs.length > 0) return String(int64Rhs[0]);
  return '';
}

/** Extracts ALL RHS values as strings. */
function protoFilterAllRhsValues(
  f: protos.PerfettoSqlStructuredQuery.IFilter,
): string[] {
  const values: string[] = [];
  for (const v of f.stringRhs ?? []) values.push(v);
  for (const v of f.doubleRhs ?? []) values.push(String(v));
  for (const v of f.int64Rhs ?? []) values.push(String(v));
  return values;
}

function protoFiltersToFilterState(
  filters: protos.PerfettoSqlStructuredQuery.IFilter[],
): Record<string, unknown> {
  const uiFilters = filters.map((f) => {
    const op = FILTER_OP_MAP[f.op ?? 0] ?? '=';
    const column = f.columnName ?? '';
    const value = protoFilterFirstRhsValue(f);
    return {column, op, value, enabled: true};
  });

  return {filters: uiFilters};
}

/**
 * Expands a single proto filter with multiple RHS values into an OR
 * FilterNode state. Each RHS value becomes a separate filter entry.
 */
function protoFilterToExpandedOrState(
  f: protos.PerfettoSqlStructuredQuery.IFilter,
): Record<string, unknown> {
  const op = FILTER_OP_MAP[f.op ?? 0] ?? '=';
  const column = f.columnName ?? '';
  const values = protoFilterAllRhsValues(f);

  const uiFilters = values.map((value) => ({
    column,
    op,
    value,
    enabled: true,
  }));

  return {filters: uiFilters, filterOperator: 'OR'};
}

const AGG_OP_MAP: Record<number, string> = {
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.COUNT]: 'COUNT',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.SUM]: 'SUM',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MIN]: 'MIN',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MAX]: 'MAX',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MEAN]: 'MEAN',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MEDIAN]: 'MEDIAN',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
    .DURATION_WEIGHTED_MEAN]: 'DURATION_WEIGHTED_MEAN',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.COUNT_DISTINCT]:
    'COUNT_DISTINCT',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.PERCENTILE]:
    'PERCENTILE',
  [protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.CUSTOM]: 'CUSTOM',
};

function protoGroupByToAggregationState(
  groupBy: protos.PerfettoSqlStructuredQuery.IGroupBy,
): Record<string, unknown> {
  const groupByColumns = (groupBy.columnNames ?? []).map((name) => ({
    name,
    checked: true,
  }));

  const aggregations = (groupBy.aggregates ?? []).map((agg) => ({
    column: agg.columnName !== undefined ? {name: agg.columnName} : undefined,
    aggregationOp: AGG_OP_MAP[agg.op ?? 0] ?? 'COUNT',
    newColumnName: agg.resultColumnName || undefined,
    percentile: agg.percentile ?? undefined,
    isValid: true,
  }));

  return {groupByColumns, aggregations};
}

// Returns true when select_columns just re-lists the group_by output columns
// without aliases. In that case the ModifyColumns node would be a no-op.
function isSelectRedundantAfterGroupBy(
  sq: protos.IPerfettoSqlStructuredQuery,
): boolean {
  if (sq.groupBy === undefined || sq.groupBy === null) return false;
  const selectCols = sq.selectColumns ?? [];
  if (selectCols.length === 0) return false;

  // Collect the column names the group_by produces.
  const groupByCols = new Set(sq.groupBy.columnNames ?? []);
  for (const agg of sq.groupBy.aggregates ?? []) {
    if (agg.resultColumnName) groupByCols.add(agg.resultColumnName);
  }

  // If any select column has an alias or expression, it's not redundant.
  for (const sc of selectCols) {
    const name = sc.columnName ?? sc.columnNameOrExpression ?? '';
    if (sc.alias) return false;
    if (!groupByCols.has(name)) return false;
  }
  return selectCols.length === groupByCols.size;
}

function protoSelectColumnsToModifyState(
  selectColumns: protos.PerfettoSqlStructuredQuery.ISelectColumn[],
): Record<string, unknown> {
  const selectedColumns = selectColumns.map((sc) => ({
    name: sc.columnNameOrExpression ?? sc.columnName ?? sc.alias ?? '',
    checked: true,
    alias: sc.alias || undefined,
  }));

  return {selectedColumns};
}

function protoOrderByToSortState(
  orderBy: protos.PerfettoSqlStructuredQuery.IOrderBy,
): Record<string, unknown> {
  const sortCriteria = (orderBy.orderingSpecs ?? []).map((spec) => ({
    colName: spec.columnName ?? '',
    direction:
      spec.direction ===
      protos.PerfettoSqlStructuredQuery.OrderBy.Direction.DESC
        ? 'DESC'
        : 'ASC',
  }));

  return {sortCriteria};
}

// ============================================================================
// Metric Spec → MetricsNode State Conversion
// ============================================================================

const UNIT_ENUM_TO_STRING: Record<number, string> = {
  [protos.TraceMetricV2Spec.MetricUnit.COUNT]: 'COUNT',
  [protos.TraceMetricV2Spec.MetricUnit.TIME_NANOS]: 'TIME_NANOS',
  [protos.TraceMetricV2Spec.MetricUnit.TIME_MICROS]: 'TIME_MICROS',
  [protos.TraceMetricV2Spec.MetricUnit.TIME_MILLIS]: 'TIME_MILLIS',
  [protos.TraceMetricV2Spec.MetricUnit.TIME_SECONDS]: 'TIME_SECONDS',
  [protos.TraceMetricV2Spec.MetricUnit.BYTES]: 'BYTES',
  [protos.TraceMetricV2Spec.MetricUnit.KILOBYTES]: 'KILOBYTES',
  [protos.TraceMetricV2Spec.MetricUnit.MEGABYTES]: 'MEGABYTES',
  [protos.TraceMetricV2Spec.MetricUnit.PERCENTAGE]: 'PERCENTAGE',
  [protos.TraceMetricV2Spec.MetricUnit.BOUNDED_PERCENTAGE]:
    'BOUNDED_PERCENTAGE',
  [protos.TraceMetricV2Spec.MetricUnit.MILLI_AMPS]: 'MILLI_AMPS',
  [protos.TraceMetricV2Spec.MetricUnit.MILLI_WATTS]: 'MILLI_WATTS',
  [protos.TraceMetricV2Spec.MetricUnit.MILLI_WATT_HOURS]: 'MILLI_WATT_HOURS',
  [protos.TraceMetricV2Spec.MetricUnit.MILLI_AMP_HOURS]: 'MILLI_AMP_HOURS',
  [protos.TraceMetricV2Spec.MetricUnit.CELSIUS]: 'CELSIUS',
  [protos.TraceMetricV2Spec.MetricUnit.MILLI_VOLTS]: 'MILLI_VOLTS',
};

const POLARITY_ENUM_TO_STRING: Record<number, string> = {
  [protos.TraceMetricV2Spec.MetricPolarity.NOT_APPLICABLE]: 'NOT_APPLICABLE',
  [protos.TraceMetricV2Spec.MetricPolarity.HIGHER_IS_BETTER]:
    'HIGHER_IS_BETTER',
  [protos.TraceMetricV2Spec.MetricPolarity.LOWER_IS_BETTER]: 'LOWER_IS_BETTER',
};

function resolveUnit(spec: {
  unit?: protos.TraceMetricV2Spec.MetricUnit | null;
  customUnit?: string | null;
}): {unit: string; customUnit?: string} {
  if (
    spec.customUnit !== undefined &&
    spec.customUnit !== null &&
    spec.customUnit !== ''
  ) {
    return {unit: 'CUSTOM', customUnit: spec.customUnit};
  }
  if (spec.unit !== undefined && spec.unit !== null) {
    return {unit: UNIT_ENUM_TO_STRING[spec.unit] ?? 'COUNT'};
  }
  return {unit: 'COUNT'};
}

function resolvePolarity(
  polarity: protos.TraceMetricV2Spec.MetricPolarity | null | undefined,
): string {
  if (polarity !== undefined && polarity !== null) {
    return POLARITY_ENUM_TO_STRING[polarity] ?? 'NOT_APPLICABLE';
  }
  return 'NOT_APPLICABLE';
}

export function templateSpecToMetricsState(
  spec: protos.ITraceMetricV2TemplateSpec,
): Record<string, unknown> {
  const metricIdPrefix = spec.idPrefix ?? '';

  // Value columns from valueColumnSpecs or simple valueColumns.
  const valueColumns: Array<Record<string, unknown>> = [];
  const valueColumnSpecs = spec.valueColumnSpecs ?? [];
  const simpleValueColumns = spec.valueColumns ?? [];
  if (valueColumnSpecs.length > 0) {
    for (const vcs of valueColumnSpecs) {
      const {unit, customUnit} = resolveUnit(vcs);
      valueColumns.push({
        column: vcs.name ?? '',
        unit,
        customUnit,
        polarity: resolvePolarity(vcs.polarity),
        displayName: vcs.displayName || undefined,
        displayHelp: vcs.displayHelp || undefined,
      });
    }
  } else if (simpleValueColumns.length > 0) {
    for (const colName of simpleValueColumns) {
      valueColumns.push({
        column: colName,
        unit: 'COUNT',
        polarity: 'NOT_APPLICABLE',
      });
    }
  }

  // Dimension configs from dimensionsSpecs.
  const dimensionConfigs: Record<string, Record<string, unknown>> = {};
  const dimSpecs = spec.dimensionsSpecs ?? [];
  for (const ds of dimSpecs) {
    if (ds.name !== undefined && ds.name !== null) {
      const cfg: Record<string, unknown> = {};
      if (ds.displayName) cfg.displayName = ds.displayName;
      if (ds.displayHelp) cfg.displayHelp = ds.displayHelp;
      if (Object.keys(cfg).length > 0) {
        dimensionConfigs[ds.name] = cfg;
      }
    }
  }

  // Dimension uniqueness.
  let dimensionUniqueness = 'NOT_UNIQUE';
  if (
    spec.dimensionUniqueness ===
    protos.TraceMetricV2Spec.DimensionUniqueness.UNIQUE
  ) {
    dimensionUniqueness = 'UNIQUE';
  }

  return {
    metricIdPrefix,
    valueColumns,
    dimensionConfigs:
      Object.keys(dimensionConfigs).length > 0 ? dimensionConfigs : undefined,
    dimensionUniqueness,
  };
}

export function metricSpecToMetricsState(
  spec: protos.ITraceMetricV2Spec,
): Record<string, unknown> {
  const metricIdPrefix = spec.id ?? '';
  const valueColumns: Array<Record<string, unknown>> = [];

  if (spec.value !== undefined && spec.value !== null && spec.value !== '') {
    const {unit, customUnit} = resolveUnit(spec);
    valueColumns.push({
      column: spec.value,
      unit,
      customUnit,
      polarity: resolvePolarity(spec.polarity),
    });
  }

  const dimensionConfigs: Record<string, Record<string, unknown>> = {};
  const dimSpecs = spec.dimensionsSpecs ?? [];
  for (const ds of dimSpecs) {
    if (ds.name !== undefined && ds.name !== null) {
      const cfg: Record<string, unknown> = {};
      if (ds.displayName) cfg.displayName = ds.displayName;
      if (ds.displayHelp) cfg.displayHelp = ds.displayHelp;
      if (Object.keys(cfg).length > 0) {
        dimensionConfigs[ds.name] = cfg;
      }
    }
  }

  let dimensionUniqueness = 'NOT_UNIQUE';
  if (
    spec.dimensionUniqueness ===
    protos.TraceMetricV2Spec.DimensionUniqueness.UNIQUE
  ) {
    dimensionUniqueness = 'UNIQUE';
  }

  return {
    metricIdPrefix,
    valueColumns,
    dimensionConfigs:
      Object.keys(dimensionConfigs).length > 0 ? dimensionConfigs : undefined,
    dimensionUniqueness,
  };
}

// ============================================================================
// Pbtxt Detection & Wrapping
// ============================================================================

/**
 * Detects whether the pbtxt is a single metric_template_spec, a single
 * metric_spec, or a full TraceSummarySpec. Wraps single specs if needed.
 */
export function detectAndWrapPbtxt(text: string): string {
  const trimmed = text.trim();

  // If it already contains top-level TraceSummarySpec fields, use as-is.
  if (/^(metric_template_spec|metric_spec|query)\s*[:{]/m.test(trimmed)) {
    return trimmed;
  }

  // If it looks like a single TraceMetricV2TemplateSpec (has id_prefix).
  if (/^id_prefix\s*:/m.test(trimmed)) {
    return `metric_template_spec {\n${trimmed}\n}`;
  }

  // If it looks like a single TraceMetricV2Spec (has id: field).
  if (/^id\s*:/m.test(trimmed)) {
    return `metric_spec {\n${trimmed}\n}`;
  }

  // Otherwise, assume it's a full TraceSummarySpec.
  return trimmed;
}

/**
 * Converts SimpleSlices glob fields into GLOB filter entries so they are
 * preserved as FilterNode state. The SlicesSourceNode itself does not
 * support glob fields, so without this the filters would be silently lost.
 */
function buildSimpleSlicesFilters(
  slices: protos.PerfettoSqlStructuredQuery.ISimpleSlices,
): Array<Record<string, unknown>> {
  const GLOB_COLUMN_MAP: Array<{
    field: string | null | undefined;
    column: string;
  }> = [
    {field: slices.sliceNameGlob, column: 'name'},
    {field: slices.processNameGlob, column: 'process_name'},
    {field: slices.threadNameGlob, column: 'thread_name'},
    {field: slices.trackNameGlob, column: 'track_name'},
  ];

  return GLOB_COLUMN_MAP.filter(
    (entry) =>
      entry.field !== undefined && entry.field !== null && entry.field !== '',
  ).map((entry) => ({
    column: entry.column,
    op: 'GLOB',
    value: entry.field,
    enabled: true,
  }));
}

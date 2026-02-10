# Explore Page Architecture

This document explains how Perfetto's Explore Page works, from creating visual query graphs to executing SQL queries and displaying results. It covers the key components, data flow, and architectural patterns that enable the Explore Page to provide an interactive, node-based SQL query builder for trace analysis.

## Overview

The Explore Page is a visual query builder that allows users to construct complex SQL queries by connecting nodes in a directed acyclic graph (DAG). Each node represents either a data source (table, slices, custom SQL) or an operation (filter, aggregation, join, etc.). The system converts this visual graph into structured SQL queries, executes them via the trace processor, and displays results in an interactive data grid.

## Core Data Flow

```
User Interaction → Node Graph → Structured Query Generation →
Query Analysis (Validation) → Query Materialization → Result Display
```

## Node Graph Structure

**QueryNode** (`ui/src/plugins/dev.perfetto.ExplorePage/query_node.ts:128-161`)
- Base abstraction for all node types
- Maintains bidirectional connections: `primaryInput` (upstream), `nextNodes` (downstream), `secondaryInputs` (side connections)
- Generates structured query protobuf via `getStructuredQuery()`
- Validates configuration and provides UI rendering methods

**Node Connections** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/graph_utils.ts`)
- Primary Input: Vertical data flow (single parent node)
- Secondary Inputs: Horizontal data flow (side connections with port numbers)
- Bidirectional relationship management via `addConnection()`/`removeConnection()`
- Port-based routing for multi-input operations

## Node Registration and Creation

**NodeRegistry** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/node_registry.ts`)
- Central registry for all node types
- Descriptors specify: name, icon, type (source/modification/multisource), factory function
- Optional `preCreate()` hook for interactive setup (e.g., table selection modal)
- Supports keyboard shortcuts for rapid node creation

**Core Nodes** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/core_nodes.ts`)
```typescript
registerCoreNodes() {
  nodeRegistry.register('table', {...});
  nodeRegistry.register('slice', {...});
  nodeRegistry.register('sql', {...});
  nodeRegistry.register('filter', {...});
  nodeRegistry.register('aggregation', {...});
  // ... more nodes
}
```

## Node Types

### 1. Source Nodes (Data Origin)
**TableSourceNode** - Queries a specific SQL table
**SlicesSourceNode** - Pre-configured query for trace slices
**SqlSourceNode** - Custom SQL query as data source
**TimeRangeSourceNode** - Generates time intervals

### 2. Single-Input Modification Nodes
**FilterNode** - Adds WHERE conditions
**SortNode** - Adds ORDER BY clauses
**AggregationNode** - GROUP BY with aggregate functions
**ModifyColumnsNode** - Renames/removes columns
**AddColumnsNode** - Adds columns from secondary source via LEFT JOIN and/or computed expressions
**LimitAndOffsetNode** - Pagination

### 3. Multi-Input Nodes
**UnionNode** - Combines rows from multiple sources
**JoinNode** - Combines columns via JOIN conditions
**IntervalIntersectNode** - Finds overlapping time intervals
**FilterDuringNode** - Filters using secondary interval input
**CreateSlicesNode** - Pairs start/end events from two secondary sources into slices

## UI Components

**Builder** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/builder.ts`)
- Main component coordinating all sub-components
- Manages layout with resizable sidebar and split panel
- Three views: Info, Modify (node-specific), Result
- Handles node selection, execution callbacks, undo/redo

**Graph** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/graph/graph.ts`)
- Visual canvas for node manipulation
- Drag-and-drop positioning with persistent layouts
- Connection management via draggable ports
- Label annotations for documentation

**NodeExplorer** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/node_explorer.ts`)
- Sidebar panel for selected node
- Displays node info, configuration UI, and SQL preview
- Triggers query analysis on state changes
- Manages execution flow via QueryExecutionService

**DataExplorer** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/data_explorer.ts`)
- Bottom drawer showing query results
- Server-side pagination via SQLDataSource
- Column-based filtering and sorting
- Export to timeline functionality

## Query Execution Model

### Two-Phase Execution

**Phase 1: Analysis (Validation)**
```
Node Graph → Structured Query Protobuf → Engine.updateSummarizerSpec() + querySummarizer() →
Query {sql, textproto, columns} | Error
```
- Creates summarizer via `createSummarizer(summarizerId)` (once per session)
- Registers queries with TP via `updateSummarizerSpec(summarizerId, spec)`
- Fetches SQL and metadata via `querySummarizer(summarizerId, queryId)` (triggers lazy materialization)
- TP computes proto hash for change detection internally

**Phase 2: Materialization (Execution)**
```
engine.querySummarizer(summarizerId, nodeId) → TP creates/reuses table →
{tableName, rowCount, columns, durationMs} → SQLDataSource → DataGrid Display
```
- TP creates persistent table for server-side pagination (lazy, on first querySummarizer)
- TP handles caching internally (reuses table if proto hash unchanged)
- querySummarizer returns all metadata needed for display

### QueryExecutionService

**Purpose** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_execution_service.ts`)
- Prevents race conditions during rapid user interaction via FIFO execution queue
- Debounces rapid requests to batch user input
- Coordinates with Trace Processor's materialization API
- Query analysis (validation) before execution

**Trace Processor as Single Source of Truth**

All materialization state is managed by Trace Processor (TP), not the UI:
- TP tracks which queries are materialized (by query_id)
- TP compares SQL hashes internally to detect changes
- TP creates/drops tables as needed
- TP stores table names and error states

The UI queries TP on-demand instead of caching:
```typescript
// Fetch table name from TP when needed (e.g., for "Copy Table Name" or export)
async getTableName(nodeId: string): Promise<string | undefined> {
  const result = await engine.querySummarizer(DATA_EXPLORER_SUMMARIZER_ID, nodeId);
  if (result.exists !== true || result.error) {
    return undefined;
  }
  return result.tableName;
}
```

This eliminates state synchronization bugs between UI and TP.

**FIFO Execution Queue**
- Serialized execution (one operation at a time)
- Preserves node dependencies (parent materializes before child)
- Per-operation error isolation (errors are logged, not thrown)

**Rapid Node Click Handling** (`ui/src/base/async_limiter.ts`)

The `AsyncLimiter` ensures only the latest queued task runs when clicking nodes rapidly:
```typescript
// AsyncLimiter behavior:
while ((task = taskQueue.shift())) {
  if (taskQueue.length > 0) {
    task.deferred.resolve();  // Skip - newer tasks waiting
  } else {
    await task.work();  // Run - this is the latest
  }
}
```

Example: Click A → B → C rapidly while A is processing:
1. A starts processing
2. B queued, C queued
3. A finishes
4. B skipped (queue has C), C runs

This ensures the currently selected node (C) is processed, intermediate clicks (B) are skipped.

**Materialization via TP API**
```typescript
// Sync all queries with TP, then fetch result for the target node
async processNode(node: QueryNode): Promise<void> {
  // 1. Ensure summarizer exists (created once per session)
  await engine.createSummarizer(DATA_EXPLORER_SUMMARIZER_ID);

  // 2. Register all queries with TP (handles change detection)
  const spec = buildTraceSummarySpec(allNodes);
  await engine.updateSummarizerSpec(DATA_EXPLORER_SUMMARIZER_ID, spec);

  // 3. Fetch result - triggers lazy materialization
  const result = await engine.querySummarizer(DATA_EXPLORER_SUMMARIZER_ID, node.nodeId);
  // Returns: tableName, rowCount, columns, durationMs, sql, textproto
}
```

**Auto-Execute Logic** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_execution_service.ts`)

| autoExecute | manual | Behavior                              |
|-------------|--------|---------------------------------------|
| true        | false  | Analyze + execute automatically       |
| true        | true   | Analyze + execute (forced)            |
| false       | false  | Skip - show "Run Query" button        |
| false       | true   | Analyze + execute (user clicked)      |

Auto-execute disabled for: SqlSourceNode, IntervalIntersectNode, UnionNode, FilterDuringNode, CreateSlicesNode

### State Management

**ExplorePageState** (`ui/src/plugins/dev.perfetto.ExplorePage/explore_page.ts:57-70`)
```typescript
interface ExplorePageState {
  rootNodes: QueryNode[];           // Nodes without parents (starting points)
  selectedNode?: QueryNode;         // Currently selected node
  nodeLayouts: Map<string, {x, y}>; // Visual positions
  labels?: Array<{...}>;            // Annotations
  isExplorerCollapsed?: boolean;
  sidebarWidth?: number;
}
```

**Query State Management** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/builder.ts:60-86`)

Builder maintains `this.query` as the single source of truth for query state:
- Updated by both automatic analysis (from NodeExplorer) and manual execution (from Builder)
- Passed to NodeExplorer as a prop for rendering SQL/Proto tabs
- Ensures consistent query display for both autoExecute=true and autoExecute=false nodes

Query State Flow:
```
Automatic execution (autoExecute=true):
  NodeExplorer.updateQuery() → processNode({ manual: false })
  → onAnalysisComplete → sets NodeExplorer.currentQuery
  → onAnalysisComplete → calls onQueryAnalyzed callback → sets Builder.query
  → Builder passes query as prop to NodeExplorer
  → NodeExplorer.renderContent() uses attrs.query ?? this.currentQuery

Manual execution (autoExecute=false):
  User clicks "Run Query" → Builder calls processNode({ manual: true })
  → onAnalysisComplete → sets Builder.query
  → onAnalysisComplete → calls onNodeQueryAnalyzed callback → sets Builder.query
  → Builder passes query as prop to NodeExplorer
  → NodeExplorer.renderContent() uses attrs.query (this.currentQuery may be undefined)
```

This ensures SQL/Proto tabs display correctly for both automatic and manual execution modes.

**Race Condition Prevention** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/builder.ts:283-292`)

The callback captures the selected node at creation time to prevent stale query leakage:
```typescript
const callbackNode = selectedNode;
this.onNodeQueryAnalyzed = (query) => {
  // Only update if still on the same node
  if (callbackNode === this.previousSelectedNode) {
    this.query = query;
  }
};
```

Without this check, rapid node switching can cause:
1. User selects Node A → async analysis starts
2. User quickly switches to Node B → Node A's component destroyed
3. Node A's analysis completes → callback fires with Node A's query
4. Node B incorrectly displays Node A's query in SQL/Proto tabs

The validation ensures callbacks from old nodes are ignored after switching.

**HistoryManager** (`ui/src/plugins/dev.perfetto.ExplorePage/history_manager.ts`)
- Undo/redo stack with state snapshots
- Serialization via `serializeState()` for each node
- Deserialization reconstructs entire graph from JSON

## Graph Operations

**Node Creation** (`ui/src/plugins/dev.perfetto.ExplorePage/explore_page.ts:260-308`)
```typescript
// Source nodes
handleAddSourceNode(id) {
  const descriptor = nodeRegistry.get(id);
  const initialState = await descriptor.preCreate?.();  // Optional modal
  const newNode = descriptor.factory(initialState);
  rootNodes.push(newNode);
}

// Operation nodes
handleAddOperationNode(id, parentNode) {
  const newNode = descriptor.factory(initialState);
  if (singleNodeOperation(newNode.type)) {
    insertNodeBetween(parentNode, newNode);  // A → C becomes A → B → C
  } else {
    addConnection(parentNode, newNode);       // Multi-input: just connect
  }
}
```

**Node Deletion** (`ui/src/plugins/dev.perfetto.ExplorePage/explore_page.ts:599-775`)
```typescript
// Complex reconnection logic preserves data flow
async handleDeleteNode(node) {
  1. await cleanupManager.cleanupNode(node);  // Drop SQL tables
  2. Capture graph structure (parent, children, port connections)
  3. disconnectNodeFromGraph(node)
  4. Reconnect primary parent to children (bypass deleted node)
     - Only primary connections (portIndex === undefined)
     - Secondary connections dropped (specific to deleted node)
  5. Update root nodes (add orphaned nodes)
  6. Transfer layouts to docked children
  7. Notify affected nodes via onPrevNodesUpdated()
}
```

**Graph Traversal** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/graph_utils.ts`)
- `getAllNodes()`: BFS traversal (both forward and backward)
- `getAllDownstreamNodes()`: Forward traversal (for invalidation)
- `getAllUpstreamNodes()`: Backward traversal (for dependency checking)
- `insertNodeBetween()`: Rewires connections when inserting operations

## Invalidation and Caching

**TP-Managed Caching**

Query hash caching and change detection is handled entirely by Trace Processor:
- TP computes and stores proto hashes for each materialized query
- When `updateSummarizerSpec()` is called, TP compares new hash to stored hash
- If unchanged, TP returns existing table name without re-execution
- If changed, TP drops old table and creates new one

**Lazy Materialization**

Materialization is lazy - TP only materializes a query when `querySummarizer()` is called
for that specific query. When `updateSummarizerSpec()` is called, all valid queries in
the graph are registered with TP, but no SQL is executed. Only when `querySummarizer(nodeId)`
is called does TP actually materialize that query (and its dependencies). This avoids
unnecessary work for nodes the user hasn't viewed yet.

**Smart Re-materialization Optimization**

When queries are synced with TP via `updateSummarizerSpec()`, TP performs intelligent change
detection and dependency tracking to minimize redundant work:

1. **Proto-based change detection**: Each query's structured query proto bytes are
   hashed (not the generated SQL). This works correctly for queries with
   `inner_query_id` references, which cannot have their SQL generated independently.

2. **Dependency propagation**: If query B depends on query A via `inner_query_id`,
   and A's proto changes, B must also be re-materialized even if B's proto is
   unchanged (because B's output depends on A's data). TP propagates this
   transitively through the entire dependency chain.

3. **Table-source substitution**: For unchanged queries that are already
   materialized, TP substitutes them with simple table-source structured queries
   that reference the materialized table. When SQL is generated for changed queries,
   they reference these tables directly instead of re-expanding the full query chain.

Example: For chain A → B → C → D, if C changes:
- A, B: Unchanged, use existing materialized tables (`_exp_mat_0`, `_exp_mat_1`)
- C: Changed, re-materialize (SQL references B's materialized table directly)
- D: Transitively changed (depends on C), re-materialize (SQL references C's new table)

This optimization significantly speeds up incremental edits in long query chains
by avoiding redundant SQL generation and execution. The TP-side implementation
lives in `src/trace_processor/trace_summary/summarizer.cc`.

**On-Demand State Queries**

The UI queries materialization state from TP when needed:
```typescript
// Get current state from TP (for "Copy Table Name", export, etc.)
const result = await engine.querySummarizer(DATA_EXPLORER_SUMMARIZER_ID, nodeId);
// Returns: { exists: boolean, tableName?: string, error?: string, ... }
```

This design ensures:
- No UI-side state can become stale or out of sync with TP
- TP is the authoritative source for all materialization state
- Simpler UI code with no cache invalidation logic

**Trace Processor Restart Handling**

If the Trace Processor restarts or crashes, all summarizer state (including materialized
tables) is lost. The UI may still hold a stale `summarizerId` that no longer exists in TP.
When the next `querySummarizer()` call is made, TP will return an error indicating the
summarizer doesn't exist. The UI handles this gracefully by treating it as a need to
re-create the summarizer and re-sync all queries on the next execution attempt. Users
may see an error message, but clicking "Run Query" again will recover the state.

## Structured Query Generation

**Query Construction** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_builder_utils.ts`)
```typescript
getStructuredQueries(finalNode) {
  const queries: PerfettoSqlStructuredQuery[] = [];
  let currentNode = finalNode;

  // Walk up the graph from leaf to root
  while (currentNode) {
    queries.push(currentNode.getStructuredQuery());
    currentNode = currentNode.primaryInput;  // Follow primary input chain
  }

  return queries.reverse();  // Root → Leaf order
}

analyzeNode(node, engine) {
  const structuredQueries = getStructuredQueries(node);
  const spec = new TraceSummarySpec();
  spec.query = structuredQueries;
  await engine.createSummarizer(ANALYZE_NODE_SUMMARIZER_ID);  // Ensure summarizer exists
  await engine.updateSummarizerSpec(ANALYZE_NODE_SUMMARIZER_ID, spec);  // Register with TP
  const result = await engine.querySummarizer(ANALYZE_NODE_SUMMARIZER_ID, node.nodeId);  // Fetch result
  return {sql: result.sql, textproto: result.textproto};
}
```

## Serialization and Examples

**JSON Serialization** (`ui/src/plugins/dev.perfetto.ExplorePage/json_handler.ts`)
- `exportStateAsJson()`: Serializes entire graph state to JSON file
- `deserializeState()`: Reconstructs graph from JSON
- Each node implements `serializeState()` for node-specific state
- Used for: Import/Export, Examples, Undo/Redo snapshots

**Examples System** (`ui/src/plugins/dev.perfetto.ExplorePage/examples_modal.ts`)
- Pre-built graphs stored as JSON in `ui/src/assets/explore_page/`
- Base page state auto-loaded on first visit
- Modal allows users to load curated examples

## Key Architectural Patterns

### 1. Node-Based Query Building
All queries constructed via composable nodes:
- Sources provide initial data (tables, slices, custom SQL)
- Operations transform data (filter, aggregate, join)
- Nodes connected via drag-and-drop visual interface
- Graph structure maps directly to SQL query structure

### 2. Bidirectional Graph Connections
Nodes maintain both forward and backward links:
- `primaryInput`: Single parent (vertical data flow)
- `secondaryInputs`: Map of port → parent (side connections)
- `nextNodes`: Array of children (consumers of this node's output)
- Graph operations maintain consistency across all links

### 3. Two-Phase Execution with Lazy Materialization
- Analysis phase: Validate query structure without execution
- Execution phase: Materialize into PERFETTO table for pagination
- Lazy materialization: only materialize selected node and its upstream dependencies
- TP manages table caching internally (reuses when proto hash unchanged)
- Smart re-materialization: unchanged parent queries use table-source substitution
- Server-side pagination via SQLDataSource (no full result fetch)

### 4. FIFO Queue with TP-Managed State
- Prevents race conditions during rapid user input
- Operations execute in order (preserves node dependencies)
- Per-operation error isolation (one failure doesn't block queue)
- TP handles all caching/change detection internally
- UI queries TP on-demand for table names (no UI-side caching)

### 5. Structured Query Protocol
- Nodes generate protobuf `PerfettoSqlStructuredQuery`
- Engine validates and converts to SQL via `updateSummarizerSpec()` + `querySummarizer()`
- Hash-based change detection (proto bytes hashed by TP)
- Enables query analysis without SQL string manipulation

## File Path Reference

**Core Infrastructure**:
- `ui/src/plugins/dev.perfetto.ExplorePage/explore_page.ts` - Main plugin and state management
- `ui/src/plugins/dev.perfetto.ExplorePage/query_node.ts` - Node abstraction and type definitions
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/builder.ts` - Main UI component
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_execution_service.ts` - Execution coordination

**Node System**:
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/node_registry.ts` - Node registration
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/core_nodes.ts` - Core node registration
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/nodes/` - Individual node implementations

**UI Components**:
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/graph/graph.ts` - Visual graph canvas
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/node_explorer.ts` - Node sidebar
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/data_explorer.ts` - Results drawer

**Utilities**:
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/graph_utils.ts` - Graph traversal and connection management
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_builder_utils.ts` - Query analysis and utilities
- `ui/src/plugins/dev.perfetto.ExplorePage/query_builder/cleanup_manager.ts` - Resource cleanup
- `ui/src/plugins/dev.perfetto.ExplorePage/history_manager.ts` - Undo/redo management
- `ui/src/plugins/dev.perfetto.ExplorePage/json_handler.ts` - Serialization

**Trace Processor (C++)**:
- `src/trace_processor/trace_summary/summarizer.cc` - Smart re-materialization with change detection and dependency propagation
- `src/trace_processor/trace_summary/summarizer.h` - Summarizer class definition and QueryState
- `src/trace_processor/perfetto_sql/generator/structured_query_generator.cc` - SQL generation from structured queries

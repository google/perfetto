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
Node Graph → Structured Query Protobuf → Engine.analyzeStructuredQuery() →
Query {sql, textproto, modules, preambles, columns} | Error
```
- Validates query structure without execution
- Returns generated SQL and metadata
- Computes query hash for change detection

**Phase 2: Materialization (Execution)**
```
Query → CREATE PERFETTO TABLE _exp_materialized_{nodeId} AS {sql} →
COUNT(*) + Column Metadata → SQLDataSource → DataGrid Display
```
- Creates persistent table for server-side pagination
- Fetches row count and column schema
- Reuses table if query hash unchanged

### QueryExecutionService

**Purpose** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_execution_service.ts:27-71`)
- Prevents race conditions during rapid user interaction
- Coordinates materialization lifecycle (create/drop/reuse tables)
- Caches query hashes to avoid expensive recomputation
- Implements staleness detection to skip outdated operations

**FIFO Execution Queue** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_execution_service.ts:442-659`)
- Serialized execution (one operation at a time)
- Preserves node dependencies (parent materializes before child)
- Staleness detection: operations store query hash at queue time, skip if hash changed
- Per-operation error isolation (one failure doesn't block queue)

**Materialization Lifecycle** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_execution_service.ts:122-279`)
```typescript
// Create/reuse materialized table
materializeNode(node, query, queryHash) {
  if (canReuseTable(node, queryHash)) return existingTable;
  if (node.state.materialized) await dropMaterialization(node);
  const tableName = await createTable(query);
  node.state.materializedQueryHash = queryHash;
  return tableName;
}

// Critical: State updated BEFORE dropping table to prevent race conditions
dropMaterialization(node) {
  node.state.materialized = false;  // ← Update first
  await engine.query(`DROP TABLE ${tableName}`);  // ← Then drop
}
```

**Auto-Execute Logic** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_execution_service.ts:892-1028`)

| autoExecute | manual | materialized | Behavior                              |
|-------------|--------|--------------|---------------------------------------|
| true        | false  | -            | Analyze + execute if query changed    |
| true        | true   | -            | Analyze + execute (forced)            |
| false       | false  | true         | Load existing data from table         |
| false       | false  | false        | Skip - show "Run Query" button        |
| false       | true   | -            | Analyze + execute (user clicked)      |

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

**Node Invalidation** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_execution_service.ts:369-440`)
```typescript
invalidateNode(node) {
  const downstreamNodes = getAllDownstreamNodes(node);
  for (const downstream of downstreamNodes) {
    queryHashCache.delete(downstream.nodeId);  // Force hash recomputation
    downstream.state.materializedQueryHash = undefined;  // Mark table stale
  }
}
```

**Query Hash Caching** (`ui/src/plugins/dev.perfetto.ExplorePage/query_builder/query_builder_utils.ts`)
- `hashNodeQuery()`: Expensive JSON stringification of entire query tree
- Cached per node to avoid redundant computation during rapid analysis
- Cache invalidated when node or upstream dependencies change

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
  const result = await engine.analyzeStructuredQuery(structuredQueries);
  return {sql, textproto, modules, preambles, columns};
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

### 3. Two-Phase Execution with Materialization
- Analysis phase: Validate query structure without execution
- Execution phase: Materialize into PERFETTO table for pagination
- Materialized tables reused when query hash unchanged
- Server-side pagination via SQLDataSource (no full result fetch)

### 4. FIFO Queue with Staleness Detection
- Prevents race conditions during rapid user input
- Operations execute in order (preserves node dependencies)
- Staleness check: skip operations with outdated query hashes
- Per-operation error isolation (one failure doesn't block queue)

### 5. Structured Query Protocol
- Nodes generate protobuf `PerfettoSqlStructuredQuery`
- Engine validates and converts to SQL via `analyzeStructuredQuery()`
- Hash-based change detection (entire query tree serialized to JSON)
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

// Copyright (C) 2024 The Android Open Source Project
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

import './styles.scss';
import m from 'mithril';
import {z} from 'zod';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import type {Store} from '../../base/store';
import {shortUuid} from '../../base/uuid';
import {getErrorMessage} from '../../base/errors';
import {debounce} from '../../base/rate_limiters';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import IntellettoPlugin from '../dev.perfetto.Intelletto';
import {
  DataExplorer,
  type DataExplorerState,
  type DataExplorerTab,
} from './data_explorer';
import {nodeRegistry} from './query_builder/node_registry';
import {
  deserializeState,
  serializeState,
  validateSerializedGraph,
} from './json_handler';
import {
  collectGraphErrors,
  formatGraphErrors,
  type GraphNodeError,
} from './graph_check';
import {recentGraphsStorage} from './recent_graphs';
import {getAllNodes} from './query_builder/graph_utils';
import {isDashboardNode} from './query_builder/nodes/dashboard_node';
import {
  dataExplorerTabsStorage,
  createNewTabName,
  createEmptyState,
  serializeAllDashboards,
  deserializeDashboardsForTab,
} from './data_explorer_tabs_storage';
import {dashboardRegistry} from './dashboard/dashboard_registry';
import type {DashboardTabState} from './data_explorer';
import type {
  PersistedDataExplorerTabData,
  PersistedDashboardData,
} from './data_explorer_tabs_storage';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

// --- Assistant tool docs ---

// The full JSON-graph format spec, folded into the set_graph tool description.
// This is the load-bearing part: a model can only write a valid graph if the
// envelope, the connection model, the shared sub-types, and every node's
// `state` shape are all in front of it. This block is built "from the ground
// up" from the node code-behind (each node's `attrs` interface IS its
// serialized `state`), so it documents every node type the registry knows
// about. `sql_source` is still flagged as the recommended default because one
// SQL query can stand in for most of the other nodes and is the hardest to get
// wrong, but the rest are fully specified for when the user asks for them.
const GRAPH_FORMAT = `
================================================================================
DATA EXPLORER GRAPH JSON FORMAT
================================================================================

Send the whole graph as a JSON string. Top-level shape:

{
  "nodes": [ <node>, ... ],          // every node in the graph
  "rootNodeIds": [ "<id>", ... ],    // ids of nodes with no input feeding them
  "nodeLayouts": { "<id>": {"x":0,"y":0} },  // OPTIONAL - omit for auto-layout
  "selectedNodeId": "<id>"           // OPTIONAL - node to select after loading
}

Each <node>:

{
  "nodeId": "<unique string id, e.g. \\"0\\", \\"1\\">",
  "type": "<one of the node types below>",
  "state": { ...type-specific fields, see catalog... },
  "nextNodes": [ "<downstream nodeId>", ... ],
  "primaryInputId": "<nodeId>",                  // OPTIONAL: the node above this one
  "secondaryInputIds": { "0": "<nodeId>", ... }  // OPTIONAL: side inputs, keyed by port
}

--------------------------------------------------------------------------------
CONNECTION MODEL (read carefully - this is where graphs break)
--------------------------------------------------------------------------------
Edges are stored BOTH directions and must agree:
- The upstream node lists the downstream id in its "nextNodes".
- The downstream node names the upstream id in EITHER "primaryInputId" (the
  single input that flows in from above) OR "secondaryInputIds" (side inputs,
  keyed by port number "0","1",... ).
If only one side is set the edge is dropped on load. Always set both.

- "rootNodeIds" = exactly the nodes that have NO input (no primaryInputId and no
  secondaryInputIds). Source nodes are always roots. Every other node must be
  reachable from a root via nextNodes, or it is lost on load.
- No cycles.

Which input a node uses:
- Source nodes (table, simple_slices, sql_source, time_range_source): no input.
- Single-input operations (aggregation, modify_columns, filter, sort,
  limit_and_offset, counter_to_intervals, visualisation, dashboard, metrics):
  use "primaryInputId".
- add_columns, filter_during, filter_in: "primaryInputId" for the main data,
  PLUS "secondaryInputIds":{"0": <the other input>}.
- Multi-source nodes (join, union, interval_intersect, create_slices,
  trace_summary): NO primaryInputId; all inputs go in "secondaryInputIds" by
  port. join/create_slices use ports "0" and "1"; union/interval_intersect take
  two or more ports "0","1","2",...

--------------------------------------------------------------------------------
SHARED SUB-TYPES (used inside several nodes' "state")
--------------------------------------------------------------------------------
ColumnInfo = {
  "name": "<column name>",
  "checked": true,            // true = keep/use this column (usually what you want)
  "alias": "<rename>",        // OPTIONAL
  "type": { "kind": "<k>" }   // OPTIONAL; kind is one of:
                              //   int double boolean string bytes timestamp duration arg_set_id
}
You can omit "type"; it is re-derived from the upstream schema on load.

--------------------------------------------------------------------------------
RECOMMENDED DEFAULT: real nodes (STRONGLY PREFERRED)
--------------------------------------------------------------------------------
STRONGLY prefer building the graph out of real, structured nodes (a "table" or
"slice" source feeding "filter", "sort", "aggregation", "modify_columns",
"limit_and_offset", joins, etc.) rather than packing everything into one
"sql_source" node. Each real node shows up in the UI as its own editable step:
the user can see exactly what every stage does, tweak it, reorder it, and learn
from it. A single "sql_source" collapses all of that into an opaque blob of SQL
the user cannot inspect or adjust without reading raw SQL.

So: decompose the request into one node per logical operation. Use a source node
for the data, then a separate node for each filter / sort / group-by / column
selection / limit.

Fall back to "sql_source" ONLY when no real node can express the operation (an
exotic SQL construct, a CTE, a window function, etc.), or when the user
explicitly asks for raw SQL. type "sql_source", state:
  { "sql": "<a single PerfettoSQL SELECT>" }
Rules for "sql":
- Exactly ONE SELECT (a leading "WITH ... SELECT" is allowed). No trailing ";".
- To use stdlib tables, prepend "INCLUDE PERFETTO MODULE <name>;" statements
  (each ending in ";"), then the SELECT last.
- It can read upstream nodes connected via secondaryInputIds as $input_0,
  $input_1, ... (port number).
- Confirm tables/columns with run_query / get_schema first; do not guess schema.

Preferred shape (the common case) - real nodes, one operation each:
{
  "nodes": [
    { "nodeId": "0", "type": "table",
      "state": { "sqlTable": "slice" },
      "nextNodes": ["1"] },
    { "nodeId": "1", "type": "sort",
      "state": { "sortCriteria": [ { "colName": "dur", "direction": "DESC" } ] },
      "primaryInputId": "0", "nextNodes": ["2"] },
    { "nodeId": "2", "type": "limit_and_offset",
      "state": { "limit": 20, "offset": 0 },
      "primaryInputId": "1", "nextNodes": [] }
  ],
  "rootNodeIds": ["0"]
}

Equivalent with sql_source (use ONLY as a fallback, less visible to the user):
{
  "nodes": [
    { "nodeId": "0", "type": "sql_source",
      "state": { "sql": "SELECT name, dur FROM slice ORDER BY dur DESC LIMIT 20" },
      "nextNodes": [] }
  ],
  "rootNodeIds": ["0"]
}

================================================================================
FULL NODE CATALOG (the "state" object for each "type")
================================================================================

SOURCES (no input; always go in rootNodeIds)

- "sql_source"  -> see RECOMMENDED DEFAULT above. state: { "sql": string }

- "table"  -> read a whole trace table.
  state: { "sqlTable": "<table name, e.g. \\"slice\\">" }

- "simple_slices"  -> all slices in the trace. state: {} (no fields)

- "time_range_source"  -> a time window (ns).
  state: { "start": "<ns as string>", "end": "<ns as string>", "isDynamic": false }
  isDynamic true = follows the timeline selection; false = fixed snapshot.

SINGLE-INPUT OPERATIONS (set "primaryInputId" to the upstream node)

- "filter"  -> keep rows matching a condition.
  Freeform (preferred, simplest):
    state: { "filterMode": "freeform", "sqlExpression": "dur > 1000000 AND name GLOB 'foo*'" }
  Structured:
    state: {
      "filterMode": "structured",
      "filterOperator": "AND",            // or "OR"
      "filters": [
        { "column": "dur", "op": ">", "value": 1000000 }
      ]
    }
    op is one of: "=" "!=" "<" "<=" ">" ">=" "glob" | "in" "not in" (value is an
    array) | "is null" "is not null" (no value).

- "sort"  -> order rows.
  state: { "sortCriteria": [ { "colName": "dur", "direction": "DESC" } ] }  // ASC | DESC

- "limit_and_offset"  -> cap row count.
  state: { "limit": 100, "offset": 0 }

- "aggregation"  -> GROUP BY + aggregate.
  state: {
    "groupByColumns": [ { "name": "name", "checked": true } ],   // ColumnInfo[]
    "aggregations": [
      { "column": { "name": "dur" }, "aggregationOp": "SUM", "newColumnName": "total_dur" }
    ]
  }
  aggregationOp: COUNT | COUNT(*) | COUNT_DISTINCT | SUM | MIN | MAX | MEAN |
  MEDIAN | DURATION_WEIGHTED_MEAN | PERCENTILE. For COUNT(*) omit "column". For
  PERCENTILE add "percentile": <number 0-100>.

- "modify_columns"  -> select / rename columns.
  state: { "selectedColumns": [ { "name": "name", "checked": true, "alias": "slice_name" } ] }  // ColumnInfo[]

- "counter_to_intervals"  -> turn counter rows (ts, value) into intervals
  (adds dur, next_value, delta_value). Input must have id, ts, track_id, value
  and NOT already have dur. state: {} (no fields)

- "visualisation"  -> charts over the input rows.
  state: {
    "chartConfigs": [
      { "id": "c1", "column": "name", "chartType": "bar", "aggregation": "COUNT" }
    ]
  }
  chartType: bar | histogram | line | scatter | pie | treemap | boxplot |
  heatmap | cdf | scorecard. aggregation (bar/pie/treemap): COUNT (default) |
  SUM | MIN | MAX | MEAN ... For non-count add "measureColumn". histogram uses
  "column" (+ optional "binCount"); line/scatter need "yColumn".

SINGLE-INPUT + ONE SIDE INPUT (set "primaryInputId" AND "secondaryInputIds":{"0":...})

- "add_columns"  -> LEFT JOIN columns from the side node onto the main rows.
  state: {
    "selectedColumns": [ "<col from side node>", ... ],
    "leftColumn": "<join key in main input>",
    "rightColumn": "<join key in side input>",
    "columnAliases": { "<col>": "<alias>" }   // OPTIONAL
  }

- "filter_during"  -> keep only main-input intervals that overlap intervals from
  the side node. Both inputs need ts and dur.
  state: { "partitionColumns": ["utid"], "clipToIntervals": true }
  clipToIntervals true (default) = output uses the intersected ts/dur.

- "filter_in"  -> keep main rows whose value appears in the side node.
  state: { "baseColumn": "<col in main input>", "matchColumn": "<col in side input>" }

MULTI-SOURCE (no primaryInputId; inputs in "secondaryInputIds" by port)

- "join"  -> join two inputs. Ports "0" (left) and "1" (right).
  state: {
    "joinType": "INNER",            // or "LEFT"
    "conditionType": "equality",    // or "freeform"
    "leftColumn": "<left key>",     // for equality
    "rightColumn": "<right key>",   // for equality
    "sqlExpression": "",            // for freeform, e.g. "left.id = right.parent_id"
    "leftQueryAlias": "left",
    "rightQueryAlias": "right",
    "leftColumns": [ { "name": "...", "checked": true } ],   // ColumnInfo[], columns to keep
    "rightColumns": [ { "name": "...", "checked": true } ]   // ColumnInfo[]
  }

- "union"  -> stack rows from 2+ inputs (ports "0","1",...).
  state: { "selectedColumns": [ { "name": "...", "checked": true } ] }  // ColumnInfo[] common cols

- "interval_intersect"  -> intersect intervals from 2+ inputs (ports "0","1",...).
  All inputs need ts and dur.
  state: { "partitionColumns": ["utid"], "tsDurSource": "intersection" }
  tsDurSource: "intersection" or an input index number.

- "create_slices"  -> build slices by pairing start/end timestamps from two
  inputs. Ports "0" (starts) and "1" (ends).
  state: { "startsTsColumn": "ts", "endsTsColumn": "ts" }
  ( optional: "startsDurColumn", "endsDurColumn", "startsMode", "endsMode" )

EXPORT NODES (advanced; usually only when the user asks for metrics/dashboards)

- "metrics"  -> define a metric (primaryInputId = the data).
  state: {
    "metricIdPrefix": "my_metric",
    "valueColumns": [ { "column": "dur", "unit": "NS", "polarity": "POSITIVE" } ],
    "dimensionConfigs": { "<dim col>": { "displayName": "..." } },
    "dimensionUniqueness": ""
  }

- "trace_summary"  -> bundle metrics. Inputs are metrics nodes via
  "secondaryInputIds" ("0","1",...). state: {} (no fields)

- "dashboard"  -> export the input for use on dashboards (primaryInputId = data).
  state: { "exportName": "<name>" }

================================================================================
WORKFLOW
================================================================================
- Call get_graph FIRST when editing an existing graph; copy its exact shape and
  reuse its nodeIds rather than rebuilding from scratch.
- Prefer a single sql_source node for most requests.
- Use real node types only when the user explicitly wants that operation/UI.
- An invalid graph comes back as a tool error - read it, fix the JSON, retry.
`.trim();

// --- Permalink persistence ---

const STORE_VERSION = 2;

interface DataExplorerPersistedState {
  version: number;
  // Multi-tab format (version 2+)
  tabs?: PersistedDataExplorerTabData[];
  activeTabId?: string;
  // Flat list of dashboards, each referencing its parent graph tab.
  dashboards?: PersistedDashboardData[];
  // Old single-graph format (version 1) - kept for backward compat
  graphJson?: string;
}

function isValidPersistedState(
  init: unknown,
): init is DataExplorerPersistedState {
  if (typeof init !== 'object' || init === null || !('version' in init)) {
    return false;
  }
  const version = (init as {version: unknown}).version;
  // Accept both v1 (old single-graph) and v2 (multi-tab)
  return version === 1 || version === STORE_VERSION;
}

// --- Plugin ---

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.DataExplorer';
  static readonly dependencies = [
    QueryPagePlugin,
    SqlModulesPlugin,
    // Depended on so we can contribute get_graph / set_graph tools to the
    // assistant from onTraceLoad (see registerIntellettoTools).
    IntellettoPlugin,
  ];

  // Multi-tab state
  private tabs: DataExplorerTab[] = [];
  private activeTabId = '';

  // The loaded trace, stored so the public get/set graph API (used by other
  // plugins, e.g. the Intelletto assistant) can deserialize against it.
  private trace?: Trace;

  // Track whether we've successfully loaded state from local storage
  private hasAttemptedStateLoad = false;

  // Store for persisting state in permalinks
  private permalinkStore?: Store<DataExplorerPersistedState>;

  // Debounced saves to avoid expensive serialization on every state change
  private debouncedSave = debounce(() => {
    dataExplorerTabsStorage.save(this.tabs, this.activeTabId);
  }, 1000);

  private debouncedPermalinkSave = debounce(() => {
    this.saveToPermalinkStore();
  }, 1000);

  // Flush pending saves on page unload to avoid data loss
  private readonly onBeforeUnload = () => {
    try {
      dataExplorerTabsStorage.save(this.tabs, this.activeTabId);
      this.saveToPermalinkStore();
    } catch (e) {
      console.warn('Failed to flush data explorer tabs on unload:', e);
    }
  };

  // --- Tab helpers ---

  private createNewTab(title?: string): DataExplorerTab {
    return {
      id: shortUuid(),
      title: title ?? createNewTabName(this.tabs),
      state: createEmptyState(),
      dashboards: [
        {
          id: shortUuid(),
          items: [],
          brushFilters: new Map(),
        },
      ],
    };
  }

  private getActiveTab(): DataExplorerTab | undefined {
    return this.tabs.find((t) => t.id === this.activeTabId);
  }

  private ensureAtLeastOneTab(): void {
    if (this.tabs.length === 0) {
      const tab = this.createNewTab();
      this.tabs.push(tab);
      this.activeTabId = tab.id;
    }
  }

  // --- Tab CRUD ---

  private handleTabAdd = (): void => {
    const newTab = this.createNewTab();
    this.tabs.push(newTab);
    this.activeTabId = newTab.id;
    this.debouncedSave();
    m.redraw();
  };

  private handleTabClose = (tabId: string): void => {
    const index = this.tabs.findIndex((t) => t.id === tabId);
    if (index === -1) return;

    // Don't close the last tab
    if (this.tabs.length === 1) return;

    this.tabs.splice(index, 1);

    // If we closed the active tab, switch to an adjacent one
    if (this.activeTabId === tabId) {
      const newIndex = Math.min(index, this.tabs.length - 1);
      this.activeTabId = this.tabs[newIndex].id;
    }

    this.debouncedSave();
    m.redraw();
  };

  private handleTabChange = (tabId: string): void => {
    this.activeTabId = tabId;
    this.debouncedSave();
    m.redraw();
  };

  private handleTabRename = (tabId: string, newName: string): void => {
    const tab = this.tabs.find((t) => t.id === tabId);
    if (!tab) return;
    const trimmed = newName.trim();
    if (trimmed === '') return;
    const isDuplicate = this.tabs.some(
      (t) => t.id !== tabId && t.title === trimmed,
    );
    if (isDuplicate) return;
    tab.title = trimmed;
    this.debouncedSave();
    m.redraw();
  };

  private handleTabReorder = (
    draggedTabId: string,
    beforeTabId: string | undefined,
  ): void => {
    const draggedIndex = this.tabs.findIndex((t) => t.id === draggedTabId);
    if (draggedIndex === -1) return;

    const [draggedTab] = this.tabs.splice(draggedIndex, 1);

    if (beforeTabId === undefined) {
      this.tabs.push(draggedTab);
    } else {
      const beforeIndex = this.tabs.findIndex((t) => t.id === beforeTabId);
      if (beforeIndex === -1) {
        this.tabs.push(draggedTab);
      } else {
        this.tabs.splice(beforeIndex, 0, draggedTab);
      }
    }

    this.debouncedSave();
  };

  private handleTabAddWithState = (
    title: string,
    state: DataExplorerState,
    afterTabId: string,
    dashboards?: DashboardTabState[],
  ): void => {
    const newTab: DataExplorerTab = {
      id: shortUuid(),
      title,
      state,
      dashboards: dashboards ?? [
        {
          id: shortUuid(),
          items: [],
          brushFilters: new Map(),
        },
      ],
    };

    const afterIndex = this.tabs.findIndex((t) => t.id === afterTabId);
    if (afterIndex !== -1) {
      this.tabs.splice(afterIndex + 1, 0, newTab);
    } else {
      this.tabs.push(newTab);
    }
    this.activeTabId = newTab.id;

    this.debouncedSave();
    m.redraw();
  };

  // --- Per-tab state update ---

  private makeOnStateUpdate(tabId: string) {
    return (
      update:
        | DataExplorerState
        | ((current: DataExplorerState) => DataExplorerState),
    ) => {
      const tab = this.tabs.find((t) => t.id === tabId);
      if (!tab) return;

      if (typeof update === 'function') {
        tab.state = update(tab.state);
      } else {
        tab.state = update;
      }

      // Save active tab's state to recent graphs (updates the working slot)
      if (tabId === this.activeTabId) {
        recentGraphsStorage.saveCurrentState(tab.state);
      }

      // Save all tabs to permalink store (debounced)
      this.debouncedPermalinkSave();

      // Save all tabs to localStorage (debounced)
      this.debouncedSave();

      m.redraw();
    };
  }

  // --- Permalink store ---

  private mountPermalinkStore(trace: Trace): void {
    if (this.permalinkStore) return;

    this.permalinkStore = trace.mountStore<DataExplorerPersistedState>(
      'dev.perfetto.DataExplorer',
      (init: unknown) => {
        if (isValidPersistedState(init)) {
          return init;
        }
        return {version: STORE_VERSION};
      },
    );
  }

  private saveToPermalinkStore(): void {
    if (!this.permalinkStore) return;

    const hasDashboardContent = (tab: DataExplorerTab): boolean =>
      tab.dashboards.some((db) => db.items.length > 0);

    const tabsData: PersistedDataExplorerTabData[] = this.tabs
      .filter(
        (tab) => tab.state.rootNodes.length > 0 || hasDashboardContent(tab),
      )
      .map((tab) => ({
        id: tab.id,
        title: tab.title,
        graphJson:
          tab.state.rootNodes.length > 0
            ? serializeState(tab.state)
            : undefined,
      }));

    this.permalinkStore.edit((draft) => {
      draft.version = STORE_VERSION;
      draft.tabs = tabsData.length > 0 ? tabsData : undefined;
      draft.activeTabId = this.activeTabId;
      draft.dashboards = serializeAllDashboards(this.tabs);
      // Clear deprecated single-graph field
      draft.graphJson = undefined;
    });
  }

  // --- State loading ---

  /** Hydrate tabs from persisted tab data, returning the list of loaded tabs. */
  private hydrateTabs(
    tabsData: ReadonlyArray<PersistedDataExplorerTabData>,
    trace: Trace,
    sqlModules: SqlModules,
    allDashboards?: ReadonlyArray<PersistedDashboardData>,
  ): DataExplorerTab[] {
    return tabsData.map((tabData) => {
      const state =
        tabData.graphJson !== undefined
          ? deserializeState(tabData.graphJson, trace, sqlModules)
          : createEmptyState();

      // Stamp graphId on dashboard nodes and re-publish their sources.
      // postDeserializeLate already called publishExportedSource but graphId
      // was empty at that point because it's only known from the tab.
      for (const node of getAllNodes(state.rootNodes)) {
        if (isDashboardNode(node)) {
          node.graphId = tabData.id;
          node.onPrevNodesUpdated?.();
        }
      }

      const deserialized = deserializeDashboardsForTab(
        tabData.id,
        allDashboards,
      );
      return {
        id: tabData.id,
        title: tabData.title,
        state,
        dashboards: deserialized,
      };
    });
  }

  private tryLoadState(trace: Trace): void {
    if (this.hasAttemptedStateLoad) return;

    this.mountPermalinkStore(trace);

    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      // SQL modules not ready yet, we'll retry on next render
      return;
    }

    // SQL modules are ready, mark load as attempted regardless of outcome
    this.hasAttemptedStateLoad = true;

    this.loadStateFromSources(trace, sqlModules);

    // Sync loaded state to the permalink store so that "Share trace" includes
    // the Data Explorer state even if the user hasn't modified anything.
    // Without this, state loaded from localStorage or recent graphs would
    // never be written to the permalink store, causing permalinks to lose
    // the Data Explorer state.
    this.saveToPermalinkStore();
  }

  private loadStateFromSources(trace: Trace, sqlModules: SqlModules): void {
    // Priority 1: Check permalink store
    const permalinkState = this.permalinkStore?.state;
    if (permalinkState) {
      // Try multi-tab format first (version 2+)
      if (permalinkState.tabs !== undefined && permalinkState.tabs.length > 0) {
        try {
          this.tabs = this.hydrateTabs(
            permalinkState.tabs,
            trace,
            sqlModules,
            permalinkState.dashboards,
          );
          this.activeTabId =
            permalinkState.activeTabId !== undefined &&
            this.tabs.some((t) => t.id === permalinkState.activeTabId)
              ? permalinkState.activeTabId
              : this.tabs[0].id;
          return;
        } catch (e) {
          const msg = getErrorMessage(e);
          console.warn(
            'Failed to load Data Explorer tabs from permalink:',
            msg,
          );
          this.tabs = [];
          // Fall through to try other sources
        }
      }

      // Try old single-graph format (version 1 backward compat)
      if (permalinkState.graphJson !== undefined) {
        try {
          const state = deserializeState(
            permalinkState.graphJson,
            trace,
            sqlModules,
          );
          const tab = this.createNewTab();
          tab.state = state;
          this.tabs.push(tab);
          this.activeTabId = tab.id;
          return;
        } catch (e) {
          const msg = getErrorMessage(e);
          console.warn(
            'Failed to load Data Explorer state from permalink:',
            msg,
          );
          // Fall through to try other sources
        }
      }
    }

    // Priority 2: Check localStorage tabs
    const persistedTabs = dataExplorerTabsStorage.load();
    if (persistedTabs !== undefined) {
      try {
        this.tabs = this.hydrateTabs(
          persistedTabs.tabs,
          trace,
          sqlModules,
          persistedTabs.dashboards,
        );
        this.activeTabId = this.tabs.some(
          (t) => t.id === persistedTabs.activeTabId,
        )
          ? persistedTabs.activeTabId
          : this.tabs[0].id;
        return;
      } catch (e) {
        console.debug(
          'Failed to load Data Explorer tabs from localStorage:',
          e,
        );
        this.tabs = [];
        // Fall through to try recent graphs
      }
    }

    // Priority 3: Backward compat - try old recentGraphsStorage
    try {
      const json = recentGraphsStorage.getCurrentJson();
      if (json) {
        const state = deserializeState(json, trace, sqlModules);
        const tab = this.createNewTab();
        tab.state = state;
        this.tabs.push(tab);
        this.activeTabId = tab.id;
        return;
      }
    } catch (e) {
      console.debug(
        'Failed to load Data Explorer state from recent graphs:',
        e,
      );
      recentGraphsStorage.clear();
    }

    // Priority 4: Create one empty default tab
    this.ensureAtLeastOneTab();
  }

  // --- Public API for other plugins ---

  /**
   * Returns the active tab's graph serialized as JSON (the same format the
   * "Export" button produces and importStateFromJson consumes), or undefined
   * if there is no trace loaded or the active graph is empty.
   *
   * Used by dependent plugins (e.g. the Intelletto assistant) to read what the
   * user is currently exploring.
   */
  getActiveGraphJson(): string | undefined {
    if (this.trace !== undefined) {
      // Lazily hydrate tabs in case the user hasn't opened the page yet.
      this.tryLoadState(this.trace);
    }
    const tab = this.getActiveTab();
    if (tab === undefined || tab.state.rootNodes.length === 0) {
      return undefined;
    }
    return serializeState(tab.state);
  }

  /**
   * Replaces the active tab's graph with the one described by `json` (same
   * format as getActiveGraphJson) and navigates to the Data Explorer so the
   * user sees the result. Throws if the JSON is invalid or SQL modules are not
   * ready yet.
   *
   * Used by dependent plugins (e.g. the Intelletto assistant) to build a graph
   * on the user's behalf.
   */
  setActiveGraphJson(json: string): void {
    const trace = this.trace;
    if (trace === undefined) {
      throw new Error('No trace loaded.');
    }
    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    sqlModulesPlugin.ensureInitialized();
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      throw new Error(
        'SQL modules are not ready yet. Open the Data Explorer once and retry.',
      );
    }
    // Structural pre-check: aggregate ALL problems (bad JSON, unknown node
    // types, dangling/one-sided edges, ...) into one clear message rather than
    // throwing on whatever deserializeState happens to trip over first.
    const {errors} = validateSerializedGraph(json);
    if (errors.length > 0) {
      throw new Error(
        `Invalid graph (${errors.length} problem` +
          `${errors.length === 1 ? '' : 's'}):\n- ${errors.join('\n- ')}`,
      );
    }
    // deserializeState can still throw on deeper per-node-state issues; let it
    // propagate so the caller (and, for the assistant, the model) sees it.
    const state = deserializeState(json, trace, sqlModules);
    // Make sure tabs are hydrated and there is a tab to write into.
    this.tryLoadState(trace);
    this.ensureAtLeastOneTab();
    // makeOnStateUpdate handles persistence (localStorage + permalink) and
    // triggers a redraw.
    this.makeOnStateUpdate(this.activeTabId)(state);
    trace.navigate('#!/explore');
  }

  /**
   * Runs the active graph against the trace engine and returns one error per
   * failing node (bad SQL, missing column/table, invalid config). An empty
   * array means the graph runs cleanly. Lets a caller (e.g. the assistant)
   * verify a graph it built and iterate until it works.
   */
  async checkActiveGraph(): Promise<GraphNodeError[]> {
    const trace = this.trace;
    if (trace === undefined) {
      throw new Error('No trace loaded.');
    }
    const tab = this.getActiveTab();
    if (tab === undefined || tab.state.rootNodes.length === 0) {
      return [];
    }
    return collectGraphErrors(trace.engine, getAllNodes(tab.state.rootNodes));
  }

  /**
   * Checks a graph JSON for problems WITHOUT applying it: structural validation
   * first (aggregated), then - if structurally sound - it is deserialized into
   * a throwaway state and run against the engine to catch runtime errors. The
   * active graph and the UI are left untouched. Returns a human-readable
   * report. Lets the assistant verify a graph before committing it.
   */
  async dryRunGraph(json: string): Promise<string> {
    const trace = this.trace;
    if (trace === undefined) {
      throw new Error('No trace loaded.');
    }

    const {errors: structuralErrors} = validateSerializedGraph(json);
    if (structuralErrors.length > 0) {
      return (
        `Invalid graph (${structuralErrors.length} problem` +
        `${structuralErrors.length === 1 ? '' : 's'}):\n- ` +
        structuralErrors.join('\n- ')
      );
    }

    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    sqlModulesPlugin.ensureInitialized();
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      throw new Error(
        'SQL modules are not ready yet. Open the Data Explorer once and retry.',
      );
    }

    let state;
    try {
      // Build the nodes in isolation - this is NOT assigned to any tab.
      state = deserializeState(json, trace, sqlModules);
    } catch (e) {
      return `Graph could not be built: ${getErrorMessage(e)}`;
    }

    const runtimeErrors = await collectGraphErrors(
      trace.engine,
      getAllNodes(state.rootNodes),
    );
    if (runtimeErrors.length === 0) {
      return 'OK: graph is valid and runs cleanly.';
    }
    return (
      'Graph is structurally valid, but some nodes fail to run:\n' +
      formatGraphErrors(runtimeErrors)
    );
  }

  // --- Assistant tools ---

  // Contribute get_graph / set_graph tools to the Intelletto assistant so it
  // can read and build Data Explorer graphs on the user's behalf.
  private registerIntellettoTools(trace: Trace): void {
    const intelletto = trace.plugins.getPlugin(IntellettoPlugin);

    intelletto.registerTool({
      name: 'get_graph',
      description:
        'Read the current Data Explorer query graph as JSON. Call this to see ' +
        'what the user is exploring before answering questions about it, and ' +
        'ALWAYS before set_graph when editing an existing graph - copy its ' +
        'shape rather than rebuilding from scratch. Returns the string ' +
        '"<empty>" when there is no graph yet.',
      shape: {},
      callback: async () => this.getActiveGraphJson() ?? '<empty>',
    });

    intelletto.registerTool({
      name: 'set_graph',
      description:
        'Replace the Data Explorer query graph with a new one and switch the ' +
        'UI to the Data Explorer so the user sees it. Use this when the user ' +
        'asks you to build, change, or visualise a query/pipeline in the Data ' +
        'Explorer. The argument is the whole graph as a JSON string. A ' +
        'structurally invalid graph (bad JSON, unknown node type, dangling or ' +
        'one-sided edge) comes back as a tool error listing every problem - ' +
        'fix them and retry. If the graph is structurally fine but a node ' +
        'fails to run (bad SQL, missing column/table), it is still applied and ' +
        'the per-node runtime errors are returned; fix the SQL and call ' +
        'set_graph again until it reports it runs cleanly.\n\n' +
        GRAPH_FORMAT,
      mutating: true,
      shape: {
        graph: z
          .string()
          .describe(
            'The complete graph, as a JSON string in the documented format ' +
              '(an object with "nodes" and "rootNodeIds").',
          ),
      },
      callback: async ({graph}) => {
        // Throws (-> tool error) on structural problems, before any UI change.
        this.setActiveGraphJson(graph);
        // Applied; now report any runtime errors so the model can iterate.
        const errors = await this.checkActiveGraph();
        if (errors.length === 0) {
          return 'OK: graph applied and runs cleanly.';
        }
        return (
          'Graph applied, but some nodes fail to run. Fix these and call ' +
          'set_graph again:\n' +
          formatGraphErrors(errors)
        );
      },
    });

    intelletto.registerTool({
      name: 'check_graph',
      description:
        'Run the current Data Explorer graph against the trace and report any ' +
        'per-node errors (bad SQL, missing columns/tables, invalid config) ' +
        'without changing it. Returns "OK: graph runs cleanly." when there are ' +
        'none. Use this to verify the graph after editing, or to diagnose what ' +
        'the user means by "my graph is broken".',
      shape: {},
      callback: async () => {
        const errors = await this.checkActiveGraph();
        if (errors.length === 0) {
          return 'OK: graph runs cleanly.';
        }
        return 'Graph has errors:\n' + formatGraphErrors(errors);
      },
    });

    intelletto.registerTool({
      name: 'validate_graph',
      description:
        'Check a candidate graph JSON for problems WITHOUT applying it - the ' +
        'current graph and the UI are left untouched. Reports structural ' +
        'problems (bad JSON, unknown node type, dangling or one-sided edges) ' +
        'and, if structurally sound, runtime errors from running it (bad SQL, ' +
        'missing columns/tables). Returns "OK: graph is valid and runs ' +
        'cleanly." when there are none. Use this to iterate on a graph before ' +
        'committing it with set_graph. See set_graph for the JSON format.',
      shape: {
        graph: z
          .string()
          .describe(
            'The candidate graph as a JSON string, same format as set_graph.',
          ),
      },
      callback: async ({graph}) => this.dryRunGraph(graph),
    });
  }

  // --- Plugin lifecycle ---

  async onTraceLoad(trace: Trace): Promise<void> {
    this.trace = trace;

    this.registerIntellettoTools(trace);

    // Flush pending localStorage saves on page unload
    window.addEventListener('beforeunload', this.onBeforeUnload);
    trace.trash.defer(() => {
      window.removeEventListener('beforeunload', this.onBeforeUnload);
    });

    trace.trash.defer(() => dashboardRegistry.clear());

    trace.pages.registerPage({
      route: '/explore',
      render: () => {
        // Ensure SQL modules initialization is triggered (no-op if already
        // started). This kicks off the data availability checks that determine
        // which modules should be marked as "No data".
        trace.plugins.getPlugin(SqlModulesPlugin).ensureInitialized();

        // Try to load saved state lazily (waits for SQL modules to be ready).
        this.tryLoadState(trace);

        const activeTab = this.getActiveTab();
        if (!activeTab) {
          return m('.pf-data-explorer', 'Loading...');
        }

        return m(DataExplorer, {
          trace,
          tabs: this.tabs,
          activeTabId: this.activeTabId,
          state: activeTab.state,
          sqlModulesPlugin: trace.plugins.getPlugin(SqlModulesPlugin),
          onStateUpdate: this.makeOnStateUpdate(this.activeTabId),
          makeOnStateUpdate: (tabId: string) => this.makeOnStateUpdate(tabId),
          onTabAdd: this.handleTabAdd,
          onTabClose: this.handleTabClose,
          onTabChange: this.handleTabChange,
          onTabRename: this.handleTabRename,
          onTabReorder: this.handleTabReorder,
          onTabAddWithState: this.handleTabAddWithState,
          onDashboardStateChange: () => {
            this.debouncedSave();
            this.debouncedPermalinkSave();
          },
        });
      },
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 20,
      text: 'Data Explorer',
      href: '#!/explore',
      icon: 'data_exploration',
    });

    // Register "Move selection to Data Explorer" command
    trace.commands.registerCommand({
      id: 'dev.perfetto.DataExplorer.MoveSelectionToDataExplorer',
      name: 'Move selection to Data Explorer',
      callback: () => {
        const timeSpan = trace.selection.getTimeSpanOfSelection();
        if (!timeSpan) {
          // No valid time selection - inform user
          console.warn(
            'No time selection found. Please select a time range on the timeline first.',
          );
          return;
        }

        // Capture the time range values before clearing selection
        const start = timeSpan.start;
        const end = timeSpan.end;

        // Clear the timeline selection FIRST to avoid UI artifacts
        trace.selection.clearSelection();

        // Get the TimeRange node descriptor
        const descriptor = nodeRegistry.get('timerange');
        if (!descriptor) {
          console.error('TimeRange node not found in registry');
          return;
        }

        // Create the TimeRange node with captured values
        const newNode = descriptor.factory(
          {start, end},
          {allNodes: [], context: {trace}},
        );

        // Ensure we have an active tab
        this.ensureAtLeastOneTab();

        // Add node to active tab's state
        const onStateUpdate = this.makeOnStateUpdate(this.activeTabId);
        onStateUpdate((currentState) => ({
          ...currentState,
          rootNodes: [...currentState.rootNodes, newNode],
          selectedNodes: new Set([newNode.nodeId]),
        }));

        // Navigate to Data Explorer
        trace.navigate('#!/explore');
      },
    });
  }
}

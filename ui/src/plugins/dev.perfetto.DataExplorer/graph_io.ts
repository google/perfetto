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

import m from 'mithril';
import {assetSrc} from '../../base/assets';
import {showModal} from '../../widgets/modal';
import {Trace} from '../../public/trace';
import {QueryNode} from './query_node';
import {
  serializeState,
  exportStateAsJson,
  deserializeState,
  downloadJsonFile,
} from './json_handler';
import {nodeRegistry} from './query_builder/node_registry';
import {SlicesSourceNode} from './query_builder/nodes/sources/slices_source';
import {
  showStateOverwriteWarning,
  showExportWarning,
} from './query_builder/widgets';
import {recentGraphsStorage} from './recent_graphs';
import {DataExplorerState, DashboardTabState} from './data_explorer';
import type {DataExplorerTab} from './data_explorer';
import {parsePbtxtToState} from './pbtxt_import';
import {
  serializeDashboardsForTab,
  SerializedDashboard,
} from './data_explorer_tabs_storage';
import {
  validateDashboardItems,
  parseBrushFilters,
} from './dashboard/dashboard_registry';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

// Dependencies needed by graph I/O operations.
export interface GraphIODeps {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;
  readonly onStateUpdate: (
    update:
      | DataExplorerState
      | ((currentState: DataExplorerState) => DataExplorerState),
  ) => void;
  readonly cleanupExistingNodes: (rootNodes: QueryNode[]) => Promise<void>;
}

// Shows confirmation dialog if there are unsaved changes, and finalizes
// the current graph before loading a new one. Returns true if the user
// confirmed (or there was nothing to confirm), false if cancelled.
export async function confirmAndFinalizeCurrentGraph(
  state: DataExplorerState,
): Promise<boolean> {
  if (state.rootNodes.length > 0 || state.labels.length > 0) {
    const confirmed = await showStateOverwriteWarning();
    if (!confirmed) return false;
    recentGraphsStorage.finalizeCurrentGraph();
  }
  return true;
}

export async function exportGraph(
  state: DataExplorerState,
  trace: Trace,
): Promise<void> {
  const confirmed = await showExportWarning();
  if (!confirmed) return;
  exportStateAsJson(state, trace);
}

// Common method to load state from a JSON string.
// Handles cleanup of existing nodes and state update.
export async function loadGraphFromJson(
  deps: GraphIODeps,
  currentRootNodes: QueryNode[],
  json: string,
): Promise<void> {
  await deps.cleanupExistingNodes(currentRootNodes);

  const newState = deserializeState(json, deps.trace, deps.sqlModules);
  deps.onStateUpdate((currentState) => ({
    ...newState,
    loadGeneration: (currentState.loadGeneration ?? 0) + 1,
  }));
}

export async function importGraph(
  deps: GraphIODeps,
  onCreateTab: (title: string, state: DataExplorerState) => void,
): Promise<void> {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.json,.pbtxt';
  input.onchange = async (event) => {
    const files = (event.target as HTMLInputElement).files;
    if (files && files.length > 0) {
      const file = files[0];
      const reader = new FileReader();
      reader.onload = async (e) => {
        const text = e.target?.result as string;
        if (!text) {
          console.error('The selected file is empty or could not be read.');
          return;
        }
        try {
          const isPbtxt = file.name.toLowerCase().endsWith('.pbtxt');
          const newState = isPbtxt
            ? await parsePbtxtToState(text, deps.trace, deps.sqlModules)
            : deserializeState(text, deps.trace, deps.sqlModules);
          const name = file.name.replace(/\.(json|pbtxt)$/i, '');
          onCreateTab(name, newState);
        } catch (error) {
          console.error('Failed to import graph:', error);
          showModal({
            title: 'Import Failed',
            content: () =>
              m(
                'div',
                `Failed to import: ${error instanceof Error ? error.message : String(error)}`,
              ),
            buttons: [],
          });
        }
      };
      reader.readAsText(file);
    }
  };
  input.click();
}

// Centralized method to load JSON from a URL path.
// Handles confirmation, fetching, and error handling.
export async function loadGraphFromPath(
  deps: GraphIODeps,
  state: DataExplorerState,
  jsonPath: string,
  errorTitle: string = 'Failed to Load',
): Promise<void> {
  if (!(await confirmAndFinalizeCurrentGraph(state))) return;

  try {
    const response = await fetch(assetSrc(jsonPath));
    if (!response.ok) {
      throw new Error(
        `Failed to load: ${response.status} ${response.statusText}`,
      );
    }
    const json = await response.text();
    await loadGraphFromJson(deps, state.rootNodes, json);
  } catch (error) {
    console.error(`Failed to load from ${jsonPath}:`, error);
    showModal({
      title: errorTitle,
      content: () =>
        m(
          'div',
          `An error occurred while loading: ${error instanceof Error ? error.message : String(error)}`,
        ),
      buttons: [],
    });
  }
}

export async function createDataExplorerGraph(
  deps: GraphIODeps,
): Promise<void> {
  const {sqlModules, trace} = deps;
  const coreNodes: QueryNode[] = [];
  const rightNodes: QueryNode[] = [];

  const tableDescriptor = nodeRegistry.get('table');

  // Helper to create table nodes for a given importance level.
  function createTableNodes(importance: string, target: QueryNode[]): void {
    if (!tableDescriptor) return;
    const tables = sqlModules
      .listTables()
      .filter((table) => table.importance === importance);

    for (const sqlTable of tables) {
      try {
        const module = sqlModules.getModuleForTable(sqlTable.name);
        if (module && sqlModules.isModuleDisabled(module.includeKey)) {
          continue;
        }

        const tableNode = tableDescriptor.factory(
          {sqlTable, sqlModules, trace},
          {allNodes: [...coreNodes, ...rightNodes]},
        );
        target.push(tableNode);
      } catch (error) {
        console.error(
          `Failed to create table node for ${sqlTable.name}:`,
          error,
        );
      }
    }
  }

  // Create core table nodes (left column)
  createTableNodes('core', coreNodes);

  // Create slices source node (right side)
  const slicesNode = new SlicesSourceNode({sqlModules, trace});
  rightNodes.push(slicesNode);

  // Create high-importance table nodes (right side)
  createTableNodes('high', rightNodes);

  const allNodes = [...coreNodes, ...rightNodes];

  if (allNodes.length > 0) {
    const newNodeLayouts = new Map<string, {x: number; y: number}>();
    const NODE_WIDTH = 300;
    const CORE_COL_WIDTH = 200;
    const START_X = 50;
    const START_Y = 50;

    // Left column: "Core tables" label + core nodes tightly stacked.
    // LABEL_HEIGHT accounts for the label above the first core node.
    const CORE_GAP_Y = 55;
    const LABEL_HEIGHT = CORE_GAP_Y;
    const LABEL_WIDTH = 100;
    const coreStartY = START_Y + LABEL_HEIGHT;
    coreNodes.forEach((node, index) => {
      newNodeLayouts.set(node.nodeId, {
        x: START_X,
        y: coreStartY + index * CORE_GAP_Y,
      });
    });

    // Right side: slices + high-importance tables in a tight grid,
    // vertically centered relative to the core column.
    const GROUP_GAP = 50;
    const RIGHT_GAP_X = 10;
    const RIGHT_GAP_Y = 80;
    const rightStartX = START_X + CORE_COL_WIDTH + GROUP_GAP;
    const rightCols = Math.max(1, Math.ceil(Math.sqrt(rightNodes.length)));
    const rightRows = Math.ceil(rightNodes.length / rightCols);

    const coreColumnHeight = LABEL_HEIGHT + (coreNodes.length - 1) * CORE_GAP_Y;
    const rightGroupHeight = (rightRows - 1) * RIGHT_GAP_Y;
    const rightStartY =
      START_Y + Math.max(0, (coreColumnHeight - rightGroupHeight) / 2);

    rightNodes.forEach((node, index) => {
      const col = index % rightCols;
      const row = Math.floor(index / rightCols);
      newNodeLayouts.set(node.nodeId, {
        x: rightStartX + col * (NODE_WIDTH + RIGHT_GAP_X),
        y: rightStartY + row * RIGHT_GAP_Y,
      });
    });

    // Add labels above each group when the group has nodes.
    const labels: Array<{
      id: string;
      x: number;
      y: number;
      width: number;
      text: string;
    }> = [];
    if (coreNodes.length > 0) {
      labels.push({
        id: 'core-tables-label',
        x: START_X + 30,
        y: START_Y,
        width: LABEL_WIDTH,
        text: 'Core tables',
      });
    }
    if (rightNodes.length > 0) {
      labels.push({
        id: 'right-tables-label',
        x: rightStartX,
        y: rightStartY - LABEL_HEIGHT,
        width: 200,
        text: 'Tables important for this trace',
      });
    }

    deps.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: allNodes,
      nodeLayouts: newNodeLayouts,
      selectedNodes: new Set([allNodes[0].nodeId]),
      labels,
      loadGeneration: (currentState.loadGeneration ?? 0) + 1,
    }));
  }
}

// --- Whole-tab export/import ---

export interface SerializedTabExport {
  version: number;
  title: string;
  graph: string;
  dashboards?: SerializedDashboard[];
}

export function isSerializedTabExport(
  obj: unknown,
): obj is SerializedTabExport {
  if (typeof obj !== 'object' || obj === null) return false;
  const rec = obj as Record<string, unknown>;
  return (
    typeof rec.version === 'number' &&
    typeof rec.title === 'string' &&
    typeof rec.graph === 'string'
  );
}

export function deserializeDashboardsFromExport(
  serialized?: unknown,
): DashboardTabState[] | undefined {
  if (!Array.isArray(serialized) || serialized.length === 0) return undefined;
  const result: DashboardTabState[] = [];
  for (const raw of serialized) {
    if (typeof raw !== 'object' || raw === null) continue;
    const db = raw as Record<string, unknown>;
    if (typeof db.id !== 'string') continue;
    result.push({
      id: db.id,
      items: validateDashboardItems(db.items as unknown[] | undefined) ?? [],
      brushFilters:
        db.brushFilters !== undefined &&
        typeof db.brushFilters === 'object' &&
        !Array.isArray(db.brushFilters)
          ? parseBrushFilters(db.brushFilters as Record<string, unknown[]>)
          : new Map(),
    });
  }
  return result.length > 0 ? result : undefined;
}

export async function exportTab(
  tab: DataExplorerTab,
  trace: Trace,
): Promise<void> {
  const confirmed = await showExportWarning();
  if (!confirmed) return;

  const graphJson = serializeState(tab.state);
  const dashboards = serializeDashboardsForTab(tab);

  const exported: SerializedTabExport = {
    version: 1,
    title: tab.title,
    graph: graphJson,
    dashboards,
  };

  const json = JSON.stringify(exported, null, 2);
  const traceName = trace.traceInfo.traceTitle.replace(
    /[^a-zA-Z0-9._-]+/g,
    '_',
  );
  const tabName = tab.title.replace(/[^a-zA-Z0-9._-]+/g, '_');
  const date = new Date().toISOString().slice(0, 10);
  downloadJsonFile(json, `${traceName}-tab-${tabName}-${date}.json`);
}

export function importTab(
  deps: GraphIODeps,
  onCreateTab: (
    title: string,
    state: DataExplorerState,
    dashboards?: DashboardTabState[],
  ) => void,
): void {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.json,.pbtxt';
  input.onchange = (event) => {
    const files = (event.target as HTMLInputElement).files;
    if (files === null || files.length === 0) return;
    const file = files[0];
    const reader = new FileReader();
    reader.onload = async (e) => {
      const text = e.target?.result;
      if (typeof text !== 'string' || text.length === 0) {
        console.error('The selected file is empty or could not be read.');
        return;
      }
      try {
        const isPbtxt = file.name.toLowerCase().endsWith('.pbtxt');
        if (isPbtxt) {
          const newState = await parsePbtxtToState(
            text,
            deps.trace,
            deps.sqlModules,
          );
          const name = file.name.replace(/\.pbtxt$/i, '');
          onCreateTab(name, newState);
        } else {
          const parsed: unknown = JSON.parse(text);
          if (isSerializedTabExport(parsed)) {
            const newState = deserializeState(
              parsed.graph,
              deps.trace,
              deps.sqlModules,
            );
            const dashboards = deserializeDashboardsFromExport(
              parsed.dashboards,
            );
            const name = parsed.title ?? file.name.replace(/\.json$/i, '');
            onCreateTab(name, newState, dashboards);
          } else {
            // Plain graph import (backward compat)
            const newState = deserializeState(
              text,
              deps.trace,
              deps.sqlModules,
            );
            const name = file.name.replace(/\.json$/i, '');
            onCreateTab(name, newState);
          }
        }
      } catch (error) {
        console.error('Failed to import tab:', error);
        showModal({
          title: 'Import Failed',
          content: () =>
            m(
              'div',
              `Failed to import: ${error instanceof Error ? error.message : String(error)}`,
            ),
          buttons: [],
        });
      }
    };
    reader.readAsText(file);
  };
  input.click();
}

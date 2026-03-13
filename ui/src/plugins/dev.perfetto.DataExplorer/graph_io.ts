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
import {exportStateAsJson, deserializeState} from './json_handler';
import {nodeRegistry} from './query_builder/node_registry';
import {SlicesSourceNode} from './query_builder/nodes/sources/slices_source';
import {
  showStateOverwriteWarning,
  showExportWarning,
} from './query_builder/widgets';
import {recentGraphsStorage} from './recent_graphs';
import {DataExplorerState} from './data_explorer';
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
  state: DataExplorerState,
): Promise<void> {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.json';
  input.onchange = async (event) => {
    const files = (event.target as HTMLInputElement).files;
    if (files && files.length > 0) {
      const file = files[0];

      if (!(await confirmAndFinalizeCurrentGraph(state))) return;

      const reader = new FileReader();
      reader.onload = async (e) => {
        const json = e.target?.result as string;
        if (!json) {
          console.error('The selected file is empty or could not be read.');
          return;
        }
        await loadGraphFromJson(deps, state.rootNodes, json);
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

  // Create core table nodes (left column)
  if (tableDescriptor) {
    const coreTables = sqlModules
      .listTables()
      .filter((table) => table.importance === 'core');

    for (const sqlTable of coreTables) {
      try {
        const module = sqlModules.getModuleForTable(sqlTable.name);
        if (module && sqlModules.isModuleDisabled(module.includeKey)) {
          continue;
        }

        const tableNode = tableDescriptor.factory(
          {sqlTable, sqlModules, trace},
          {allNodes: [...coreNodes, ...rightNodes]},
        );
        coreNodes.push(tableNode);
      } catch (error) {
        console.error(
          `Failed to create table node for ${sqlTable.name}:`,
          error,
        );
      }
    }
  }

  // Create slices source node (right side)
  const slicesNode = new SlicesSourceNode({sqlModules, trace});
  rightNodes.push(slicesNode);

  // Create high-importance table nodes (right side)
  if (tableDescriptor) {
    const highFreqTables = sqlModules
      .listTables()
      .filter((table) => table.importance === 'high');

    for (const sqlTable of highFreqTables) {
      try {
        const module = sqlModules.getModuleForTable(sqlTable.name);
        if (module && sqlModules.isModuleDisabled(module.includeKey)) {
          continue;
        }

        const tableNode = tableDescriptor.factory(
          {sqlTable, sqlModules, trace},
          {allNodes: [...coreNodes, ...rightNodes]},
        );
        rightNodes.push(tableNode);
      } catch (error) {
        console.error(
          `Failed to create table node for ${sqlTable.name}:`,
          error,
        );
      }
    }
  }

  const allNodes = [...coreNodes, ...rightNodes];

  if (allNodes.length > 0) {
    const newNodeLayouts = new Map<string, {x: number; y: number}>();
    const NODE_WIDTH = 300;
    const START_X = 50;
    const START_Y = 50;

    // Left column: "Core tables" label + core nodes tightly stacked
    const CORE_GAP_Y = 55;
    const LABEL_WIDTH = 100;
    const coreStartY = START_Y + CORE_GAP_Y;
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
    const rightStartX = START_X + 200 + GROUP_GAP;
    const rightCols = Math.max(1, Math.ceil(Math.sqrt(rightNodes.length)));
    const rightRows = Math.ceil(rightNodes.length / rightCols);

    const coreColumnHeight = CORE_GAP_Y + (coreNodes.length - 1) * CORE_GAP_Y;
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

    // Create the "Core tables" label, aligned with the node column
    const labelX = START_X + 30;
    const labels = [
      {
        id: 'core-tables-label',
        x: labelX,
        y: START_Y,
        width: LABEL_WIDTH,
        text: 'Core tables',
      },
    ];

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

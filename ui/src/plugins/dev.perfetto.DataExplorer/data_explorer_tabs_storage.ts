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

import {z} from 'zod';
import {shortUuid} from '../../base/uuid';
import {
  DataExplorerTab,
  DashboardTabState,
  DataExplorerState,
} from './data_explorer';
import {serializeState} from './json_handler';
import {
  serializeDashboardItems,
  parseBrushFilters,
  validateDashboardItems,
} from './dashboard/dashboard_registry';

const DATA_EXPLORER_TABS_STORAGE_KEY = 'perfettoDataExplorerTabs';

// Tab schema — unchanged from the original format.
const PERSISTED_DATA_EXPLORER_TAB_SCHEMA = z.object({
  id: z.string(),
  title: z.string(),
  graphJson: z.string().optional(),
});

// Dashboard schema — new, flat list with a reference to the parent graph tab.
const PERSISTED_DASHBOARD_SCHEMA = z.object({
  id: z.string(),
  graphTabId: z.string(),
  items: z.array(z.unknown()).optional(),
  brushFilters: z.record(z.string(), z.array(z.unknown())).optional(),
});

const PERSISTED_DATA_EXPLORER_TABS_STATE_SCHEMA = z.object({
  tabs: z.array(PERSISTED_DATA_EXPLORER_TAB_SCHEMA).min(1),
  activeTabId: z.string(),
  // Optional — old data without dashboards still loads fine.
  dashboards: z.array(PERSISTED_DASHBOARD_SCHEMA).optional(),
});

export type PersistedDashboardData = z.infer<typeof PERSISTED_DASHBOARD_SCHEMA>;

export type PersistedDataExplorerTabData = z.infer<
  typeof PERSISTED_DATA_EXPLORER_TAB_SCHEMA
>;

export type PersistedDataExplorerTabsState = z.infer<
  typeof PERSISTED_DATA_EXPLORER_TABS_STATE_SCHEMA
>;

/**
 * Storage class for data explorer tabs state.
 * Persists tab layout (IDs, titles, serialized graphs) and active tab to
 * localStorage so that the Data Explorer survives page reloads.
 */
class DataExplorerTabsStorage {
  save(tabs: DataExplorerTab[], activeTabId: string): void {
    const state: PersistedDataExplorerTabsState = {
      tabs: tabs.map((tab) => ({
        id: tab.id,
        title: tab.title,
        graphJson:
          tab.state.rootNodes.length > 0
            ? serializeState(tab.state)
            : undefined,
      })),
      activeTabId,
      dashboards: serializeAllDashboards(tabs),
    };
    try {
      window.localStorage.setItem(
        DATA_EXPLORER_TABS_STORAGE_KEY,
        JSON.stringify(state),
      );
    } catch (e) {
      console.warn('Failed to save data explorer tabs to localStorage:', e);
    }
  }

  load(): PersistedDataExplorerTabsState | undefined {
    const value = window.localStorage.getItem(DATA_EXPLORER_TABS_STORAGE_KEY);
    if (value === null) {
      return undefined;
    }
    try {
      const res = PERSISTED_DATA_EXPLORER_TABS_STATE_SCHEMA.safeParse(
        JSON.parse(value),
      );
      return res.success ? res.data : undefined;
    } catch (e) {
      console.debug('Failed to parse data explorer tabs from localStorage:', e);
      return undefined;
    }
  }
}

// Singleton instance
export const dataExplorerTabsStorage = new DataExplorerTabsStorage();

export function createNewTabName(
  tabs: DataExplorerTab[],
  prefix = 'Graph',
): string {
  const existingNames = new Set(tabs.map((t) => t.title));
  let count = 1;
  while (existingNames.has(`${prefix} ${count}`)) {
    count++;
  }
  return `${prefix} ${count}`;
}

export function createEmptyState(): DataExplorerState {
  return {
    rootNodes: [],
    selectedNodes: new Set(),
    nodeLayouts: new Map(),
    labels: [],
  };
}

/** Serialized dashboard without a graphTabId reference. */
export interface SerializedDashboard {
  id: string;
  items?: unknown[];
  brushFilters?: Record<string, unknown[]>;
}

/** Serialize all dashboards for a single tab. */
export function serializeDashboardsForTab(
  tab: DataExplorerTab,
): SerializedDashboard[] | undefined {
  const result: SerializedDashboard[] = [];
  for (const db of tab.dashboards) {
    const items = serializeDashboardItems(db.items);
    let brushFilters: Record<string, unknown[]> | undefined;
    if (db.brushFilters.size > 0) {
      const raw: Record<string, unknown[]> = {};
      for (const [sourceNodeId, filters] of db.brushFilters) {
        raw[sourceNodeId] = filters;
      }
      brushFilters = JSON.parse(
        JSON.stringify(raw, (_k, v) => (typeof v === 'bigint' ? Number(v) : v)),
      );
    }
    result.push({
      id: db.id,
      items,
      brushFilters,
    });
  }
  return result.length > 0 ? result : undefined;
}

/** Reconstruct live DashboardTabState[] from persisted dashboard data. */
export function deserializeDashboardsForTab(
  tabId: string,
  allDashboards?: ReadonlyArray<PersistedDashboardData>,
): DashboardTabState[] {
  if (allDashboards !== undefined) {
    const matching = allDashboards.filter((db) => db.graphTabId === tabId);
    if (matching.length > 0) {
      return matching.map((db) => ({
        id: db.id,
        items: validateDashboardItems(db.items) ?? [],
        brushFilters:
          db.brushFilters !== undefined
            ? parseBrushFilters(db.brushFilters)
            : new Map(),
      }));
    }
  }
  return [
    {
      id: shortUuid(),
      items: [],
      brushFilters: new Map(),
    },
  ];
}

/** Flatten all dashboards across all tabs into a single serializable list. */
export function serializeAllDashboards(
  tabs: DataExplorerTab[],
): PersistedDashboardData[] | undefined {
  const result: PersistedDashboardData[] = [];
  for (const tab of tabs) {
    const serialized = serializeDashboardsForTab(tab);
    if (serialized !== undefined) {
      for (const db of serialized) {
        result.push({...db, graphTabId: tab.id});
      }
    }
  }
  return result.length > 0 ? result : undefined;
}

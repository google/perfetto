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
import {DataExplorerTab, DataExplorerState} from './data_explorer';
import {serializeState} from './json_handler';

const DATA_EXPLORER_TABS_STORAGE_KEY = 'perfettoDataExplorerTabs';

const PERSISTED_DATA_EXPLORER_TAB_SCHEMA = z.object({
  id: z.string(),
  title: z.string(),
  graphJson: z.string().optional(),
});

const PERSISTED_DATA_EXPLORER_TABS_STATE_SCHEMA = z.object({
  tabs: z.array(PERSISTED_DATA_EXPLORER_TAB_SCHEMA).min(1),
  activeTabId: z.string(),
});

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

export function createNewTabName(tabs: DataExplorerTab[], prefix = 'Graph'): string {
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

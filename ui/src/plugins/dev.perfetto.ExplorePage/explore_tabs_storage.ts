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
import {ExploreTab, ExplorePageState} from './explore_page';
import {serializeState} from './json_handler';

const EXPLORE_TABS_STORAGE_KEY = 'perfettoExploreTabs';

const PERSISTED_EXPLORE_TAB_SCHEMA = z.object({
  id: z.string(),
  title: z.string(),
  graphJson: z.string().optional(),
});

const PERSISTED_EXPLORE_TABS_STATE_SCHEMA = z.object({
  tabs: z.array(PERSISTED_EXPLORE_TAB_SCHEMA).min(1),
  activeTabId: z.string(),
});

export type PersistedExploreTabData = z.infer<
  typeof PERSISTED_EXPLORE_TAB_SCHEMA
>;

export type PersistedExploreTabsState = z.infer<
  typeof PERSISTED_EXPLORE_TABS_STATE_SCHEMA
>;

/**
 * Storage class for explore tabs state.
 * Persists tab layout (IDs, titles, serialized graphs) and active tab to
 * localStorage so that the Data Explorer survives page reloads.
 */
class ExploreTabsStorage {
  save(tabs: ExploreTab[], activeTabId: string): void {
    const state: PersistedExploreTabsState = {
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
        EXPLORE_TABS_STORAGE_KEY,
        JSON.stringify(state),
      );
    } catch (e) {
      console.warn('Failed to save explore tabs to localStorage:', e);
    }
  }

  load(): PersistedExploreTabsState | undefined {
    const value = window.localStorage.getItem(EXPLORE_TABS_STORAGE_KEY);
    if (value === null) {
      return undefined;
    }
    try {
      const res = PERSISTED_EXPLORE_TABS_STATE_SCHEMA.safeParse(
        JSON.parse(value),
      );
      return res.success ? res.data : undefined;
    } catch (e) {
      console.debug('Failed to parse explore tabs from localStorage:', e);
      return undefined;
    }
  }
}

// Singleton instance
export const exploreTabsStorage = new ExploreTabsStorage();

export function createNewTabName(tabs: ExploreTab[], prefix = 'Graph'): string {
  const existingNames = new Set(tabs.map((t) => t.title));
  let count = 1;
  while (existingNames.has(`${prefix} ${count}`)) {
    count++;
  }
  return `${prefix} ${count}`;
}

export function createEmptyState(): ExplorePageState {
  return {
    rootNodes: [],
    selectedNodes: new Set(),
    nodeLayouts: new Map(),
    labels: [],
  };
}

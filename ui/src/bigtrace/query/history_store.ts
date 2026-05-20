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
import {queryHistoryStorage} from './query_history_storage';
import {queryStore, type QueryExecution} from './query_store';

const HISTORY_REFRESH_DEBOUNCE_MS = 1000;

const MONTH_NAMES = [
  'January',
  'February',
  'March',
  'April',
  'May',
  'June',
  'July',
  'August',
  'September',
  'October',
  'November',
  'December',
] as const;

// Sidebar history-row date format: "May 9, 2026, 6:01 PM".
export function formatCompactDate(d: Date): string {
  const month = MONTH_NAMES[d.getMonth()];
  const day = d.getDate();
  const year = d.getFullYear();
  let h = d.getHours();
  const m12 = h >= 12 ? 'PM' : 'AM';
  h = h % 12 || 12;
  const mm = String(d.getMinutes()).padStart(2, '0');
  return `${month} ${day}, ${year}, ${h}:${mm} ${m12}`;
}

// Module-level: survives sidebar toggles so we don't re-fetch on every show.
export class HistoryStore {
  history: QueryExecution[] = [];
  isLoading = true;
  error: string | null = null;
  activeTabKey = 'standard';
  private lastRefreshSignal = -1;
  private debounceTimer?: number;
  private hasEverLoaded = false;

  requestRefresh(refreshSignal: number): void {
    if (refreshSignal === this.lastRefreshSignal) return;
    this.lastRefreshSignal = refreshSignal;
    if (!this.hasEverLoaded) {
      this.load();
      return;
    }
    if (this.debounceTimer !== undefined) {
      window.clearTimeout(this.debounceTimer);
    }
    this.debounceTimer = window.setTimeout(
      () => this.load(),
      HISTORY_REFRESH_DEBOUNCE_MS,
    );
  }

  refreshNow(): void {
    if (this.debounceTimer !== undefined) {
      window.clearTimeout(this.debounceTimer);
      this.debounceTimer = undefined;
    }
    this.load();
  }

  private async load(): Promise<void> {
    this.hasEverLoaded = true;
    this.isLoading = true;
    this.error = null;
    m.redraw();
    try {
      const list = await queryHistoryStorage.getAllHistory();
      this.history = list.map((entry) =>
        queryStore.getOrCreate(entry.uuid, entry),
      );
    } catch (e) {
      this.error = e instanceof Error ? e.message : String(e);
    } finally {
      this.isLoading = false;
      m.redraw();
    }
  }
}

export const historyStore = new HistoryStore();

// Point the History sidebar at the tab matching the impending run.
export function setHistoryActiveTab(materialize: boolean): void {
  const key = materialize ? 'materialized' : 'standard';
  if (historyStore.activeTabKey === key) return;
  historyStore.activeTabKey = key;
  m.redraw();
}

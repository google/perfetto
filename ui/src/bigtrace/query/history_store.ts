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

// Sidebar history-row time format: "6:01 PM".
export function formatCompactTime(d: Date): string {
  let h = d.getHours();
  const m12 = h >= 12 ? 'PM' : 'AM';
  h = h % 12 || 12;
  const mm = String(d.getMinutes()).padStart(2, '0');
  return `${h}:${mm} ${m12}`;
}

// Full local date + time for tooltips and modals: "May 9, 2026, 6:01 PM".
export function formatCompactDate(d: Date): string {
  return `${MONTH_NAMES[d.getMonth()]} ${d.getDate()}, ${d.getFullYear()}, ${formatCompactTime(d)}`;
}

interface HistoryDayGroup {
  readonly dayKey: string;
  readonly label: string;
  readonly entries: QueryExecution[];
}

function formatDayLabel(d: Date, dayKey: string, now: Date): string {
  if (dayKey === now.toDateString()) return 'Today';
  const yesterdayKey = new Date(
    now.getFullYear(),
    now.getMonth(),
    now.getDate() - 1,
  ).toDateString();
  if (dayKey === yesterdayKey) return 'Yesterday';
  return `${MONTH_NAMES[d.getMonth()].slice(0, 3)} ${d.getDate()}, ${d.getFullYear()}`;
}

// Buckets sorted entries by local calendar day.
export function groupHistoryByDay(
  entries: ReadonlyArray<QueryExecution>,
  now: Date = new Date(),
): HistoryDayGroup[] {
  const groups: HistoryDayGroup[] = [];
  for (const entry of entries) {
    const d = new Date(entry.startTime ?? 0);
    const dayKey = d.toDateString();
    let last: HistoryDayGroup | undefined = groups[groups.length - 1];
    if (last === undefined || last.dayKey !== dayKey) {
      last = {dayKey, label: formatDayLabel(d, dayKey, now), entries: []};
      groups.push(last);
    }
    last.entries.push(entry);
  }
  return groups;
}

// Which kinds of run the history list shows. Independent, so both on shows
// everything and both off shows nothing (the list says so).
export interface HistoryFilter {
  readonly ephemeral: boolean;
  readonly persistent: boolean;
}

export const ALL_KINDS: HistoryFilter = {ephemeral: true, persistent: true};

export function filterHistory(
  entries: ReadonlyArray<QueryExecution>,
  filter: HistoryFilter,
): QueryExecution[] {
  return entries.filter((e) =>
    e.materialized === true ? filter.persistent : filter.ephemeral,
  );
}

// Module-level: survives sidebar toggles so we don't re-fetch on every show.
export class HistoryStore {
  history: QueryExecution[] = [];
  isLoading = true;
  error: string | null = null;
  filter: HistoryFilter = ALL_KINDS;
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

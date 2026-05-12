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

import {DataSource} from '../../components/widgets/datagrid/data_source';
import {Row as DataGridRow} from '../../trace_processor/query_result';
import {debounce} from '../../base/rate_limiters';
import {shortUuid} from '../../base/uuid';
import {BigtraceQueryClient} from '../query/bigtrace_query_client';
import {queryStore, QueryExecution} from '../query/query_store';
import {SettingFilter} from '../settings/settings_types';

const QUERY_TABS_STORAGE_KEY = 'bigtraceQueryTabs';
const DEFAULT_SQL = '';
const DEFAULT_LIMIT = 100;
const TAB_TITLE_MAX_CHARS = 32;

// First non-empty, comment-stripped line of `sql`, clipped to
// TAB_TITLE_MAX_CHARS. Returns undefined if nothing's left. Heuristic —
// `/* … */` blocks aren't stripped (rare; worst case is an ugly title).
export function deriveTitleFromQuery(sql: string): string | undefined {
  const stripped = sql
    .split('\n')
    .map((line) => {
      const idx = line.indexOf('--');
      return idx === -1 ? line : line.slice(0, idx);
    })
    .map((line) => line.trim())
    .filter((line) => line.length > 0);
  if (stripped.length === 0) return undefined;
  const firstLine = stripped[0];
  if (firstLine.length <= TAB_TITLE_MAX_CHARS) return firstLine;
  return firstLine.slice(0, TAB_TITLE_MAX_CHARS - 1) + '…';
}

// Sync queries populate rows/columns; async queries leave them empty
// and the editor tab reads from `tab.dataSource` instead.
export interface QueryResponse {
  query: string;
  error?: string;
  totalRowCount: number;
  durationMs: number;
  columns: string[];
  rows: DataGridRow[];
  statementCount: number;
  statementWithOutputCount: number;
  lastStatementSql: string;
}

// QueryResponse with sensible defaults; callers spread real values via `partial`.
export function makeQueryResponse(
  query: string,
  partial: Partial<Omit<QueryResponse, 'query' | 'lastStatementSql'>> = {},
): QueryResponse {
  return {
    query,
    lastStatementSql: query,
    statementCount: 1,
    statementWithOutputCount: 0,
    totalRowCount: 0,
    durationMs: 0,
    columns: [],
    rows: [],
    error: undefined,
    ...partial,
  };
}

// State for one editor tab. Mutated in-place by the runner; only
// QueryTabsState creates and destroys these.
export interface BigTraceEditorTab {
  readonly id: string;
  title: string;
  editorText: string;
  limit: number;
  queryResult?: QueryResponse;
  isLoading: boolean;
  dataSource?: DataSource;
  querySettings: SettingFilter[];
  // Tab-lifetime AbortController. Aborted on tab close. Every backend
  // request that touches this tab plumbs `lifecycle.signal` so closing the
  // tab cancels in-flight requests instead of letting them write into a
  // dead `tab.execution`.
  readonly lifecycle: AbortController;
  // AbortController for the in-flight execute_* request specifically. Tied
  // to the tab lifecycle but separately abortable from a Cancel click
  // (which shouldn't tear down the rest of the tab's state).
  activeRequest?: AbortController;
  queryClient?: BigtraceQueryClient;
  materialize: boolean;
  queryUuid?: string;
  pollInterval?: number;
  lastProcessedRows: number;
  clientStartTime?: number;
  execution?: QueryExecution;
  // Incremented each time startPolling() is called. Stale poll loops
  // compare against this to self-terminate when superseded.
  pollGeneration: number;
}

// Persisted subset of BigTraceEditorTab. Transient state is rebuilt on load.
interface StoredTab {
  readonly id: string;
  readonly title: string;
  readonly editorText: string;
  readonly limit: number;
  readonly materialize: boolean;
  readonly queryUuid?: string;
  readonly error?: string;
}

interface StoredState {
  readonly tabs: ReadonlyArray<StoredTab>;
  readonly activeTabId?: string;
}

// Manages the collection of editor tabs. Survives QueryPage re-mounts so
// the user's tab layout is preserved across navigation.
export class QueryTabsState {
  tabs: BigTraceEditorTab[] = [];
  activeTabId = '';

  private tabCounter = 0;
  private readonly debouncedSave = debounce(() => this.saveToStorage(), 1000);

  constructor() {
    if (!this.loadFromStorage()) {
      this.addNewTab(undefined, DEFAULT_SQL);
    }
  }

  markDirty(): void {
    this.debouncedSave();
  }

  getActiveTab(): BigTraceEditorTab | undefined {
    return this.tabs.find((t) => t.id === this.activeTabId);
  }

  // Create a tab and make it active. If `forceNew` is false (the default)
  // and a tab already matches by `queryUuid` (preferred) or `initialQuery`,
  // the existing tab is reactivated instead of creating a duplicate.
  addNewTab(
    title?: string,
    initialQuery?: string,
    limit?: number,
    queryUuid?: string,
    materialize?: boolean,
    forceNew?: boolean,
  ): BigTraceEditorTab {
    if (!forceNew) {
      const existingTab = this.tabs.find((t) => {
        if (queryUuid && t.queryUuid === queryUuid) return true;
        if (!queryUuid && initialQuery && t.editorText === initialQuery) {
          return true;
        }
        return false;
      });

      if (existingTab) {
        this.activeTabId = existingTab.id;
        this.markDirty();
        return existingTab;
      }
    }

    // Caller-supplied title wins. Otherwise, derive from the
    // initialQuery (SQL → first non-comment line, clipped) so tabs
    // opened from History or example-query buttons get meaningful
    // labels for free, instead of falling all the way through to
    // "Query N". A run-time auto-name (`maybeAutoNameTab`) still
    // refines the title once the user actually runs something.
    const derivedTitle =
      title ?? (initialQuery && deriveTitleFromQuery(initialQuery));
    const tab: BigTraceEditorTab = {
      id: shortUuid(),
      title: derivedTitle || this.nextTabName(),
      editorText: initialQuery ?? '',
      limit: limit ?? DEFAULT_LIMIT,
      queryResult: undefined,
      isLoading: false,
      dataSource: undefined,
      querySettings: [],
      lifecycle: new AbortController(),
      activeRequest: undefined,
      // History-reopen (uuid present) → Persistent; brand-new (no
      // uuid) → sync. Caller can override.
      materialize: materialize ?? Boolean(queryUuid),
      lastProcessedRows: 0,
      queryUuid,
      pollGeneration: 0,
    };
    tab.execution = queryStore.getOrCreate(queryUuid || tab.id, {
      materialized: tab.materialize,
    });
    this.tabs.push(tab);
    this.activeTabId = tab.id;
    this.markDirty();
    return tab;
  }

  closeTab(tabId: string): void {
    if (this.tabs.length <= 1) return;
    const index = this.tabs.findIndex((t) => t.id === tabId);
    if (index === -1) return;
    const tabToClose = this.tabs[index];
    if (tabToClose.pollInterval !== undefined) {
      window.clearTimeout(tabToClose.pollInterval);
      tabToClose.pollInterval = undefined;
    }
    // Abort everything tied to this tab's lifecycle: the active execute_*
    // request, plus any one-off getStatus / getQueryExecution / fetchResults
    // that received the lifecycle signal.
    tabToClose.activeRequest?.abort();
    tabToClose.lifecycle.abort();
    this.tabs.splice(index, 1);
    if (this.activeTabId === tabId) {
      const newIndex = Math.min(index, this.tabs.length - 1);
      this.activeTabId = this.tabs[newIndex].id;
    }
    this.markDirty();
  }

  renameTab(tabId: string, newTitle: string): void {
    const tab = this.tabs.find((t) => t.id === tabId);
    if (tab) {
      tab.title = newTitle;
      this.markDirty();
    }
  }

  // Auto-derive a tab title from the query text when the tab still has
  // its default placeholder name ("Query N"). Skipped when the user has
  // manually renamed the tab (renameTab) — that title wins. Called by
  // QueryRunner.run before submitting; the title shows up in the tab
  // strip on the next render so users can tell tabs apart by content
  // instead of "Query 1" / "Query 2" / "Query 3".
  maybeAutoNameTab(tabId: string, queryText: string): void {
    const tab = this.tabs.find((t) => t.id === tabId);
    if (!tab) return;
    if (!/^Query \d+$/.test(tab.title)) return;
    const derived = deriveTitleFromQuery(queryText);
    if (derived === undefined) return;
    tab.title = derived;
    this.markDirty();
  }

  reorderTab(draggedId: string, beforeId: string | undefined): void {
    const draggedIndex = this.tabs.findIndex((t) => t.id === draggedId);
    if (draggedIndex === -1) return;
    const [dragged] = this.tabs.splice(draggedIndex, 1);
    if (beforeId === undefined) {
      this.tabs.push(dragged);
      return;
    }
    const beforeIndex = this.tabs.findIndex((t) => t.id === beforeId);
    if (beforeIndex === -1) {
      this.tabs.push(dragged);
    } else {
      this.tabs.splice(beforeIndex, 0, dragged);
    }
  }

  // ----- Persistence -----

  private saveToStorage(): void {
    const state: StoredState = {
      tabs: this.tabs.map((t) => ({
        id: t.id,
        title: t.title,
        editorText: t.editorText,
        limit: t.limit,
        materialize: t.materialize,
        queryUuid: t.queryUuid,
        error: t.queryResult?.error,
      })),
      activeTabId: this.activeTabId,
    };
    localStorage.setItem(QUERY_TABS_STORAGE_KEY, JSON.stringify(state));
  }

  private loadFromStorage(): boolean {
    const stored = localStorage.getItem(QUERY_TABS_STORAGE_KEY);
    if (!stored) return false;
    let parsed: StoredState;
    try {
      parsed = JSON.parse(stored) as StoredState;
    } catch {
      return false;
    }
    if (!Array.isArray(parsed.tabs) || parsed.tabs.length === 0) return false;

    for (const t of parsed.tabs) {
      const tab = this.addNewTab(
        t.title,
        t.editorText,
        t.limit,
        t.queryUuid,
        t.materialize,
      );
      if (t.error !== undefined && t.error !== '') {
        tab.queryResult = makeQueryResponse(tab.editorText, {error: t.error});
      }
    }
    if (typeof parsed.activeTabId === 'string') {
      const found = this.tabs.find((t) => t.id === parsed.activeTabId);
      if (!found) {
        // Restored tabs get new IDs, so activate by index instead.
        const idx = parsed.tabs.findIndex((t) => t.id === parsed.activeTabId);
        if (idx >= 0 && idx < this.tabs.length) {
          this.activeTabId = this.tabs[idx].id;
        }
      }
    }
    return true;
  }

  private nextTabName(): string {
    const existingNames = new Set(this.tabs.map((t) => t.title));
    let count = ++this.tabCounter;
    while (existingNames.has(`Query ${count}`)) {
      count = ++this.tabCounter;
    }
    return `Query ${count}`;
  }
}

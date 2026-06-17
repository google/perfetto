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

import type {DataSource} from '../../components/widgets/datagrid/data_source';
import type {Filter} from '../../components/widgets/datagrid/model';
import type {Row as DataGridRow} from '../../trace_processor/query_result';
import {debounce} from '../../base/rate_limiters';
import {shortUuid} from '../../base/uuid';
import type {
  BigtraceQueryClient,
  TracePreset,
} from '../query/bigtrace_query_client';
import {queryStore, type QueryExecution} from '../query/query_store';
import type {SettingCategory, SettingFilter} from '../settings/settings_types';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {
  traceFilterState,
  traceOrderByState,
  traceQueryColumnsState,
} from '../settings/trace_selection_state';

const QUERY_TABS_STORAGE_KEY = 'bigtraceQueryTabs';
const DEFAULT_SQL = '';
const DEFAULT_LIMIT = 100;
// Presets default to a higher row cap than a bare tab — they're meant to
// surface a meaningful result set, not a 100-row teaser.
const PRESET_DEFAULT_LIMIT = 1000;
const TAB_TITLE_MAX_CHARS = 32;

// First non-empty `--`-stripped line, clipped. `/* */` blocks not handled.
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

// Sync populates rows/columns; async leaves them empty (reads via `tab.dataSource`).
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

// Settings a run on this tab uses: global defaults (even globally-off ones)
// overridden by per-tab values, minus the tab's per-tab-disabled settings.
// Shared by the trace-grid data source (/trace_metadata) and the query runner
// (/execute_*) so the two agree on what the tab runs with.
export function effectiveTabSettings(tab: BigTraceEditorTab): SettingFilter[] {
  const byId = new Map<string, SettingFilter>();
  for (const s of bigTraceSettingsStorage.buildSettingFilters({
    includeDisabled: true,
  })) {
    byId.set(s.settingId, s);
  }
  for (const s of tab.querySettings) byId.set(s.settingId, s);
  for (const id of tab.disabledSettings) byId.delete(id);
  return [...byId.values()];
}

// Inverse of effectiveTabSettings' disable step: the snapshot lists ACTIVE
// settings, so disabled = the complement (every categoried setting it omits).
// Used to restore which toggles were off when reopening a query from history.
// Callers treat an empty snapshot as "no snapshot" and skip reconstruction,
// since an all-active complement is indistinguishable from a missing snapshot.
export function disabledSettingsFromSnapshot(
  activeSettingIds: ReadonlyArray<string>,
  allCategoriedSettingIds: ReadonlyArray<string>,
): string[] {
  const active = new Set(activeSettingIds);
  return allCategoriedSettingIds.filter((id) => !active.has(id));
}

// Mutated in-place by the runner; only QueryTabsState creates/destroys.
export interface BigTraceEditorTab {
  readonly id: string;
  title: string;
  editorText: string;
  limit: number;
  queryResult?: QueryResponse;
  isLoading: boolean;
  dataSource?: DataSource;
  querySettings: SettingFilter[];
  // Submit-time trace-selection snapshot — what the tab's last run used. Set by
  // QueryRunner at run time, restored from history; powers the query-page "what
  // did this run with?" view.
  traceFilters: readonly Filter[];
  // Tri-state (effectiveQueryColumns): null = defaultVisible; [] = nothing; [...] = these.
  traceMetadataColumns: readonly string[] | null;
  traceOrderBy: string;
  // Per-tab shown columns (display pref, persisted); null = show all.
  resultColumns: readonly string[] | null;
  // Per-tab disabled setting IDs, independent of global /settings. Seeded from
  // globals at creation, then toggled per-tab; excluded from effective settings.
  disabledSettings: readonly string[];
  // Tab-lifetime: every request plumbs `signal`; aborts on close.
  readonly lifecycle: AbortController;
  // Per-execute request: Cancel aborts this without tearing down the tab.
  activeRequest?: AbortController;
  queryClient?: BigtraceQueryClient;
  materialize: boolean;
  queryUuid?: string;
  pollInterval?: number;
  lastProcessedRows: number;
  clientStartTime?: number;
  execution?: QueryExecution;
  // Stale-poll guard: bumped on each startPolling() call.
  pollGeneration: number;
  // Active results tab (Table / Error / Chart). Undefined = auto-select (Error
  // on a no-row failure, else Table). Set on user click so it sticks across
  // redraws.
  resultsTabKey?: string;
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
  // Per-tab trace-selection snapshot, persisted so edits to the tab's
  // Settings sub-tab survive reload.
  readonly querySettings?: ReadonlyArray<SettingFilter>;
  readonly traceFilters?: ReadonlyArray<Filter>;
  // null = unchosen (attach defaultVisible); preserved distinct from [].
  readonly traceMetadataColumns?: ReadonlyArray<string> | null;
  readonly traceOrderBy?: string;
  readonly resultColumns?: ReadonlyArray<string> | null;
  readonly disabledSettings?: ReadonlyArray<string>;
}

interface StoredState {
  readonly tabs: ReadonlyArray<StoredTab>;
  readonly activeTabId?: string;
}

// Manages editor tabs + localStorage persistence across page reloads.
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

  // Create and activate. Without `forceNew`, reactivates an existing tab
  // matching by `queryUuid` (preferred) or `initialQuery`.
  addNewTab(
    title?: string,
    initialQuery?: string,
    limit?: number,
    queryUuid?: string,
    materialize?: boolean,
    forceNew?: boolean,
    stored?: Partial<StoredTab>,
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

    // Caller title wins; else derive from SQL so History opens get meaningful
    // labels instead of "Query N". maybeAutoNameTab refines on first run.
    const derivedTitle =
      title ?? (initialQuery && deriveTitleFromQuery(initialQuery));
    // Seed the per-tab trace-selection snapshot. Restored tabs use the
    // persisted snapshot; history-reopen tabs start empty (runner rehydrates
    // from /query_executions/{uuid}); fresh tabs copy the current /settings
    // globals.
    const isFromStorage = stored !== undefined;
    const isFromHistory = queryUuid !== undefined && !isFromStorage;
    const querySettings: SettingFilter[] = isFromStorage
      ? [...(stored?.querySettings ?? [])]
      : isFromHistory
        ? []
        : [...bigTraceSettingsStorage.buildSettingFilters()];
    const traceFilters: Filter[] = isFromStorage
      ? [...(stored?.traceFilters ?? [])]
      : isFromHistory
        ? []
        : [...traceFilterState.get()];
    // Restored: persisted; history-reopen: null (runner rehydrates); fresh: global.
    const traceMetadataColumns: readonly string[] | null = isFromStorage
      ? stored?.traceMetadataColumns ?? null
      : isFromHistory
        ? null
        : traceQueryColumnsState.get();
    const traceOrderBy: string = isFromStorage
      ? stored?.traceOrderBy ?? ''
      : isFromHistory
        ? ''
        : traceOrderByState.get();
    // Restored tabs keep their layout; fresh/history start at show-all (null).
    const resultColumns: readonly string[] | null = isFromStorage
      ? stored?.resultColumns ?? null
      : null;
    // Per-tab enable/disable. Fresh tabs mirror the current global state, then
    // diverge independently; restored tabs use their persisted set.
    const disabledSettings: string[] = isFromStorage
      ? [...(stored?.disabledSettings ?? [])]
      : isFromHistory
        ? []
        : bigTraceSettingsStorage
            .getAllSettings()
            .filter((s) => s.isDisabled())
            .map((s) => s.id);
    const tab: BigTraceEditorTab = {
      id: shortUuid(),
      title: derivedTitle || this.nextTabName(),
      editorText: initialQuery ?? '',
      limit: limit ?? DEFAULT_LIMIT,
      queryResult: undefined,
      isLoading: false,
      dataSource: undefined,
      querySettings,
      traceFilters,
      traceMetadataColumns,
      traceOrderBy,
      resultColumns,
      disabledSettings,
      lifecycle: new AbortController(),
      activeRequest: undefined,
      // Default to persistent; ?? (not ||) keeps an explicit/restored ephemeral.
      materialize: materialize ?? true,
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

  // Seed and activate a new tab from a home-page preset. The preset defines the
  // complete settings config: its own settings are applied and every other
  // setting is turned off — togglable settings get disabled, booleans get
  // value=false (they have no disable concept).
  addTabFromPreset(t: TracePreset): BigTraceEditorTab {
    const presetIds = new Set((t.settings ?? []).map((s) => s.setting_id));
    const querySettings: SettingFilter[] = (t.settings ?? []).map((s) => ({
      settingId: s.setting_id,
      values: [...s.values],
      category: s.category as SettingCategory,
    }));
    const disabledSettings: string[] = [];
    for (const raw of bigTraceSettingsStorage.getAllSettings()) {
      if (raw.category === undefined) continue;
      if (presetIds.has(raw.id)) continue;
      if (raw.type === 'boolean') {
        querySettings.push({
          settingId: raw.id,
          values: ['false'],
          category: raw.category as SettingCategory,
        });
      } else {
        disabledSettings.push(raw.id);
      }
    }
    const metadataColumns = t.traceMetadataColumns ?? [];
    return this.addNewTab(
      t.name || undefined,
      t.perfettoSql,
      // Optional in the contract; a preset with no explicit limit defaults to
      // 1000 rows (not the bare-tab default).
      t.limit != null && t.limit > 0 ? t.limit : PRESET_DEFAULT_LIMIT,
      undefined, // queryUuid — a preset is a fresh run, not a reopened one
      t.materialized ?? true,
      true, // forceNew
      {
        querySettings,
        disabledSettings,
        traceFilters: [...(t.traceFilters ?? [])],
        // [] from the wire means "unspecified" → use the default-visible set.
        traceMetadataColumns: metadataColumns.length
          ? [...metadataColumns]
          : null,
        traceOrderBy: t.traceOrderBy ?? '',
      },
    );
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
    // Aborts execute_* and any one-off request holding `lifecycle.signal`.
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

  // Replace "Query N" with a SQL-derived title before submit;
  // user-renamed tabs are skipped.
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
        // Persist the per-tab snapshot so Settings-sub-tab edits survive
        // reload; restored via the `stored` arg on addNewTab.
        querySettings: t.querySettings,
        traceFilters: t.traceFilters,
        traceMetadataColumns: t.traceMetadataColumns,
        traceOrderBy: t.traceOrderBy,
        resultColumns: t.resultColumns,
        disabledSettings: t.disabledSettings,
      })),
      activeTabId: this.activeTabId,
    };
    try {
      localStorage.setItem(QUERY_TABS_STORAGE_KEY, JSON.stringify(state));
    } catch {
      // QuotaExceededError — non-fatal; tabs persist on next successful save.
    }
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
      // Skip corrupted entries — missing fields would create broken tabs.
      if (typeof t.editorText !== 'string') continue;
      const tab = this.addNewTab(
        t.title,
        t.editorText,
        typeof t.limit === 'number' && t.limit > 0 ? t.limit : undefined,
        t.queryUuid,
        t.materialize,
        true,
        t,
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

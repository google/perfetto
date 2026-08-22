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

const QUERY_TABS_STORAGE_KEY = 'bigtraceQueryTabs';
const DEFAULT_SQL = '';
const TAB_TITLE_MAX_CHARS = 32;

// Row cap on the query and cap on how many traces it fans out to, defaulted per
// execution mode: an ephemeral run is a quick look, a persistent one is a full
// sweep whose results are saved.
export const MODE_DEFAULTS = {
  ephemeral: {rowLimit: 1_000, traceLimit: 10_000},
  persistent: {rowLimit: 10_000, traceLimit: 100_000},
} as const;

// The backend setting carrying the trace fan-out cap. The UI has to name it to
// seed the per-mode default; a backend that doesn't declare it simply gets no
// cap, and the toolbar hides the control.
export const TRACE_LIMIT_SETTING_ID = 'trace_limit';

function modeDefaults(materialize: boolean): {
  readonly rowLimit: number;
  readonly traceLimit: number;
} {
  return materialize ? MODE_DEFAULTS.persistent : MODE_DEFAULTS.ephemeral;
}

// Clamp to the bounds the backend declared, so a default we seed ourselves
// can't exceed what this deployment accepts.
function clampTraceLimit(value: number): number {
  const setting = bigTraceSettingsStorage.get(TRACE_LIMIT_SETTING_ID);
  if (setting === undefined) return value;
  const {min, max} = setting;
  let out = value;
  if (typeof min === 'number' && out < min) out = min;
  if (typeof max === 'number' && max > 0 && out > max) out = max;
  return out;
}

// The cap the user (or a preset) set explicitly on this tab, if any.
// Whether this tab ships no cap at all, because the setting is switched off.
export function traceLimitDisabled(tab: BigTraceEditorTab): boolean {
  return tab.disabledSettings.includes(TRACE_LIMIT_SETTING_ID);
}

function explicitTraceLimit(tab: BigTraceEditorTab): number | undefined {
  const entry = tab.querySettings.find(
    (s) => s.settingId === TRACE_LIMIT_SETTING_ID,
  );
  if (entry === undefined) return undefined;
  const n = Number(entry.values[0]);
  return Number.isFinite(n) ? n : undefined;
}

// The cap the next run will use: the explicit value, else the mode default.
// Meaningless when the tab has the setting disabled — then the run is uncapped
// and callers hide the control (see traceLimitDisabled).
export function effectiveTraceLimit(tab: BigTraceEditorTab): number {
  return (
    explicitTraceLimit(tab) ??
    clampTraceLimit(modeDefaults(tab.materialize).traceLimit)
  );
}

export function setTraceLimit(tab: BigTraceEditorTab, value: number): void {
  const setting = bigTraceSettingsStorage.get(TRACE_LIMIT_SETTING_ID);
  if (setting === undefined) return;
  // Setting a cap turns the setting back on — the toolbar control is the cap,
  // so a value typed there can't be silently dropped by a stale disable.
  tab.disabledSettings = tab.disabledSettings.filter(
    (id) => id !== TRACE_LIMIT_SETTING_ID,
  );
  tab.querySettings = [
    ...tab.querySettings.filter((s) => s.settingId !== TRACE_LIMIT_SETTING_ID),
    {
      settingId: TRACE_LIMIT_SETTING_ID,
      values: [String(clampTraceLimit(value))],
      category: (setting.category ?? 'TRACE_ADDRESS') as SettingCategory,
    },
  ];
}

// Switch a tab's execution mode, moving both limits to the new mode's defaults
// — but only where they still hold the old mode's default, so a value the user
// typed (or a preset set) survives the flip.
export function applyModeDefaults(
  tab: BigTraceEditorTab,
  materialize: boolean,
): void {
  const from = modeDefaults(tab.materialize);
  const to = modeDefaults(materialize);
  if (tab.limit === from.rowLimit) tab.limit = to.rowLimit;
  const explicit = explicitTraceLimit(tab);
  if (explicit !== undefined && explicit === clampTraceLimit(from.traceLimit)) {
    setTraceLimit(tab, to.traceLimit);
  }
  tab.materialize = materialize;
}

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
  // With no explicit per-tab cap, fall back to this tab's mode default rather
  // than the backend's global one, so an ephemeral run stays a quick look.
  const traceLimit = byId.get(TRACE_LIMIT_SETTING_ID);
  if (traceLimit !== undefined && explicitTraceLimit(tab) === undefined) {
    byId.set(TRACE_LIMIT_SETTING_ID, {
      ...traceLimit,
      values: [String(effectiveTraceLimit(tab))],
    });
  }
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

// Configure a tab from a preset: its query, trace selection, caps and mode.
// The preset's own settings are applied and every other registered setting is
// turned off — togglable ones disabled, booleans set to false (they have no
// disable concept) — so a preset describes the whole run, not a diff.
export function applyPresetToTab(tab: BigTraceEditorTab, t: TracePreset): void {
  const presetIds = new Set((t.settings ?? []).map((s) => s.settingId));
  const querySettings: SettingFilter[] = (t.settings ?? []).map((s) => ({
    settingId: s.settingId,
    values: [...s.values],
    category: s.category as SettingCategory,
  }));
  const metadataColumns = t.traceMetadataColumns ?? [];
  const materialized = t.materialized ?? true;
  const disabledSettings: string[] = [];
  for (const raw of bigTraceSettingsStorage.getAllSettings()) {
    if (raw.category === undefined) continue;
    if (presetIds.has(raw.id)) continue;
    if (raw.id === TRACE_LIMIT_SETTING_ID) {
      // The trace cap is a run control with a per-mode default, not something
      // a preset can leave unset: a preset that doesn't state one runs at the
      // default for its mode, never uncapped.
      querySettings.push({
        settingId: raw.id,
        values: [
          String(clampTraceLimit(modeDefaults(materialized).traceLimit)),
        ],
        category: raw.category as SettingCategory,
      });
    } else if (raw.type === 'boolean') {
      querySettings.push({
        settingId: raw.id,
        values: ['false'],
        category: raw.category as SettingCategory,
      });
    } else {
      disabledSettings.push(raw.id);
    }
  }

  tab.editorText = t.perfettoSql;
  if (t.name) tab.title = t.name;
  tab.querySettings = querySettings;
  tab.disabledSettings = disabledSettings;
  tab.traceFilters = [...(t.traceFilters ?? [])];
  // [] from the wire means "unspecified" → use the default-visible set.
  tab.traceMetadataColumns = metadataColumns.length
    ? [...metadataColumns]
    : null;
  tab.traceOrderBy = t.traceOrderBy ?? '';
  tab.materialize = materialized;
  // Optional in the contract; a preset that doesn't state a cap uses the
  // default for the mode it runs in.
  tab.limit =
    t.limit != null && t.limit > 0
      ? t.limit
      : modeDefaults(materialized).rowLimit;
  tab.configured = true;
}

// The run configuration Settings edits, as it stood when the form opened —
// enough to put the tab back if the user leaves with Cancel instead of Apply.
export interface TabConfigSnapshot {
  readonly querySettings: ReadonlyArray<SettingFilter>;
  readonly disabledSettings: ReadonlyArray<string>;
  readonly traceFilters: ReadonlyArray<Filter>;
  readonly traceMetadataColumns: ReadonlyArray<string> | null;
  readonly traceOrderBy: string;
  readonly limit: number;
  readonly materialize: boolean;
}

// Deep enough that neither side can reach the other's arrays.
function copySettingFilters(
  list: ReadonlyArray<SettingFilter>,
): SettingFilter[] {
  return list.map((s) => ({...s, values: [...s.values]}));
}

export function snapshotTabConfig(tab: BigTraceEditorTab): TabConfigSnapshot {
  return {
    querySettings: copySettingFilters(tab.querySettings),
    disabledSettings: [...tab.disabledSettings],
    traceFilters: [...tab.traceFilters],
    traceMetadataColumns:
      tab.traceMetadataColumns === null ? null : [...tab.traceMetadataColumns],
    traceOrderBy: tab.traceOrderBy,
    limit: tab.limit,
    materialize: tab.materialize,
  };
}

export function restoreTabConfig(
  tab: BigTraceEditorTab,
  snap: TabConfigSnapshot,
): void {
  tab.querySettings = copySettingFilters(snap.querySettings);
  tab.disabledSettings = [...snap.disabledSettings];
  tab.traceFilters = [...snap.traceFilters];
  tab.traceMetadataColumns =
    snap.traceMetadataColumns === null ? null : [...snap.traceMetadataColumns];
  tab.traceOrderBy = snap.traceOrderBy;
  tab.limit = snap.limit;
  tab.materialize = snap.materialize;
}

// Open Settings on a tab that already has a configuration: the launcher takes
// the tab over on its settings form, and the configuration on entry is kept so
// that leaving with Cancel restores it while Apply keeps the edits.
export function openSettings(tab: BigTraceEditorTab): void {
  tab.settingsSession = {before: snapshotTabConfig(tab)};
  tab.setupMode = 'custom';
}

// Leave the launcher for the editor. Without `keep`, the configuration goes
// back to what it was when Settings opened; a tab with no such session (a new
// tab starting its first query) has nothing to restore.
export function closeSettings(
  tab: BigTraceEditorTab,
  {keep}: {readonly keep: boolean},
): void {
  if (!keep && tab.settingsSession !== undefined) {
    restoreTabConfig(tab, tab.settingsSession.before);
  }
  tab.settingsSession = undefined;
  tab.setupMode = undefined;
  tab.configured = true;
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
  // False until the tab has been given a configuration — a preset, or settings
  // the user chose by hand. Unconfigured tabs show the launcher instead of the
  // editor. Tabs restored from storage or opened from History are configured.
  configured: boolean;
  // Which half of the launcher to show: the preset gallery, or the settings
  // form. View state, so not persisted.
  setupMode?: 'presets' | 'custom';
  // Set while Settings is open on a configured tab: the run configuration on
  // entry, so Cancel can put it back. View state, not persisted — a reload
  // closes Settings with the edits kept, as Apply would.
  settingsSession?: {readonly before: TabConfigSnapshot};
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
  readonly configured?: boolean;
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
    // Seed the per-tab settings snapshot. Restored tabs use the persisted
    // one; history-reopen tabs start empty (the runner rehydrates from
    // /query_executions/{uuid}); fresh tabs start from the backend defaults.
    const isFromStorage = stored !== undefined;
    const isFromHistory = queryUuid !== undefined && !isFromStorage;
    // Default to persistent; ?? (not ||) keeps an explicit/restored ephemeral.
    const isPersistent = materialize ?? true;
    const querySettings: SettingFilter[] = isFromStorage
      ? [...(stored?.querySettings ?? [])]
      : isFromHistory
        ? []
        : [...bigTraceSettingsStorage.buildSettingFilters()];
    // A fresh tab copies the current global defaults, which include the
    // backend's own trace cap; replace it with the one for this tab's mode.
    if (!isFromStorage && !isFromHistory) {
      const idx = querySettings.findIndex(
        (s) => s.settingId === TRACE_LIMIT_SETTING_ID,
      );
      if (idx >= 0) {
        querySettings[idx] = {
          ...querySettings[idx],
          values: [
            String(clampTraceLimit(modeDefaults(isPersistent).traceLimit)),
          ],
        };
      }
    }
    // Restored tabs use their snapshot; history-reopen tabs are rehydrated by
    // the runner; a fresh tab starts empty and gets its selection from the
    // preset (or custom setup) chosen in the launcher.
    const traceFilters: Filter[] = isFromStorage
      ? [...(stored?.traceFilters ?? [])]
      : [];
    const traceMetadataColumns: readonly string[] | null = isFromStorage
      ? (stored?.traceMetadataColumns ?? null)
      : null;
    const traceOrderBy: string = isFromStorage
      ? (stored?.traceOrderBy ?? '')
      : '';
    // Restored tabs keep their layout; fresh/history start at show-all (null).
    const resultColumns: readonly string[] | null = isFromStorage
      ? (stored?.resultColumns ?? null)
      : null;
    // Per-tab enable/disable. Fresh tabs mirror what the backend declares
    // disabled, then diverge; restored tabs use their persisted set.
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
      // No caller-supplied cap: take the one that fits the execution mode.
      limit: limit ?? modeDefaults(isPersistent).rowLimit,
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
      materialize: isPersistent,
      lastProcessedRows: 0,
      queryUuid,
      pollGeneration: 0,
      // Tabs predating the launcher have no flag; treat them as configured so
      // a reload never drops the user back into the picker.
      configured: isFromStorage
        ? (stored?.configured ?? true)
        : isFromHistory
          ? true
          : false,
    };
    tab.execution = queryStore.getOrCreate(queryUuid || tab.id, {
      materialized: tab.materialize,
    });
    this.tabs.push(tab);
    this.activeTabId = tab.id;
    this.markDirty();
    return tab;
  }

  // Seed and activate a new tab from a preset (backend catalog or one the user
  // saved locally).
  addTabFromPreset(t: TracePreset): BigTraceEditorTab {
    const tab = this.addNewTab(
      t.name || undefined,
      t.perfettoSql,
      undefined,
      undefined, // queryUuid — a preset is a fresh run, not a reopened one
      t.materialized ?? true,
      true, // forceNew
    );
    applyPresetToTab(tab, t);
    this.markDirty();
    return tab;
  }

  // Clone a tab: same query and configuration, no results and no queryUuid,
  // so running it creates its own execution instead of colliding with the
  // original's in History.
  cloneTab(tabId: string): BigTraceEditorTab | undefined {
    const src = this.tabs.find((t) => t.id === tabId);
    if (src === undefined) return undefined;
    const clone = this.addNewTab(
      this.cloneTitle(src.title),
      src.editorText,
      src.limit,
      undefined,
      src.materialize,
      true, // forceNew
      {
        querySettings: src.querySettings,
        disabledSettings: src.disabledSettings,
        traceFilters: src.traceFilters,
        traceMetadataColumns:
          src.traceMetadataColumns === null
            ? null
            : [...src.traceMetadataColumns],
        traceOrderBy: src.traceOrderBy,
        resultColumns: src.resultColumns,
        configured: true,
      },
    );
    this.markDirty();
    return clone;
  }

  // "<name> clone", then "clone 2", … so repeated clones stay apart.
  private cloneTitle(base: string): string {
    const taken = new Set(this.tabs.map((t) => t.title));
    let title = `${base} clone`;
    for (let n = 2; taken.has(title); n++) {
      title = `${base} clone ${n}`;
    }
    return title;
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
        configured: t.configured,
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

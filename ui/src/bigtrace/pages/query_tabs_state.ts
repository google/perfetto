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
import {BigtraceAsyncDataSource} from '../query/bigtrace_async_data_source';
import type {
  BigtraceQueryClient,
  ExperimentFilterSpec,
  TracePreset,
} from '../query/bigtrace_query_client';
import {queryStore, type QueryExecution} from '../query/query_store';
import type {BigtraceColumnSchema} from '../query/column_types';
import type {SettingCategory, SettingFilter} from '../settings/settings_types';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';

const RESERVED_SQL_WORDS = new Set([
  'select',
  'where',
  'group',
  'order',
  'limit',
  'join',
  'left',
  'right',
  'inner',
  'outer',
  'cross',
  'natural',
  'using',
  'on',
  'with',
  'union',
  'from',
  'table',
]);

export function deriveTableNameFromSql(sql: string): string | undefined {
  if (!sql) return undefined;
  const match = sql.match(
    /\bFROM\s+([a-zA-Z_][a-zA-Z0-9_]*)(?:\.([a-zA-Z_][a-zA-Z0-9_]*))?\b/i,
  );
  if (!match) return undefined;
  const name = (match[2] ?? match[1]).toLowerCase();
  return RESERVED_SQL_WORDS.has(name) ? undefined : name;
}

const QUERY_TABS_STORAGE_KEY = 'bigtraceQueryTabs';
const DEFAULT_SQL = '';

// Row cap on the query and cap on how many traces it fans out to, defaulted per
// execution mode: an ephemeral run is a quick look, a persistent one is a full
// sweep whose results are saved.
export const MODE_DEFAULTS = {
  ephemeral: {rowLimit: 1_000, traceLimit: 10_000},
  persistent: {rowLimit: 10_000, traceLimit: 100_000},
} as const;

export const DEFAULT_TABLE_TTL_DAYS = 30;

// The backend setting carrying an explicit list of trace UUIDs — the second
// way to select a corpus: instead of a source plus the grid filter, exactly
// these traces. A backend that supports it declares the setting (disabled by
// default); the UI names the id to treat it as a selection MODE rather than
// an ordinary card. When it is enabled and non-empty the backend selects
// exactly these traces and ignores the filter-mode fields.
// A tab's experiment selection: the wire triple plus whatever the catalog
// told us about it. The names and denial flags are for display only — they
// are refetched, never authoritative, and never travel.
export interface ExperimentFilterState extends ExperimentFilterSpec {
  readonly experimentName?: string;
  readonly controlName?: string;
  readonly experimentDenied?: boolean;
  readonly controlDenied?: boolean;
}

export const TRACE_UUIDS_SETTING_ID = 'trace_uuids';

// Whether this deployment offers selection by UUID at all.
export function traceUuidsDeclared(): boolean {
  return bigTraceSettingsStorage.get(TRACE_UUIDS_SETTING_ID) !== undefined;
}

// UUID mode is DERIVED, not stored — one predicate for every surface. The
// tab must hold its OWN trace_uuids entry (created on entering the mode, or
// by a preset, or by a clone/restore) and not have it disabled. Requiring the
// explicit entry keeps a fresh tab out of the mode even when the exec config
// (which declares the setting) arrives only after the tab was created — the
// tab-creation mirror of globally-disabled settings can't cover settings it
// hasn't seen yet.
export function traceUuidsState(
  declared: boolean,
  disabled: boolean,
  hasEntry: boolean,
): boolean {
  return declared && !disabled && hasEntry;
}

export function traceUuidsActive(tab: BigTraceEditorTab): boolean {
  const entry = tab.querySettings.find(
    (s) => s.settingId === TRACE_UUIDS_SETTING_ID,
  );
  const inLauncher = !tab.configured || tab.settingsSession !== undefined;
  const hasEntry =
    entry !== undefined && (inLauncher || entry.values.length > 0);
  return traceUuidsState(
    traceUuidsDeclared(),
    tab.disabledSettings.includes(TRACE_UUIDS_SETTING_ID),
    hasEntry,
  );
}

// Enter/leave UUID mode. Entering ensures the tab's own entry (keeping any
// values it held from an earlier stint in the mode) and enables it; leaving
// only disables — values and the whole filter-mode configuration (source,
// grid filter, order) stay put, hidden rather than cleared.
export function setTraceUuidsActive(
  tab: BigTraceEditorTab,
  active: boolean,
): void {
  const setting = bigTraceSettingsStorage.get(TRACE_UUIDS_SETTING_ID);
  if (setting === undefined) return;
  const without = tab.disabledSettings.filter(
    (id) => id !== TRACE_UUIDS_SETTING_ID,
  );
  if (active) {
    tab.disabledSettings = without;
    if (
      !tab.querySettings.some((s) => s.settingId === TRACE_UUIDS_SETTING_ID)
    ) {
      tab.querySettings = [
        ...tab.querySettings,
        {
          settingId: TRACE_UUIDS_SETTING_ID,
          values: [],
          category: (setting.category ?? 'TRACE_ADDRESS') as SettingCategory,
        },
      ];
    }
  } else {
    tab.disabledSettings = [...without, TRACE_UUIDS_SETTING_ID];
  }
}

// Pasted text → UUID list: split on commas and any whitespace, drop empties,
// dedupe keeping first occurrence. No format validation — what a "uuid" looks
// like is the backend's business.
export function parseTraceUuids(text: string): string[] {
  const seen = new Set<string>();
  const out: string[] = [];
  for (const token of text.split(/[\s,]+/)) {
    if (token === '' || seen.has(token)) continue;
    seen.add(token);
    out.push(token);
  }
  return out;
}

// Trace selection is WHICH traces a query runs over: the source settings
// (TRACE_ADDRESS) and any per-trace metadata filters an indexer backend
// declares (TRACE_METADATA).
export function isTraceSelectionSetting(setting: {
  readonly id: string;
  readonly category?: string;
}): boolean {
  return (
    setting.category === 'TRACE_ADDRESS' ||
    setting.category === 'TRACE_METADATA'
  );
}

function modeDefaults(materialize: boolean): {
  readonly rowLimit: number;
  readonly traceLimit: number;
} {
  return materialize ? MODE_DEFAULTS.persistent : MODE_DEFAULTS.ephemeral;
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
  if (tab.traceLimit === from.traceLimit) tab.traceLimit = to.traceLimit;
  tab.materialize = materialize;
}

// Whether there is anything to run: a line that isn't blank or a `--`
// comment. `/* */` blocks not handled.
export function hasQueryText(sql: string): boolean {
  return sql.split('\n').some((line) => {
    const idx = line.indexOf('--');
    return (idx === -1 ? line : line.slice(0, idx)).trim().length > 0;
  });
}

// Sync populates rows/columns; async leaves them empty (reads via `tab.dataSource`).
export interface QueryResponse {
  query: string;
  error?: string;
  totalRowCount: number;
  durationMs: number;
  columns: string[];
  rows: DataGridRow[];
  schema?: ReadonlyArray<BigtraceColumnSchema>;
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
    schema: undefined,
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

// Configure a tab's trace selection from a preset: the source settings, the
// grid filter and order, and the result-metadata columns. A preset carries
// trace selection and SQL, nothing else — the query options, the caps and
// the mode are the tab's own and stay untouched (anything else a stale
// catalog entry still names is ignored). Within its scope a preset is the
// whole selection, not a diff: trace settings it doesn't name are turned
// off — togglable ones disabled, booleans set to false.
export function applyPresetSetup(tab: BigTraceEditorTab, t: TracePreset): void {
  const presetTraceSettings: SettingFilter[] = (t.settings ?? [])
    .filter((s) =>
      isTraceSelectionSetting({id: s.settingId, category: s.category}),
    )
    .map((s) => ({
      settingId: s.settingId,
      values: [...s.values],
      category: s.category as SettingCategory,
    }));
  const presetIds = new Set(presetTraceSettings.map((s) => s.settingId));

  // Overrides outside trace selection (options, the cap) are kept verbatim.
  const querySettings: SettingFilter[] = tab.querySettings.filter(
    (s) => !isTraceSelectionSetting({id: s.settingId, category: s.category}),
  );
  querySettings.push(...presetTraceSettings);
  const disabledSettings = new Set(
    tab.disabledSettings.filter((id) => {
      const raw = bigTraceSettingsStorage.get(id);
      // Unknown ids are not ours to interpret; keep them disabled.
      return raw === undefined || !isTraceSelectionSetting(raw);
    }),
  );
  for (const raw of bigTraceSettingsStorage.getAllSettings()) {
    if (!isTraceSelectionSetting(raw)) continue;
    if (presetIds.has(raw.id)) continue;
    if (raw.type === 'boolean') {
      querySettings.push({
        settingId: raw.id,
        values: ['false'],
        category: raw.category as SettingCategory,
      });
    } else {
      disabledSettings.add(raw.id);
    }
  }

  tab.querySettings = querySettings;
  tab.disabledSettings = [...disabledSettings];
  tab.traceFilters = [...(t.traceFilters ?? [])];
  // [] from the wire means "unspecified" → use the default-visible set.
  const metadataColumns = t.traceMetadataColumns ?? [];
  tab.traceMetadataColumns = metadataColumns.length
    ? [...metadataColumns]
    : null;
  tab.traceOrderBy = t.traceOrderBy ?? '';
  // Ids and arm only: a preset states the selection, and the names for it are
  // resolved from the catalog. One it doesn't state is turned off, like every
  // other part of the selection.
  tab.experimentFilter =
    t.experimentFilter === undefined ? undefined : {...t.experimentFilter};
  tab.lastPresetId = t.id;
}

// Fill a tab from a preset: its trace selection plus its query. The tab keeps
// its own name, options, caps and mode. Whether the tab then leaves the
// launcher is the caller's — on a new tab a card only fills the page in, and
// Start query opens the editor.
export function applyPresetToTab(tab: BigTraceEditorTab, t: TracePreset): void {
  applyPresetSetup(tab, t);
  tab.editorText = t.perfettoSql;
}

// The run configuration Settings edits, as it stood when the form opened —
// enough to put the tab back if the user leaves with Cancel instead of Apply.
export interface TabConfigSnapshot {
  readonly querySettings: ReadonlyArray<SettingFilter>;
  readonly disabledSettings: ReadonlyArray<string>;
  readonly traceFilters: ReadonlyArray<Filter>;
  readonly traceMetadataColumns: ReadonlyArray<string> | null;
  readonly traceOrderBy: string;
  readonly experimentFilter: ExperimentFilterState | undefined;
  readonly limit: number;
  readonly traceLimit: number;
  readonly tableTtlDays: number;
  readonly materialize: boolean;
}

// Flat primitives throughout, so a spread detaches it fully.
export function copyExperimentFilter(
  filter: ExperimentFilterState | undefined,
): ExperimentFilterState | undefined {
  return filter === undefined ? undefined : {...filter};
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
    experimentFilter: copyExperimentFilter(tab.experimentFilter),
    limit: tab.limit,
    traceLimit: tab.traceLimit,
    tableTtlDays: tab.tableTtlDays,
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
  tab.experimentFilter = copyExperimentFilter(snap.experimentFilter);
  tab.limit = snap.limit;
  tab.traceLimit = snap.traceLimit;
  tab.tableTtlDays = snap.tableTtlDays;
  tab.materialize = snap.materialize;
}

// Open Settings on a tab that already has a configuration: the launcher takes
// the tab over on its settings form, and the configuration on entry is kept so
// that leaving with Cancel restores it while Apply keeps the edits.
export function openSettings(tab: BigTraceEditorTab): void {
  tab.settingsSession = {before: snapshotTabConfig(tab)};
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
  const uuidEntry = tab.querySettings.find(
    (s) => s.settingId === TRACE_UUIDS_SETTING_ID,
  );
  if (uuidEntry !== undefined && uuidEntry.values.length === 0) {
    setTraceUuidsActive(tab, false);
  }
  tab.settingsSession = undefined;
  tab.configured = true;
}

// Puts `query` in a DuckDB-mode tab without running it. Reuses the active tab
// when it is idle and empty, a tab already holding exactly `query`, or else
// opens a new one, so the user's SQL is never overwritten.
export function stageDuckdbQuery(
  tabsState: QueryTabsState,
  query: string,
  limit?: number,
): BigTraceEditorTab {
  const active = tabsState.getActiveTab();
  const reuseActive =
    active !== undefined &&
    !active.isLoading &&
    !hasQueryText(active.editorText);
  tabsState.activeEngine = 'duckdb';
  const tab = reuseActive
    ? active
    : tabsState.addNewTab(undefined, query, limit);
  tab.editorText = query;
  if (limit !== undefined) tab.limit = limit;
  tab.queryEngine = 'duckdb';
  tab.settingsSession = undefined;
  tab.configured = true;
  tabsState.activeTabId = tab.id;
  tabsState.markDirty();
  return tab;
}

export function defaultTableName(): string {
  const suffix = shortUuid()
    .slice(1)
    .replace(/[^a-zA-Z0-9]/g, '')
    .slice(0, 6)
    .toLowerCase();
  return `tbl_${suffix || '1'}`;
}

export interface SubQuerySlot {
  readonly id: string;
  sql: string;
  tableName: string;
  queryUuid?: string;
  queryResult?: QueryResponse;
  dataSource?: DataSource;
  execution?: QueryExecution;
}

export interface StoredSubQuerySlot {
  readonly id: string;
  readonly sql: string;
  readonly tableName: string;
  readonly queryUuid?: string;
}

export function loadSlotIntoTab(tab: BigTraceEditorTab, index: number): void {
  if (index < 0 || index >= tab.subQueries.length) return;
  tab.activeSubQueryIndex = index;
  const slot = tab.subQueries[index];
  tab.editorText = slot.sql;
  tab.tableName = slot.tableName;
  tab.queryUuid = slot.queryUuid;
  tab.queryResult = slot.queryResult;
  tab.dataSource = slot.dataSource;
  tab.execution =
    slot.execution ??
    (slot.queryUuid
      ? queryStore.getOrCreate(slot.queryUuid, {
          materialized: tab.materialize,
          queryEngine: tab.queryEngine,
          perfettoSql: slot.sql,
          tableName: slot.tableName,
        })
      : undefined);
  if (slot.queryUuid && !slot.dataSource && tab.queryClient) {
    slot.dataSource = new BigtraceAsyncDataSource(
      slot.queryUuid,
      tab.queryClient,
      () => slot.execution?.processedRows ?? 0,
      tab.lifecycle.signal,
      () => slot.execution?.schema,
    );
    tab.dataSource = slot.dataSource;
    if (!slot.queryResult) {
      slot.queryResult = makeQueryResponse(slot.sql, {
        totalRowCount: slot.execution?.processedRows ?? 0,
      });
      tab.queryResult = slot.queryResult;
    }
  }
}

export function syncActiveSubQueryToSlot(tab: BigTraceEditorTab): void {
  if (tab.subQueries.length === 0) return;
  const idx = Math.max(
    0,
    Math.min(tab.activeSubQueryIndex, tab.subQueries.length - 1),
  );
  const slot = tab.subQueries[idx];
  slot.sql = tab.editorText;
  slot.tableName = tab.tableName;
  slot.queryUuid = tab.queryUuid;
  slot.queryResult = tab.queryResult;
  slot.dataSource = tab.dataSource;
  slot.execution = tab.execution;
}

export function selectSubQuery(tab: BigTraceEditorTab, index: number): void {
  if (index < 0 || index >= tab.subQueries.length) return;
  syncActiveSubQueryToSlot(tab);
  loadSlotIntoTab(tab, index);
}

export function addSubQuery(tab: BigTraceEditorTab): void {
  if (tab.subQueries.length === 0) {
    const firstTable = tab.tableName.trim() || defaultTableName();
    tab.tableName = firstTable;
    tab.subQueries.push({
      id: shortUuid(),
      sql: tab.editorText,
      tableName: firstTable,
      queryUuid: tab.queryUuid,
      queryResult: tab.queryResult,
      dataSource: tab.dataSource,
      execution: tab.execution,
    });
    tab.activeSubQueryIndex = 0;
  } else {
    syncActiveSubQueryToSlot(tab);
  }
  tab.subQueries.push({
    id: shortUuid(),
    sql: '',
    tableName: defaultTableName(),
  });
  loadSlotIntoTab(tab, tab.subQueries.length - 1);
}

export function removeSubQuery(tab: BigTraceEditorTab, index: number): void {
  if (index < 0 || index >= tab.subQueries.length) return;
  syncActiveSubQueryToSlot(tab);
  tab.subQueries.splice(index, 1);
  if (tab.subQueries.length <= 1) {
    const remaining: SubQuerySlot | undefined = tab.subQueries[0];
    tab.subQueries = [];
    tab.activeSubQueryIndex = 0;
    if (remaining !== undefined) {
      tab.editorText = remaining.sql;
      tab.tableName = remaining.tableName;
      tab.queryUuid = remaining.queryUuid;
      tab.queryResult = remaining.queryResult;
      tab.dataSource = remaining.dataSource;
      tab.execution = remaining.execution;
    }
    return;
  }
  const nextIdx = Math.min(tab.activeSubQueryIndex, tab.subQueries.length - 1);
  loadSlotIntoTab(tab, nextIdx);
}

export function getSubQueryTableName(
  slot: {
    sql?: string;
    editorText?: string;
    tableName?: string;
    execution?: QueryExecution;
  },
  index: number,
): string {
  const custom = slot.tableName?.trim();
  if (custom) return custom;
  if (slot.execution?.tableName) return slot.execution.tableName;
  const sql = slot.sql ?? slot.editorText ?? '';
  const derived = deriveTableNameFromSql(sql);
  if (derived) return derived;
  return `result_${index + 1}`;
}

export function buildMultiQuerySpecs(
  subQueries: ReadonlyArray<SubQuerySlot>,
): Array<{slotId: string; perfettoSql: string; tableName: string}> {
  const active = subQueries.filter((q) => hasQueryText(q.sql));
  const used = new Set<string>();
  return active.map((q, idx) => {
    const base =
      q.tableName.trim() ||
      deriveTableNameFromSql(q.sql) ||
      `result_${idx + 1}`;
    let name = base;
    let count = 1;
    while (used.has(name.toLowerCase())) {
      name = `${base}_${++count}`;
    }
    used.add(name.toLowerCase());
    return {
      slotId: q.id,
      perfettoSql: q.sql,
      tableName: name,
    };
  });
}

// A tab's title / result table name can only be renamed before a query runs.
export function canRenameTab(tab: BigTraceEditorTab): boolean {
  return (
    !tab.isLoading &&
    tab.queryUuid === undefined &&
    tab.queryResult === undefined
  );
}

// A sub-query slot's result table name can only be renamed before it runs.
export function canRenameSubQuery(
  tab: BigTraceEditorTab,
  slot: SubQuerySlot,
): boolean {
  return (
    !tab.isLoading &&
    slot.queryUuid === undefined &&
    slot.queryResult === undefined
  );
}

export function renameSubQuery(
  tab: BigTraceEditorTab,
  index: number,
  newName: string,
): void {
  if (index < 0 || index >= tab.subQueries.length) return;
  const slot = tab.subQueries[index];
  const trimmed = newName.trim();
  if (trimmed === '' || !canRenameSubQuery(tab, slot)) return;
  slot.tableName = trimmed;
  if (index === tab.activeSubQueryIndex) {
    tab.tableName = trimmed;
  }
}

// Mutated in-place by the runner; only QueryTabsState creates/destroys.
export interface BigTraceEditorTab {
  readonly id: string;
  title: string;
  editorText: string;
  queryEngine: 'traces' | 'duckdb';
  tableName: string;
  subQueries: SubQuerySlot[];
  activeSubQueryIndex: number;
  limit: number;
  // Cap on how many traces the run fans out to; a top-level request field
  // like `limit`, defaulted per mode and moved with it while untouched.
  traceLimit: number;
  // Lifetime of the table a persistent run writes, in days.
  tableTtlDays: number;
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
  // Which experiment/control pair and arm the query runs over; undefined =
  // no experiment filtering.
  experimentFilter?: ExperimentFilterState;
  // Per-tab shown columns (display pref, persisted); null = show all.
  resultColumns: readonly string[] | null;
  // Per-tab disabled setting IDs, independent of global /settings. Seeded from
  // globals at creation, then toggled per-tab; excluded from effective settings.
  disabledSettings: readonly string[];
  // The preset last applied to this tab, from the gallery or the settings
  // form's picker. Presets can share one setup (the catalog's query-only ones
  // all have an empty one), so this is the hint for reading the setup back as
  // the preset actually chosen.
  lastPresetId?: string;
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
  readonly queryEngine?: 'traces' | 'duckdb';
  readonly tableName?: string;
  readonly subQueries?: ReadonlyArray<StoredSubQuerySlot>;
  readonly activeSubQueryIndex?: number;
  readonly limit: number;
  readonly traceLimit?: number;
  readonly tableTtlDays?: number;
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
  readonly experimentFilter?: ExperimentFilterState;
  readonly resultColumns?: ReadonlyArray<string> | null;
  readonly disabledSettings?: ReadonlyArray<string>;
  readonly configured?: boolean;
  readonly lastPresetId?: string;
}

interface StoredState {
  readonly tabs: ReadonlyArray<StoredTab>;
  readonly activeTabId?: string;
  readonly activeEngine?: 'traces' | 'duckdb';
}

// Manages editor tabs + localStorage persistence across page reloads.
export class QueryTabsState {
  tabs: BigTraceEditorTab[] = [];
  activeTabId = '';
  activeEngine: 'traces' | 'duckdb' = 'traces';
  private lastActiveTabByEngine: Partial<Record<'traces' | 'duckdb', string>> =
    {};

  private readonly debouncedSave = debounce(() => this.saveToStorage(), 1000);

  constructor() {
    if (!this.loadFromStorage()) {
      this.addNewTab(undefined, DEFAULT_SQL);
    }
  }

  markDirty(): void {
    this.debouncedSave();
  }

  saveNow(): void {
    this.saveToStorage();
  }

  getActiveTab(): BigTraceEditorTab | undefined {
    return this.tabs.find((t) => t.id === this.activeTabId);
  }

  getVisibleTabs(): BigTraceEditorTab[] {
    return this.tabs.filter((t) => t.queryEngine === this.activeEngine);
  }

  setEngine(engine: 'traces' | 'duckdb'): void {
    const current = this.getActiveTab();
    if (this.activeEngine === engine && current?.queryEngine === engine) {
      return;
    }
    if (current !== undefined) {
      this.lastActiveTabByEngine[current.queryEngine] = current.id;
    }
    this.activeEngine = engine;
    const visible = this.getVisibleTabs();
    if (visible.length === 0) {
      this.addNewTab(
        undefined,
        DEFAULT_SQL,
        undefined,
        undefined,
        undefined,
        true,
      );
      return;
    }
    const rememberedId = this.lastActiveTabByEngine[engine];
    const remembered = visible.find((t) => t.id === rememberedId);
    const target = remembered ?? visible[visible.length - 1];
    this.activeTabId = target.id;
    this.lastActiveTabByEngine[engine] = target.id;
    this.markDirty();
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
    traceLimit?: number,
  ): BigTraceEditorTab {
    const queryEngine: 'traces' | 'duckdb' =
      stored?.queryEngine === 'duckdb' || stored?.queryEngine === 'traces'
        ? stored.queryEngine
        : this.activeEngine;

    if (!forceNew) {
      const existingTab = this.tabs.find((t) => {
        if (queryUuid && t.queryUuid === queryUuid) return true;
        if (
          !queryUuid &&
          initialQuery &&
          t.editorText === initialQuery &&
          t.queryEngine === queryEngine
        ) {
          return true;
        }
        return false;
      });

      if (existingTab) {
        this.activeEngine = existingTab.queryEngine;
        this.activeTabId = existingTab.id;
        this.lastActiveTabByEngine[existingTab.queryEngine] = existingTab.id;
        this.markDirty();
        return existingTab;
      }
    }

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
    // Restored whole (names included), so a reload needs no catalog refetch.
    // Fresh tabs have no experiment selection; a history-reopen tab gets one
    // from its submit-time snapshot, not from here.
    const experimentFilter = isFromStorage
      ? copyExperimentFilter(stored?.experimentFilter)
      : undefined;
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

    const subQueries: SubQuerySlot[] = Array.isArray(stored?.subQueries)
      ? stored.subQueries.map((s: StoredSubQuerySlot) => ({
          id: typeof s.id === 'string' && s.id !== '' ? s.id : shortUuid(),
          sql: typeof s.sql === 'string' ? s.sql : '',
          tableName:
            typeof s.tableName === 'string' && s.tableName.trim() !== ''
              ? s.tableName
              : defaultTableName(),
          queryUuid: typeof s.queryUuid === 'string' ? s.queryUuid : undefined,
        }))
      : [];
    const activeSubQueryIndex =
      typeof stored?.activeSubQueryIndex === 'number' &&
      stored.activeSubQueryIndex >= 0 &&
      stored.activeSubQueryIndex < subQueries.length
        ? stored.activeSubQueryIndex
        : 0;

    let initialEditorText = initialQuery ?? '';
    let initialTableName =
      (typeof stored?.tableName === 'string' && stored.tableName.trim() !== ''
        ? stored.tableName.trim()
        : undefined) ??
      (typeof title === 'string' && title.trim() !== ''
        ? title.trim()
        : undefined) ??
      defaultTableName();
    let initialQueryUuid = queryUuid;
    const activeSlot = subQueries[activeSubQueryIndex];
    if (activeSlot !== undefined) {
      initialEditorText = activeSlot.sql;
      initialTableName = activeSlot.tableName;
      if (activeSlot.queryUuid) {
        initialQueryUuid = activeSlot.queryUuid;
      }
    }
    for (const slot of subQueries) {
      if (slot.queryUuid) {
        slot.execution = queryStore.getOrCreate(slot.queryUuid, {
          materialized: isPersistent,
          queryEngine,
          perfettoSql: slot.sql,
          tableName: slot.tableName,
        });
      }
    }

    const tab: BigTraceEditorTab = {
      id: shortUuid(),
      title: initialTableName,
      editorText: initialEditorText,
      queryEngine,
      tableName: initialTableName,
      subQueries,
      activeSubQueryIndex,
      // No caller-supplied caps: take the ones that fit the execution mode.
      limit: limit ?? modeDefaults(isPersistent).rowLimit,
      traceLimit:
        traceLimit ??
        (isFromStorage &&
        typeof stored?.traceLimit === 'number' &&
        stored.traceLimit >= 0
          ? stored.traceLimit
          : modeDefaults(isPersistent).traceLimit),
      tableTtlDays:
        isFromStorage &&
        typeof stored?.tableTtlDays === 'number' &&
        stored.tableTtlDays > 0
          ? stored.tableTtlDays
          : DEFAULT_TABLE_TTL_DAYS,
      queryResult: undefined,
      isLoading: false,
      dataSource: undefined,
      querySettings,
      traceFilters,
      traceMetadataColumns,
      traceOrderBy,
      experimentFilter,
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
      configured:
        queryEngine === 'duckdb'
          ? true
          : isFromStorage
            ? (stored?.configured ?? true)
            : isFromHistory
              ? true
              : false,
      lastPresetId: isFromStorage ? stored?.lastPresetId : undefined,
    };
    tab.execution = queryStore.getOrCreate(initialQueryUuid || tab.id, {
      materialized: tab.materialize,
      queryEngine: tab.queryEngine,
    });
    const restoredActiveSlot = subQueries[activeSubQueryIndex];
    if (restoredActiveSlot !== undefined) {
      restoredActiveSlot.execution = tab.execution;
    }
    this.tabs.push(tab);
    this.activeEngine = tab.queryEngine;
    this.activeTabId = tab.id;
    this.lastActiveTabByEngine[tab.queryEngine] = tab.id;
    this.markDirty();
    return tab;
  }

  // Clone a tab: same query and configuration, no results and no queryUuid,
  // so running it creates its own execution instead of colliding with the
  // original's in History.
  cloneTab(
    tabId: string,
    tableName?: string,
    overrideQuery?: string,
    targetSubQueryIndex?: number,
  ): BigTraceEditorTab | undefined {
    const src = this.tabs.find((t) => t.id === tabId);
    if (src === undefined) return undefined;
    const trimmedName = tableName?.trim() || undefined;
    const targetIdx = targetSubQueryIndex ?? src.activeSubQueryIndex;
    const clone = this.addNewTab(
      src.subQueries.length > 1 ? src.title : trimmedName,
      overrideQuery ?? src.editorText,
      src.limit,
      undefined,
      src.materialize,
      true, // forceNew
      {
        tableName: trimmedName,
        queryEngine: src.queryEngine,
        subQueries: src.subQueries.map((s, i) => {
          const isTarget = i === targetIdx;
          const isCurrent = i === src.activeSubQueryIndex;
          const tbl =
            isTarget && trimmedName
              ? trimmedName
              : s.queryUuid === undefined && s.tableName.trim() !== ''
                ? s.tableName.trim()
                : defaultTableName();
          const sql =
            isCurrent && overrideQuery !== undefined ? overrideQuery : s.sql;
          return {
            id: shortUuid(),
            sql,
            tableName: tbl,
          };
        }),
        activeSubQueryIndex: src.activeSubQueryIndex,
        querySettings: src.querySettings,
        disabledSettings: src.disabledSettings,
        traceFilters: src.traceFilters,
        traceMetadataColumns:
          src.traceMetadataColumns === null
            ? null
            : [...src.traceMetadataColumns],
        traceOrderBy: src.traceOrderBy,
        experimentFilter: copyExperimentFilter(src.experimentFilter),
        resultColumns: src.resultColumns,
        configured: true,
        lastPresetId: src.lastPresetId,
        traceLimit: src.traceLimit,
        tableTtlDays: src.tableTtlDays,
      },
      src.traceLimit,
    );
    this.saveToStorage();
    return clone;
  }

  closeTab(tabId: string): void {
    const index = this.tabs.findIndex((t) => t.id === tabId);
    if (index === -1) return;
    const tabToClose = this.tabs[index];
    const sameEngineTabs = this.tabs.filter(
      (t) => t.queryEngine === tabToClose.queryEngine,
    );
    if (sameEngineTabs.length <= 1) return;
    const engineIndex = sameEngineTabs.findIndex((t) => t.id === tabId);
    if (tabToClose.pollInterval !== undefined) {
      window.clearTimeout(tabToClose.pollInterval);
      tabToClose.pollInterval = undefined;
    }
    // Aborts execute_* and any one-off request holding `lifecycle.signal`.
    tabToClose.activeRequest?.abort();
    tabToClose.lifecycle.abort();
    this.tabs.splice(index, 1);
    const remainingEngineTabs = sameEngineTabs.filter((t) => t.id !== tabId);
    const nextTab =
      remainingEngineTabs[
        Math.min(engineIndex, remainingEngineTabs.length - 1)
      ];
    if (this.activeTabId === tabId && nextTab !== undefined) {
      this.activeTabId = nextTab.id;
    }
    if (
      this.lastActiveTabByEngine[tabToClose.queryEngine] === tabId &&
      nextTab !== undefined
    ) {
      this.lastActiveTabByEngine[tabToClose.queryEngine] = nextTab.id;
    }
    this.markDirty();
  }

  renameTab(tabId: string, newTitle: string): void {
    const tab = this.tabs.find((t) => t.id === tabId);
    const trimmed = newTitle.trim();
    if (!tab || !trimmed || !canRenameTab(tab)) return;
    tab.title = trimmed;
    tab.tableName = trimmed;
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
        queryEngine: t.queryEngine,
        tableName: t.tableName,
        subQueries:
          t.subQueries.length > 1
            ? t.subQueries.map((s) => ({
                id: s.id,
                sql: s.sql,
                tableName: s.tableName,
                queryUuid: s.queryUuid,
              }))
            : undefined,
        activeSubQueryIndex:
          t.subQueries.length > 1 ? t.activeSubQueryIndex : undefined,
        limit: t.limit,
        traceLimit: t.traceLimit,
        tableTtlDays: t.tableTtlDays,
        materialize: t.materialize,
        queryUuid: t.queryUuid,
        error: t.queryResult?.error,
        // Persist the per-tab snapshot so Settings-sub-tab edits survive
        // reload; restored via the `stored` arg on addNewTab.
        querySettings: t.querySettings,
        traceFilters: t.traceFilters,
        traceMetadataColumns: t.traceMetadataColumns,
        traceOrderBy: t.traceOrderBy,
        experimentFilter: t.experimentFilter,
        resultColumns: t.resultColumns,
        disabledSettings: t.disabledSettings,
        configured: t.configured,
        lastPresetId: t.lastPresetId,
      })),
      activeTabId: this.activeTabId,
      activeEngine: this.activeEngine,
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
        typeof t.limit === 'number' && t.limit >= 0 ? t.limit : undefined,
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
    const activeTab = this.getActiveTab();
    if (parsed.activeEngine === 'traces' || parsed.activeEngine === 'duckdb') {
      this.activeEngine = parsed.activeEngine;
    } else if (activeTab !== undefined) {
      this.activeEngine = activeTab.queryEngine;
    }
    if (activeTab !== undefined) {
      this.lastActiveTabByEngine[activeTab.queryEngine] = activeTab.id;
    }
    return true;
  }
}

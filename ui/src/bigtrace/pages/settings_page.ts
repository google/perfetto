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

import {EmptyState} from '../../widgets/empty_state';
import {Callout} from '../../widgets/callout';
import {linkify} from '../../widgets/anchor';
import {Intent} from '../../widgets/common';
import m from 'mithril';
import {SettingsShell} from '../../widgets/settings_shell';
import {Switch} from '../../widgets/switch';
import {
  type MultiSelectDiff,
  type MultiSelectOption,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {PopupPosition} from '../../widgets/popup';
import {Card, CardStack} from '../../widgets/card';
import {Icon} from '../../widgets/icon';
import {classNames} from '../../base/classnames';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import type {
  Setting as BigTraceSetting,
  SettingFilter,
} from '../settings/settings_types';
import {renderSetting} from '../settings/settings_widgets';
import {
  type SettingsBindings,
  TabBoundSetting,
} from '../settings/tab_bound_setting';
import {Button, ButtonVariant} from '../../widgets/button';

import {
  endpointStorage,
  getBigtraceEndpoint,
} from '../settings/endpoint_storage';
import type {Setting} from '../../public/settings';

import {TextInput} from '../../widgets/text_input';
import {Stack, StackAuto} from '../../widgets/stack';

import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import type {
  Column,
  Filter,
  SortDirection,
} from '../../components/widgets/datagrid/model';
import {
  BigtraceQueryClient,
  type TraceColumnDescriptor,
  type TracesSchemaResponse,
} from '../query/bigtrace_query_client';
import {BigtraceTraceListDataSource} from '../query/bigtrace_trace_list_data_source';
import {formatCompact} from '../query/query_store';
import {
  traceFilterState as traceFiltersState,
  traceOrderByState,
  traceColumnsState,
  traceQueryColumnsState,
  effectiveQueryColumns,
} from '../settings/trace_selection_state';
import {linkColumnFirst, LINK_COLUMN} from '../settings/column_order';

interface BigTraceSettingsCardAttrs extends m.Attributes {
  id?: string;
  title: string;
  controls: m.Children;
  description?: m.Children;
  disabled?: boolean;
  onChange?: (disabled: boolean) => void;
  fullWidthControls?: boolean;
  // Shows a "reset to default" affordance; pass only when value differs from default.
  onReset?: () => void;
}

class BigTraceSettingsCard
  implements m.ClassComponent<BigTraceSettingsCardAttrs>
{
  view(vnode: m.Vnode<BigTraceSettingsCardAttrs>) {
    const {
      id,
      title,
      controls,
      description,
      disabled,
      onChange,
      fullWidthControls,
      onReset,
      ...rest
    } = vnode.attrs;

    const details = m(
      '.pf-settings-card__details',
      m('.pf-settings-card__title.pf-bt-settings-card-title', [
        disabled !== undefined &&
          m(Switch, {
            className: 'pf-settings-card__toggle',
            style: {marginRight: '8px'},
            checked: !disabled,
            title:
              'Turn off to skip this filter — its value will not be ' +
              'sent to the backend with subsequent queries.',
            onchange: (e: Event) => {
              const target = e.target as HTMLInputElement;
              onChange?.(!target.checked);
            },
          }),
        title,
        onReset &&
          m(Button, {
            icon: 'settings_backup_restore',
            title: 'Reset this setting to its default value.',
            className: 'pf-bt-settings-card-reset',
            onclick: () => onReset(),
          }),
      ]),
      description !== undefined &&
        m('.pf-settings-card__description', description),
    );

    const controlsEl = m(
      '.pf-settings-card__controls',
      {
        className: classNames(
          disabled !== undefined &&
            disabled &&
            'pf-bt-settings-controls--disabled',
        ),
        style: fullWidthControls
          ? {gridColumn: '1 / -1', minWidth: '0'}
          : undefined,
      },
      controls,
    );

    return m(
      'div',
      {
        className: classNames(
          disabled && 'pf-bt-settings-card-wrapper--disabled',
        ),
      },
      m(
        Card,
        {
          id,
          className: classNames('pf-settings-card', disabled && 'pf-disabled'),
          ...rest,
        },
        [details, controlsEl],
      ),
    );
  }
}

// Trace-selection-grid section label. Must match CATEGORY_DISPLAY_NAMES so the
// renderer can branch on it.
const TRACE_ADDRESS_DISPLAY = 'Trace Address';

const SCHEMA_ROOT = 'trace_list';

// SchemaRegistry from /trace_metadata_schema: one entry per column, default
// string renderer (every cell is a string per the always-strings contract).
function buildSchemaRegistry(
  schema: ReadonlyArray<TraceColumnDescriptor>,
): SchemaRegistry {
  const columnSchema: ColumnSchema = {};
  for (const c of schema) {
    // The `link` column renders as a clickable link; all others as strings.
    columnSchema[c.name] =
      c.name === LINK_COLUMN
        ? {
            cellRenderer: (value) =>
              value === null || value === undefined
                ? ''
                : linkify(String(value)),
          }
        : {cellRenderer: undefined};
  }
  return {[SCHEMA_ROOT]: columnSchema};
}

interface SchemaError {
  readonly kind: 'error';
  readonly message: string;
}
type SchemaState = undefined | 'loading' | SchemaError | TracesSchemaResponse;

export interface SettingsPageAttrs {
  // When set, reads/writes route through the bindings instead of global state.
  // The /settings route mounts without bindings; the Query page's "Bigtrace
  // Settings" sub-tab mounts with per-tab bindings.
  readonly bindings?: SettingsBindings;
}

// AIP-132 single-field order_by helpers. The DataGrid supports only one active
// sort column, so multi-field strings persist verbatim but only the first entry
// round-trips into the UI's sort state. Returns undefined for empty/unparseable
// input so the caller falls back to "no sort applied".
function parseSingleFieldOrderBy(
  raw: string,
): {field: string; direction: SortDirection} | undefined {
  const token = raw.split(',', 1)[0]?.trim();
  if (!token) return undefined;
  const [field, dir] = token.split(/\s+/);
  if (!field) return undefined;
  const lower = (dir ?? 'asc').toLowerCase();
  if (lower !== 'asc' && lower !== 'desc') return undefined;
  return {field, direction: lower === 'asc' ? 'ASC' : 'DESC'};
}

function formatSingleFieldOrderBy(
  col: {field: string; sort?: SortDirection} | undefined,
): string {
  if (!col?.sort) return '';
  return `${col.field} ${col.sort.toLowerCase()}`;
}

export class SettingsPage implements m.ClassComponent<SettingsPageAttrs> {
  private searchQuery = '';
  // Captured on every view() so private methods read it without threading attrs.
  private bindings: SettingsBindings | undefined;
  // Trace-list grid state. Rebuilt whenever the endpoint changes (its
  // BigtraceQueryClient binds to one endpoint at construction). With bindings
  // set, the data source's `getSettings` callback routes through them so
  // /trace_metadata sees the per-tab snapshot, not the global defaults.
  private traceListDataSource: BigtraceTraceListDataSource | undefined;
  private traceListEndpoint: string | undefined;
  private traceFilterss: readonly Filter[] = [];
  // Sort state for the trace grid. The DataGrid carries sort on the `Column`
  // object, so controlled-mode `columns` splices it back onto the matching
  // column every render, else the click that set it is discarded on the next
  // redraw. Persisted to `traceOrderByState` because the sort is functionally
  // significant (under `trace_limit > 0` it picks which traces run first);
  // seeding on oninit survives a reload.
  private traceListSortField: string | undefined;
  private traceListSortDirection: SortDirection | undefined;
  // /trace_metadata_schema response. undefined = not yet requested; 'loading' =
  // in flight; SchemaError = failed; else the resolved response.
  private schemaState: SchemaState = undefined;
  // Keyed on endpoint + effective settings: the schema can vary by trace source
  // (TRACE_ADDRESS settings), so a source change must refetch — endpoint-only
  // keying would serve a stale catalog.
  private schemaKey: string | undefined;
  // One schema fetch at a time. A key change mid-flight is picked up once the
  // fetch settles, so rapid source edits coalesce instead of racing.
  private schemaFetching = false;
  oninit({attrs}: m.Vnode<SettingsPageAttrs>) {
    this.bindings = attrs.bindings;
    this.traceFilterss = this.readTraceFilters();
    const parsed = parseSingleFieldOrderBy(this.readTraceOrderBy());
    this.traceListSortField = parsed?.field;
    this.traceListSortDirection = parsed?.direction;
    bigTraceSettingsStorage.loadSettings();
  }

  // Binding-aware accessors (fall back to globals).

  private readTraceFilters(): readonly Filter[] {
    return this.bindings
      ? this.bindings.getTraceFilters()
      : traceFiltersState.get();
  }

  private writeTraceFilters(filters: readonly Filter[]): void {
    if (this.bindings) this.bindings.setTraceFilters(filters);
    else traceFiltersState.set(filters);
  }

  // null = unchosen (resolves to defaultVisible); [] = attach nothing.
  private readTraceMetadataColumns(): readonly string[] | null {
    return this.bindings
      ? this.bindings.getTraceMetadataColumns()
      : traceQueryColumnsState.get();
  }

  private readTraceOrderBy(): string {
    return this.bindings
      ? this.bindings.getTraceOrderBy()
      : traceOrderByState.get();
  }

  private writeTraceOrderBy(orderBy: string): void {
    if (this.bindings) this.bindings.setTraceOrderBy(orderBy);
    else traceOrderByState.set(orderBy);
  }

  // `null` resets to unchosen; a concrete list (incl. []) is stored verbatim.
  private writeTraceMetadataColumns(cols: readonly string[] | null): void {
    if (this.bindings) this.bindings.setTraceMetadataColumns(cols);
    else traceQueryColumnsState.set(cols);
  }

  // Effective settings for outgoing /trace_metadata[_schema] requests. With
  // bindings set, the per-tab snapshot wins so the grid reflects the same
  // trace_directory / trace_limit the next Run uses, not /settings defaults.
  private effectiveSettings(): ReadonlyArray<SettingFilter> {
    return this.bindings
      ? this.bindings.getEffectiveSettings()
      : bigTraceSettingsStorage.buildSettingFilters();
  }

  // Wrap a globally-registered setting so its widget reads/writes per-tab.
  // No-op when bindings is undefined (returns the original).
  private boundSetting(
    setting: BigTraceSetting<unknown>,
  ): BigTraceSetting<unknown> {
    if (!this.bindings) return setting;
    return new TabBoundSetting(setting, this.bindings);
  }

  private static readonly CATEGORY_DISPLAY_NAMES: ReadonlyMap<string, string> =
    new Map([
      ['General', 'General'],
      ['TRACE_ADDRESS', TRACE_ADDRESS_DISPLAY],
      ['TRACE_METADATA', 'Trace Metadata'],
      ['BIGTRACE_QUERY_OPTIONS', 'Query Options'],
    ]);

  private displayCategory(raw: string): string {
    return SettingsPage.CATEGORY_DISPLAY_NAMES.get(raw) ?? raw;
  }

  // Lazily build/rebuild the trace-list data source. BigtraceQueryClient binds
  // to one endpoint at construction, so an endpoint change needs a fresh
  // DataSource (the caller keys the DataGrid on the endpoint so Mithril
  // rebuilds it).
  private getTraceListDataSource(
    endpoint: string,
  ): BigtraceTraceListDataSource | undefined {
    if (endpoint === '') {
      this.traceListDataSource = undefined;
      this.traceListEndpoint = undefined;
      return undefined;
    }
    if (
      this.traceListDataSource === undefined ||
      this.traceListEndpoint !== endpoint
    ) {
      const client = new BigtraceQueryClient(endpoint);
      // `getSettings` runs on every fetch, so a per-tab caller sees latest
      // snapshot edits without rebuilding the data source.
      this.traceListDataSource = new BigtraceTraceListDataSource(client, () =>
        this.effectiveSettings(),
      );
      this.traceListEndpoint = endpoint;
    }
    return this.traceListDataSource;
  }

  // Resolved schema, or undefined while loading/errored. The toggle widget and
  // column-picker menu both go through this so one fetch backs both.
  private resolvedSchema(): TracesSchemaResponse | undefined {
    const s = this.schemaState;
    if (s === undefined || s === 'loading') return undefined;
    if ('kind' in s) return undefined;
    return s;
  }

  // Fetch /trace_metadata_schema, keyed on endpoint + effective settings (see
  // schemaKey / schemaFetching for the keying and in-flight rationale).
  private ensureSchemaFetched(endpoint: string): void {
    if (endpoint === '') {
      this.schemaState = undefined;
      this.schemaKey = undefined;
      return;
    }
    // Key on endpoint + only the TRACE_ADDRESS (source) settings: the schema
    // varies by source, so a query-option/metadata edit shouldn't refetch. The
    // fetch itself still sends every setting.
    const sourceSettings = this.effectiveSettings().filter(
      (s) => s.category === 'TRACE_ADDRESS',
    );
    const key = `${endpoint}|${JSON.stringify(sourceSettings)}`;
    if (this.schemaKey === key && this.schemaState !== undefined) {
      return;
    }
    if (this.schemaFetching) {
      return;
    }
    this.schemaKey = key;
    this.schemaState = 'loading';
    this.schemaFetching = true;
    const client = new BigtraceQueryClient(endpoint);
    client
      .listTraceMetadataSchema(this.effectiveSettings())
      .then((resp) => {
        this.schemaFetching = false;
        // Stale-response guard: drop if the key moved on (endpoint cleared, or
        // source changed).
        if (this.schemaKey !== key) {
          m.redraw();
          return;
        }
        this.schemaState = resp;
        m.redraw();
      })
      .catch((e: unknown) => {
        this.schemaFetching = false;
        if (this.schemaKey !== key) {
          m.redraw();
          return;
        }
        this.schemaState = {
          kind: 'error',
          message: e instanceof Error ? e.message : String(e),
        };
        m.redraw();
      });
  }

  // Splice the sort state onto its column so the header sort indicator survives
  // controlled-mode redraws.
  private buildTraceListColumns(names: ReadonlyArray<string>): Column[] {
    return names.map((n) => {
      const base: Column = {id: n, field: n};
      if (
        this.traceListSortField === n &&
        this.traceListSortDirection !== undefined
      ) {
        return {...base, sort: this.traceListSortDirection};
      }
      return base;
    });
  }

  // Apply a column-set change from either affordance (toggle row or DataGrid
  // header menu); both converge here so they can't drift.
  private updateChosenColumns(names: readonly string[]): void {
    if (names.length === 0) {
      // At least one column must be visible; reset to defaults instead.
      traceColumnsState.clear();
    } else {
      traceColumnsState.set(names);
    }
    m.redraw();
  }

  // Renders the "Traces" card: a caption, a column-picker toggle row, and a
  // DataGrid driven by the trace-list DataSource. The toggle row and the grid's
  // "Add column" menu both write through `traceColumnsState`, so they stay in
  // sync.
  private renderTraceListCard(endpoint: string): m.Children {
    const ds = this.getTraceListDataSource(endpoint);
    if (ds === undefined) {
      return m(
        Card,
        {className: 'pf-settings-card'},
        m('.pf-settings-card__details', [
          m('.pf-settings-card__title', 'Traces'),
          m(
            '.pf-settings-card__description',
            'Set the BigTrace Endpoint above to load traces from your ' +
              'configured directory.',
          ),
        ]),
      );
    }
    this.ensureSchemaFetched(endpoint);
    const schema = this.resolvedSchema();
    const schemaState = this.schemaState;

    const header: m.Children = [
      m('.pf-bt-trace-card__title-row', [
        m('.pf-settings-card__title', 'Traces'),
        // Forces a /trace_metadata refetch with the current filter/sort/
        // columns/settings. Sits next to the title so it's obvious which list
        // it refreshes.
        m(Button, {
          icon: 'refresh',
          className: 'pf-bt-trace-card__refresh',
          title:
            'Refresh trace list — re-fetch /trace_metadata with the current ' +
            'filter and settings.',
          onclick: () => {
            void ds.refresh();
          },
        }),
      ]),
      m(
        '.pf-settings-card__description',
        'Filter or sort to select which traces the query runs over.',
      ),
    ];

    if (schemaState === 'loading' || schemaState === undefined) {
      return m(
        Card,
        {className: 'pf-settings-card', style: {display: 'block'}},
        [
          header,
          m(EmptyState, {title: 'Loading schema…', icon: 'hourglass_empty'}),
        ],
      );
    }
    if (schemaState !== undefined && 'kind' in schemaState) {
      return m(
        Card,
        {className: 'pf-settings-card', style: {display: 'block'}},
        [
          header,
          m(
            Callout,
            {
              intent: Intent.Danger,
              icon: 'error',
              title: 'Failed to load trace schema',
            },
            schemaState.message,
          ),
        ],
      );
    }

    // Schema resolved: build the column list from the effective selection.
    const chosen = traceColumnsState.effective(schema!.columns);
    const schemaRegistry = buildSchemaRegistry(schema!.columns);

    return m(
      Card,
      {
        className: 'pf-settings-card pf-bt-trace-card',
        // Top margin separates this from the plain key-value cards above (Trace
        // Directory, Trace Limit); padding-bottom keeps the grid clear of the
        // card border.
        style: {
          display: 'block',
          marginTop: '32px',
          paddingBottom: '16px',
        },
      },
      [
        header,
        this.renderColumnPicker(schema!.columns, chosen),
        m(
          '.pf-bt-trace-list-grid',
          {
            // Fixed height bounds the inner virtualized Grid's viewport:
            // without it the DataGrid's `height: 100%` resolves against an
            // auto-height parent and renders every row (catastrophic for a
            // large trace directory). This scrolling card must set its own
            // height; 500px engages virtualization while staying generous.
            style: {height: '500px', marginTop: '16px'},
          },
          m(DataGrid, {
            schema: schemaRegistry,
            rootSchema: SCHEMA_ROOT,
            data: ds,
            // Inner virtualized Grid uses the wrapper's 500px as its viewport.
            fillHeight: true,
            // Controlled-mode columns: render exactly what the user picked, in
            // their order. The grid's header menus ("Add"/"Remove column")
            // emit onColumnsChanged, persisted to traceColumnsState — the same
            // write path as the toggle widget above.
            columns: this.buildTraceListColumns(chosen),
            onColumnsChanged: (cols: ReadonlyArray<Column>) => {
              // Extract sort (it lives on the Column object) before collapsing
              // cols to string[] so the next render can splice it back, else
              // the header click reverts each redraw. Persisted to
              // traceOrderByState so a reload keeps it; a Run ships it as
              // `trace_order_by` on /execute_*.
              const sorted = cols.find((c) => c.sort);
              this.traceListSortField = sorted?.field;
              this.traceListSortDirection = sorted?.sort;
              this.writeTraceOrderBy(formatSingleFieldOrderBy(sorted));
              this.updateChosenColumns(cols.map((c) => c.field));
            },
            canAddColumns: true,
            canRemoveColumns: true,
            // Controlled-mode filter: source of truth is the binding (per-tab
            // snapshot) or `traceFiltersState` (global on /settings). Persisted
            // immediately so a Run picks it up without a separate "apply".
            filters: this.traceFilterss,
            onFiltersChanged: (filters: readonly Filter[]) => {
              this.traceFilterss = filters;
              this.writeTraceFilters(filters);
            },
            emptyStateMessage:
              'No traces match your filter (or Trace Directory is empty).',
            disablePivotControls: true,
            // How many traces the current filter (or trace_directory alone)
            // selects. Backed by the data source's filteredTotalRows.
            toolbarItemsLeft: [this.renderTraceMatchCount(ds)],
          }),
        ),
      ],
    );
  }

  // Sibling card below Traces. Its own title/description keep the "shown in the
  // grid" picker distinct from the "attached to query results" picker. Renders
  // nothing while schema loads.
  private renderQueryColumnsCard(): m.Children {
    const schema = this.resolvedSchema();
    if (schema === undefined) return null;
    return m(
      Card,
      {
        className: 'pf-settings-card pf-bt-query-columns-card',
        style: {
          display: 'block',
          marginTop: '24px',
          paddingBottom: '16px',
        },
      },
      [
        m('.pf-settings-card__title', 'Query Result Columns'),
        m(
          '.pf-settings-card__description',
          'Trace metadata to attach to every query result row.',
        ),
        this.renderQueryColumnsPicker(schema.columns),
      ],
    );
  }

  // Single-line summary of how many traces match, shown in the grid toolbar.
  // Uses the data source's `filteredTotalRows` (post-filter count; equals the
  // trace-directory total when no filter set).
  private renderTraceMatchCount(ds: BigtraceTraceListDataSource): m.Children {
    const n = ds.filteredTotalRows;
    // Report the count to the embedded caller so a closed-drawer summary can
    // show it without re-fetching. No-op on /settings (no onTraceMatchCount).
    this.bindings?.onTraceMatchCount?.(n);
    const hasFilter = this.traceFilterss.length > 0;
    // Compact count (1.2K) like the history sidebar's row counts; the exact
    // number lives in the hover title.
    const text =
      n === undefined
        ? 'Counting traces…'
        : hasFilter
          ? `${formatCompact(n)} trace${n === 1 ? '' : 's'} match`
          : `${formatCompact(n)} trace${n === 1 ? '' : 's'}`;
    // Filled label that reads as a status, not a clickable chip.
    return m(
      'span.pf-bt-trace-match-count',
      {
        title:
          n === undefined
            ? undefined
            : `${n.toLocaleString()} trace${n === 1 ? '' : 's'}`,
        style: {
          display: 'inline-flex',
          alignItems: 'center',
          padding: '2px 8px',
          borderRadius: '4px',
          fontSize: '12px',
          fontWeight: '500',
          background: 'var(--pf-color-background-tertiary, #e3e9eb)',
          color: 'var(--pf-color-text-muted, #555)',
        },
      },
      text,
    );
  }

  // "Restore defaults" — shown only when customized.
  private renderRestoreDefaultsButton(
    customized: boolean,
    title: string,
    onReset: () => void,
  ): m.Children {
    if (!customized) return null;
    return m(Button, {
      label: 'Restore defaults',
      icon: 'settings_backup_restore',
      title,
      onclick: () => {
        onReset();
        m.redraw();
      },
    });
  }

  // Picks the trace-metadata columns attached to each result row. Unchosen
  // (null) = defaultVisible; uncheck all → [] = nothing.
  private renderQueryColumnsPicker(
    schemaCols: ReadonlyArray<TraceColumnDescriptor>,
  ): m.Children {
    const chosen = effectiveQueryColumns(
      this.readTraceMetadataColumns(),
      schemaCols,
    );
    const chosenSet = new Set(chosen);
    const customized = this.readTraceMetadataColumns() !== null;
    const options: MultiSelectOption[] = linkColumnFirst(
      schemaCols,
      (c) => c.name,
    ).map((col) => ({
      id: col.name,
      name: col.name,
      checked: chosenSet.has(col.name),
      details: col.description,
    }));
    return m(
      '.pf-bt-trace-query-columns',
      {
        style: {
          marginTop: '16px',
          display: 'flex',
          alignItems: 'center',
          gap: '8px',
        },
      },
      m(PopupMultiSelect, {
        label: 'Columns to attach',
        icon: 'label',
        showNumSelected: true,
        showSelectAllButton: true,
        position: PopupPosition.Bottom,
        options,
        onChange: (diffs: ReadonlyArray<MultiSelectDiff>) => {
          let next = [...chosen];
          for (const d of diffs) {
            if (d.checked) {
              if (!next.includes(d.id)) next.push(d.id);
            } else {
              next = next.filter((n) => n !== d.id);
            }
          }
          this.writeTraceMetadataColumns(next);
          m.redraw();
        },
      }),
      this.renderRestoreDefaultsButton(
        customized,
        "Attach the backend's default columns, and keep tracking that " +
          'default as it changes.',
        () => this.writeTraceMetadataColumns(null),
      ),
    );
  }

  // Popup multi-select for the trace grid's visible columns: one checkable
  // option per column. Each /trace_metadata_schema `description` becomes the
  // option's hover tooltip (the widget's `details`).
  private renderColumnPicker(
    schemaCols: ReadonlyArray<TraceColumnDescriptor>,
    chosen: ReadonlyArray<string>,
  ): m.Children {
    const chosenSet = new Set(chosen);
    // Backed by the global traceColumnsState only (no per-tab binding).
    const customized = traceColumnsState.get() !== null;
    const options: MultiSelectOption[] = linkColumnFirst(
      schemaCols,
      (c) => c.name,
    ).map((col) => ({
      id: col.name,
      name: col.name,
      checked: chosenSet.has(col.name),
      details: col.description,
    }));
    return m(
      '.pf-bt-trace-columns',
      {
        style: {
          marginTop: '20px',
          display: 'flex',
          alignItems: 'center',
          gap: '8px',
        },
      },
      m(PopupMultiSelect, {
        label: 'Shown columns',
        icon: 'view_column',
        showNumSelected: true,
        showSelectAllButton: true,
        position: PopupPosition.Bottom,
        options,
        onChange: (diffs: ReadonlyArray<MultiSelectDiff>) => {
          this.applyColumnDiffs(chosen, diffs);
        },
      }),
      this.renderRestoreDefaultsButton(
        customized,
        "Show the backend's default columns in the grid.",
        () => traceColumnsState.clear(),
      ),
    );
  }

  // Apply MultiSelect diffs to the shown-columns set, preserving check order:
  // newly-checked appended, unchecked removed in place.
  private applyColumnDiffs(
    chosen: ReadonlyArray<string>,
    diffs: ReadonlyArray<MultiSelectDiff>,
  ): void {
    let next = [...chosen];
    for (const d of diffs) {
      if (d.checked) {
        if (!next.includes(d.id)) next.push(d.id);
      } else {
        next = next.filter((n) => n !== d.id);
      }
    }
    this.updateChosenColumns(next);
  }

  view({attrs}: m.Vnode<SettingsPageAttrs>) {
    // Refresh bindings each render so callers can swap them without remounting.
    this.bindings = attrs.bindings;
    const embedded = this.bindings !== undefined;
    const endpointSetting = endpointStorage.get('bigtraceEndpoint');

    const query = this.searchQuery.toLowerCase();
    const categories = new Map<string, BigTraceSetting<unknown>[]>();

    // Show General (the endpoint) only on the standalone /settings route. The
    // endpoint is a connection, not a query, so it stays global and out of the
    // per-tab snapshot UI.
    if (endpointSetting && !embedded) {
      categories.set('General', []);
    }

    const settings = bigTraceSettingsStorage
      .getAllSettings()
      .filter(
        (s) =>
          s.name.toLowerCase().includes(query) ||
          s.description.toLowerCase().includes(query),
      );

    for (const setting of settings) {
      const categoryName = this.displayCategory(setting.category || 'General');
      if (!categories.has(categoryName)) {
        categories.set(categoryName, []);
      }
      categories.get(categoryName)!.push(setting);
    }

    // "No matches" hint when search hides everything but the General card.
    const hasOtherMatches = settings.length > 0;
    const showNoMatchesHint =
      this.searchQuery !== '' &&
      !hasOtherMatches &&
      !bigTraceSettingsStorage.execConfigLoadError;

    const body = m('.pf-bt-settings-page', [
      bigTraceSettingsStorage.isExecConfigLoading &&
        m(EmptyState, {
          title: 'Loading settings...',
          icon: 'hourglass_empty',
          fillHeight: true,
        }),
      Array.from(categories.entries()).map(([category, catSettings]) => {
        const categoryHeader: m.Children = m(
          'h2.pf-bt-settings-page__plugin-title',
          category,
        );

        const cards: m.Children[] = [];
        // Render the endpoint card inside "General".
        if (category === 'General' && endpointSetting) {
          // Reset restores the default URL; applied via "Reload to apply".
          const endpointAtDefault =
            JSON.stringify(endpointSetting.get()) ===
            JSON.stringify(endpointSetting.defaultValue);
          cards.push(
            m(BigTraceSettingsCard, {
              id: endpointSetting.id,
              title: endpointSetting.name,
              description: endpointSetting.description,
              disabled: undefined,
              controls: this.renderEndpointControl(endpointSetting),
              onReset: endpointAtDefault
                ? undefined
                : () => endpointSetting.reset(),
            }),
          );
        }
        for (const setting of catSettings) {
          cards.push(this.renderBigTraceSettingCard(setting));
        }
        // Below the trace_directory/trace_limit cards, two sibling cards:
        // "Traces" picks WHICH traces, "Query Result Columns" picks WHAT
        // metadata attaches to each result row. Both omitted while searching.
        if (category === TRACE_ADDRESS_DISPLAY && this.searchQuery === '') {
          const endpoint = getBigtraceEndpoint();
          cards.push(this.renderTraceListCard(endpoint));
          cards.push(this.renderQueryColumnsCard());
        }
        const categoryContent = m(CardStack, cards);

        return m(
          '.pf-bt-settings-page__plugin-section',
          categoryHeader,
          categoryContent,
        );
      }),
      // After the General card so the callout's "Set the Endpoint above" copy
      // points at a field above it.
      bigTraceSettingsStorage.execConfigLoadError &&
        m(
          Callout,
          {
            intent: Intent.Danger,
            icon: 'error',
            title: 'Failed to Load Execution Configuration',
          },
          bigTraceSettingsStorage.execConfigLoadError,
        ),
      showNoMatchesHint &&
        m(EmptyState, {
          title: `No settings match "${this.searchQuery}"`,
          icon: 'search_off',
        }),
    ]);

    // Embedded in the Query page's "Bigtrace Settings" sub-tab: skip the
    // SettingsShell chrome (title + sticky search). The pill row already labels
    // the surface, so a second "Settings" header would be noise.
    if (embedded) {
      return m('.pf-bt-settings-embedded', body);
    }
    return m(
      SettingsShell,
      {
        title: 'Settings',
        className: 'page',
        // Reload-required affordance lives next to the endpoint input, not
        // here.
        stickyHeaderContent: m(
          Stack,
          {orientation: 'horizontal'},
          m(StackAuto),
          m(TextInput, {
            placeholder: 'Search...',
            value: this.searchQuery,
            leftIcon: 'search',
            oninput: (e: Event) => {
              this.searchQuery = (e.target as HTMLInputElement).value;
            },
          }),
        ),
      },
      body,
    );
  }

  private renderEndpointControl(setting: Setting<unknown>) {
    const currentValue = setting.get() as string;
    return m(
      Stack,
      {
        orientation: 'horizontal',
        gap: '8px',
        alignItems: 'center',
        style: {flexWrap: 'wrap', justifyContent: 'flex-end'},
      },
      m(TextInput, {
        value: currentValue,
        placeholder: 'https://your-bigtrace-backend/v1',
        style: {width: 'min(300px, 30vw)'},
        oninput: (e: Event) => {
          const target = e.target as HTMLInputElement;
          setting.set(target.value);
        },
      }),
      // Endpoint is cached at module init, so changes need a reload.
      endpointStorage.isReloadRequired() &&
        m(Button, {
          label: 'Reload to apply',
          icon: 'refresh',
          intent: Intent.Primary,
          variant: ButtonVariant.Filled,
          onclick: () => window.location.reload(),
        }),
    );
  }

  private renderBigTraceSettingCard(rawSetting: BigTraceSetting<unknown>) {
    const setting = this.boundSetting(rawSetting);
    // Enable/disable goes through the bound setting: per-tab when embedded,
    // global on /settings — so a per-tab toggle doesn't leak to global state.
    const disabled = setting.isDisabled();
    const fullWidth =
      setting.type === 'string-array' ||
      (setting.type === 'string' && setting.format === 'sql');
    // Flag enabled-but-empty filters. Numeric settings excluded: 0 is valid
    // (= unlimited).
    const needsValue =
      !disabled &&
      (setting.type === 'string' || setting.type === 'string-array');
    let warning: string | undefined;
    if (needsValue) {
      const value = setting.get();
      if (setting.type === 'string') {
        if (typeof value === 'string' && value.trim() === '') {
          warning = 'Required when this filter is enabled.';
        }
      } else if (setting.type === 'string-array') {
        if (
          !Array.isArray(value) ||
          value.length === 0 ||
          value.every((v) => typeof v === 'string' && v.trim() === '')
        ) {
          warning = 'Required when this filter is enabled.';
        }
      }
    }
    // "(unlimited)" hint on a numeric setting at 0 whose description says
    // "ignored if 0".
    let hint: string | undefined;
    if (
      !disabled &&
      setting.type === 'number' &&
      setting.get() === 0 &&
      /ignored if 0/i.test(setting.description)
    ) {
      hint = '(unlimited)';
    }
    const description: m.Children = warning
      ? [
          setting.description,
          m(
            '.pf-settings-card__warning',
            {
              style: {
                color: 'var(--pf-color-danger, #b00020)',
                marginTop: '4px',
              },
            },
            m(Icon, {
              icon: 'warning',
              style: {fontSize: '14px', verticalAlign: 'middle'},
            }),
            ' ',
            warning,
          ),
        ]
      : hint
        ? [
            setting.description,
            ' ',
            m(
              'span.pf-settings-card__hint',
              {style: {opacity: 0.7, fontStyle: 'italic'}},
              hint,
            ),
          ]
        : setting.description;
    // Booleans carry on/off in the value control, so a second enable/disable
    // Switch would confuse — suppress it (disabled: undefined hides it). Every
    // other type gets the Switch.
    const showToggle = setting.type !== 'boolean';
    // Reset shown only when value ≠ default. JSON compare because the setting's
    // built-in default check uses === (unsafe for arrays).
    const atDefault =
      JSON.stringify(setting.get()) === JSON.stringify(setting.defaultValue);
    return m(BigTraceSettingsCard, {
      id: setting.id,
      title: setting.name,
      description,
      controls: renderSetting(setting),
      disabled: showToggle ? disabled : undefined,
      fullWidthControls: fullWidth,
      onChange: showToggle
        ? (newDisabled: boolean) => {
            setting.setDisabled(newDisabled);
          }
        : undefined,
      onReset: atDefault ? undefined : () => setting.reset(),
    });
  }
}

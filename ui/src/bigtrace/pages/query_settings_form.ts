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

// The settings-card styles live with the shared SettingsShell widget; this
// form renders those cards without the shell, so it pulls the stylesheet in.
import '../../widgets/settings_shell.scss';
import {EmptyState} from '../../widgets/empty_state';
import {Callout} from '../../widgets/callout';
import {linkify} from '../../widgets/anchor';
import {Intent} from '../../widgets/common';
import m from 'mithril';
import {Switch} from '../../widgets/switch';
import {TextInput} from '../../widgets/text_input';
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
import {Button} from '../../widgets/button';

import {getBigtraceEndpoint} from '../settings/endpoint_storage';

import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {ColumnSchema} from '../../components/widgets/datagrid/datagrid_schema';
import type {
  Column,
  Filter,
  SortDirection,
} from '../../components/widgets/datagrid/model';
import {
  BigtraceQueryClient,
  toExperimentFilterSpec,
  type TraceColumnDescriptor,
  type TracesSchemaResponse,
} from '../query/bigtrace_query_client';
import {ExperimentFilterControl} from './experiment_filter_control';
import {BigtraceTraceListDataSource} from '../query/bigtrace_trace_list_data_source';
import {formatCompact} from '../query/query_store';
import {effectiveQueryColumns} from '../settings/trace_selection_state';
import {
  isTraceSelectionSetting,
  parseTraceUuids,
  TRACE_UUIDS_SETTING_ID,
  traceUuidsDeclared,
  traceUuidsState,
} from './query_tabs_state';
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

class BigTraceSettingsCard implements m.ClassComponent<BigTraceSettingsCardAttrs> {
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

// Schema from /trace_metadata_schema: one entry per column, default string
// renderer (every cell is a string per the always-strings contract).
function columnSchema(
  schema: ReadonlyArray<TraceColumnDescriptor>,
): ColumnSchema {
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
  return columnSchema;
}

interface SchemaError {
  readonly kind: 'error';
  readonly message: string;
}
type SchemaState = undefined | 'loading' | SchemaError | TracesSchemaResponse;

export interface QuerySettingsFormAttrs {
  // Every read and write routes through these, so the form always edits one
  // tab's configuration — there is no global settings state behind it.
  readonly bindings: SettingsBindings;
  // Which half of the configuration to show. 'trace-selection' is the
  // launcher page: which traces a query runs over — the trace grid first,
  // then the source settings and the result-metadata columns.
  // 'query-options' is the gear modal: how it runs — the trace cap and every
  // remaining setting.
  readonly scope: 'trace-selection' | 'query-options';
  // Rendered above the sections, scrolling with them (the launcher puts its
  // preset gallery here). Anything it does to the tab's selection or order
  // shows up on the next render: the grid state is re-read each view.
  readonly header?: m.Children;
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

export class QuerySettingsForm implements m.ClassComponent<QuerySettingsFormAttrs> {
  // Captured on every view() so private methods read it without threading
  // attrs. Set in oninit before any read.
  private bindings!: SettingsBindings;
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
  // redraw. Written through to the tab because the sort is functionally
  // significant (under a trace cap it picks which traces run first); seeding
  // on oninit restores it when the form is reopened.
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
  // UUID-mode paste box: the raw text being edited, and the canonical join of
  // the value it was last synced with (so an external write resets the draft
  // while the user's own typing survives re-renders).
  private uuidDraft: string | undefined;
  private uuidDraftBase: string | undefined;
  oninit({attrs}: m.Vnode<QuerySettingsFormAttrs>) {
    this.bindings = attrs.bindings;
    this.syncFromBindings();
    bigTraceSettingsStorage.loadSettings();
  }

  // Pull the grid's filter and sort state from the tab. The grid writes them
  // through as the user edits, but so can the header (a preset's setup), so
  // this runs on every view rather than only on open.
  private syncFromBindings(): void {
    this.traceFilterss = this.readTraceFilters();
    const parsed = parseSingleFieldOrderBy(this.readTraceOrderBy());
    this.traceListSortField = parsed?.field;
    this.traceListSortDirection = parsed?.direction;
  }

  // Binding-aware accessors (fall back to globals).

  private readTraceFilters(): readonly Filter[] {
    return this.bindings.getTraceFilters();
  }

  private writeTraceFilters(filters: readonly Filter[]): void {
    this.bindings.setTraceFilters(filters);
  }

  // null = unchosen (resolves to defaultVisible); [] = attach nothing.
  private readTraceMetadataColumns(): readonly string[] | null {
    return this.bindings.getTraceMetadataColumns();
  }

  private readTraceOrderBy(): string {
    return this.bindings.getTraceOrderBy();
  }

  private writeTraceOrderBy(orderBy: string): void {
    this.bindings.setTraceOrderBy(orderBy);
  }

  // `null` resets to unchosen; a concrete list (incl. []) is stored verbatim.
  private writeTraceMetadataColumns(cols: readonly string[] | null): void {
    this.bindings.setTraceMetadataColumns(cols);
  }

  // Effective settings for outgoing /trace_metadata[_schema] requests: the
  // tab's own snapshot, so the grid reflects the same trace source the next
  // Run uses.
  private effectiveSettings(): ReadonlyArray<SettingFilter> {
    return this.bindings.getEffectiveSettings();
  }

  // Wrap a globally-registered setting so its widget reads/writes this tab's
  // configuration rather than the registry's stored value.
  private boundSetting(
    setting: BigTraceSetting<unknown>,
  ): BigTraceSetting<unknown> {
    return new TabBoundSetting(setting, this.bindings);
  }

  private static readonly CATEGORY_DISPLAY_NAMES: ReadonlyMap<string, string> =
    new Map([
      ['General', 'General'],
      ['TRACE_ADDRESS', 'Trace Address'],
      ['TRACE_METADATA', 'Trace Metadata'],
      ['BIGTRACE_QUERY_OPTIONS', 'Query Options'],
    ]);

  private displayCategory(raw: string): string {
    return QuerySettingsForm.CATEGORY_DISPLAY_NAMES.get(raw) ?? raw;
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
      this.traceListDataSource = new BigtraceTraceListDataSource(
        client,
        () => this.effectiveSettings(),
        undefined,
        undefined,
        () => toExperimentFilterSpec(this.bindings.getExperimentFilter()),
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

  // One write path for both affordances (the Shown-columns popup and the
  // grid's header menus). The shown columns ARE the trace_metadata_columns
  // attached to every result row — one picker, one meaning. Emptying the set
  // falls back to the backend's defaults: the grid needs at least one column,
  // and "attach nothing" isn't a state this page offers.
  private writeChosenColumns(names: readonly string[]): void {
    this.writeTraceMetadataColumns(names.length === 0 ? null : [...names]);
    m.redraw();
  }

  // Renders the trace-grid card: the column-picker row, then a DataGrid
  // driven by the trace-list DataSource. The picker row and the grid's header
  // menus both write through the tab's traceMetadataColumns, so what the grid
  // shows is exactly what a run attaches to every result row.
  private renderTraceListCard(endpoint: string): m.Children {
    const ds = this.getTraceListDataSource(endpoint);
    if (ds === undefined) {
      return m(
        Card,
        {className: 'pf-settings-card'},
        m(
          '.pf-settings-card__details',
          m(
            '.pf-settings-card__description',
            'Set the BigTrace endpoint (connection button, top right) to ' +
              'load traces.',
          ),
        ),
      );
    }
    this.ensureSchemaFetched(endpoint);
    const schema = this.resolvedSchema();
    const schemaState = this.schemaState;

    if (schemaState === 'loading' || schemaState === undefined) {
      return m(
        Card,
        {className: 'pf-settings-card', style: {display: 'block'}},
        m(EmptyState, {title: 'Loading schema…', icon: 'hourglass_empty'}),
      );
    }
    if (schemaState !== undefined && 'kind' in schemaState) {
      return m(
        Card,
        {className: 'pf-settings-card', style: {display: 'block'}},
        m(
          Callout,
          {
            intent: Intent.Danger,
            icon: 'error',
            title: 'Failed to load trace schema',
          },
          schemaState.message,
        ),
      );
    }

    // Schema resolved: build the column list from the effective selection.
    const chosen = effectiveQueryColumns(
      this.readTraceMetadataColumns(),
      schema!.columns,
    );
    const datagridSchema = columnSchema(schema!.columns);

    return m(
      Card,
      {
        className: 'pf-settings-card pf-bt-trace-card',
        // padding-bottom keeps the grid clear of the card border.
        style: {
          display: 'block',
          paddingBottom: '16px',
        },
      },
      [
        this.renderColumnPicker(schema!.columns, chosen),
        m(
          '.pf-bt-trace-list-grid',
          {
            // Fixed height bounds the inner virtualized Grid's viewport:
            // without it the DataGrid's `height: 100%` resolves against an
            // auto-height parent and renders every row (catastrophic for a
            // large trace directory). This scrolling card must set its own
            // height; 500px engages virtualization while staying generous.
            // The small top margin keeps the picker row and the grid toolbar
            // reading as one block.
            style: {height: '500px', marginTop: '8px'},
          },
          m(DataGrid, {
            schema: datagridSchema,
            data: ds,
            // Inner virtualized Grid uses the wrapper's 500px as its viewport.
            fillHeight: true,
            // Controlled-mode columns: render exactly what the user picked,
            // in their order. The grid's header menus ("Add"/"Remove column")
            // emit onColumnsChanged — the same write path as the picker row
            // above, onto the tab's traceMetadataColumns.
            columns: this.buildTraceListColumns(chosen),
            onColumnsChanged: (cols: ReadonlyArray<Column>) => {
              // Extract sort (it lives on the Column object) before collapsing
              // cols to string[] so the next render can splice it back, else
              // the header click reverts each redraw. Stored on the tab; a Run
              // ships it as `trace_order_by` on /execute_*.
              const sorted = cols.find((c) => c.sort);
              this.traceListSortField = sorted?.field;
              this.traceListSortDirection = sorted?.sort;
              this.writeTraceOrderBy(formatSingleFieldOrderBy(sorted));
              this.writeChosenColumns(cols.map((c) => c.field));
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
            // Refresh (re-fetch /trace_metadata with the current filter and
            // settings), then how many traces that filter selects — backed by
            // the data source's filteredTotalRows.
            toolbarItemsLeft: [
              m(Button, {
                icon: 'refresh',
                title:
                  'Refresh trace list — re-fetch the traces with the ' +
                  'current filter and settings.',
                onclick: () => {
                  void ds.refresh();
                },
              }),
              this.renderTraceMatchCount(ds),
            ],
          }),
        ),
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

  // Popup multi-select for the trace columns: one checkable option per
  // column, each /trace_metadata_schema `description` as its hover tooltip.
  // The columns shown in the grid are the trace_metadata_columns attached to
  // every query result row — one picker for both.
  private renderColumnPicker(
    schemaCols: ReadonlyArray<TraceColumnDescriptor>,
    chosen: ReadonlyArray<string>,
  ): m.Children {
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
      '.pf-bt-trace-columns',
      m(PopupMultiSelect, {
        label: 'Query result columns',
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
        "Show and attach the backend's default columns, and keep tracking " +
          'that default as it changes.',
        () => this.writeTraceMetadataColumns(null),
      ),
      // Which traces the grid lists, and which a run processes.
      m(ExperimentFilterControl, {bindings: this.bindings}),
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
    this.writeChosenColumns(next);
  }

  view({attrs}: m.Vnode<QuerySettingsFormAttrs>) {
    // Refresh bindings each render so callers can swap them without remounting.
    this.bindings = attrs.bindings;
    this.syncFromBindings();

    const sections =
      attrs.scope === 'trace-selection'
        ? this.renderTraceSelectionSections()
        : this.renderQueryOptionSections();

    return m('.pf-bt-settings-embedded', [
      m('.pf-bt-settings-page', [
        attrs.header,
        bigTraceSettingsStorage.isExecConfigLoading &&
          m(EmptyState, {
            title: 'Loading settings...',
            icon: 'hourglass_empty',
            fillHeight: true,
          }),
        sections,
        bigTraceSettingsStorage.execConfigLoadError !== undefined &&
          m(
            Callout,
            {
              intent: Intent.Danger,
              icon: 'error',
              title: 'Failed to load settings from the backend',
            },
            bigTraceSettingsStorage.execConfigLoadError,
          ),
      ]),
    ]);
  }

  // Which traces the query runs over, in one of two mutually exclusive
  // modes. Filter mode: the grid leads — it IS the selection, and its shown
  // columns are the metadata attached to results — with the source settings
  // that feed it below. UUID mode (trace_uuids enabled): the pasted list IS
  // the corpus, so nothing else is shown — the filter-mode configuration is
  // hidden, not cleared, and returns intact when the mode is left.
  private renderTraceSelectionSections(): m.Children {
    if (this.traceUuidsActive()) {
      return m(
        '.pf-bt-settings-page__plugin-section',
        m(CardStack, [this.renderTraceUuidsCard()]),
      );
    }
    const traceSettings = bigTraceSettingsStorage.getAllSettings().filter(
      (setting) =>
        isTraceSelectionSetting(setting) &&
        // The UUID list is a selection MODE with its own card and entry
        // point, never a generic settings card.
        setting.id !== TRACE_UUIDS_SETTING_ID,
    );
    return m(
      '.pf-bt-settings-page__plugin-section',
      m(CardStack, [
        this.renderTraceListCard(getBigtraceEndpoint()),
        ...traceSettings.map((setting) =>
          this.renderBigTraceSettingCard(setting),
        ),
      ]),
    );
  }

  private traceUuidsActive(): boolean {
    return traceUuidsState(
      traceUuidsDeclared(),
      this.bindings.isSettingDisabled(TRACE_UUIDS_SETTING_ID),
      this.bindings.getSettingValue(TRACE_UUIDS_SETTING_ID) !== undefined,
    );
  }

  // The UUID-mode card: a paste box and a count. The draft text is kept
  // locally so typing isn't reformatted mid-edit; the parsed list is written
  // through on every input, and an external change to the tab's value (a
  // preset, Cancel's restore) resets the draft.
  private renderTraceUuidsCard(): m.Children {
    const setting = bigTraceSettingsStorage.get(TRACE_UUIDS_SETTING_ID);
    if (setting === undefined) return null;
    const stored =
      this.bindings.getSettingValue(TRACE_UUIDS_SETTING_ID) ??
      (setting.defaultValue as readonly string[] | undefined) ??
      [];
    const canonical = stored.join('\n');
    if (this.uuidDraftBase !== canonical) {
      this.uuidDraft = canonical;
      this.uuidDraftBase = canonical;
    }
    const count = parseTraceUuids(this.uuidDraft ?? '').length;
    return m(
      Card,
      {
        className: 'pf-settings-card pf-bt-uuid-settings-card',
        style: {display: 'flex'},
      },
      m('.pf-bt-uuid-card', [
        m('.pf-bt-uuid-card__head', [
          m('.pf-bt-uuid-card__title-row', [
            m('.pf-settings-card__title', 'Trace UUIDs'),
            m(Icon, {
              icon: 'info',
              className: 'pf-bt-uuid-card__info',
              title: 'Separate UUIDs with commas, spaces or new lines.',
            }),
          ]),
          // The way out of the mode. "Back" is honest here: the switch
          // destroys nothing in either direction — the pasted list and the
          // whole filter setup are kept — and like everything on this page
          // it is provisional until Apply.
          m(Button, {
            label: 'Back to filtering',
            icon: 'arrow_back',
            title:
              'Select the corpus with the trace grid and the source ' +
              'settings instead. The pasted list is kept and can be ' +
              'returned to.',
            onclick: () => {
              this.bindings.setSettingDisabled(TRACE_UUIDS_SETTING_ID, true);
              m.redraw();
            },
          }),
        ]),
        m('textarea.pf-bt-uuid-card__input', {
          placeholder: 'Paste trace UUIDs…',
          value: this.uuidDraft ?? '',
          spellcheck: false,
          oninput: (e: Event) => {
            const text = (e.target as HTMLTextAreaElement).value;
            this.uuidDraft = text;
            const parsed = parseTraceUuids(text);
            this.uuidDraftBase = parsed.join('\n');
            this.bindings.setSettingValue(
              TRACE_UUIDS_SETTING_ID,
              parsed,
              setting.category ?? 'TRACE_ADDRESS',
            );
          },
        }),
        m('.pf-bt-uuid-card__count', count === 1 ? '1 UUID' : `${count} UUIDs`),
      ]),
    );
  }

  // How the query runs: the trace cap first — a top-level request field like
  // the row limit, not a backend setting — then every remaining setting
  // grouped as declared.
  private renderQueryOptionSections(): m.Children {
    const categories = new Map<string, BigTraceSetting<unknown>[]>();
    for (const setting of bigTraceSettingsStorage.getAllSettings()) {
      if (isTraceSelectionSetting(setting)) continue;
      const categoryName = this.displayCategory(setting.category || 'General');
      if (!categories.has(categoryName)) {
        categories.set(categoryName, []);
      }
      categories.get(categoryName)!.push(setting);
    }
    return [
      m(
        '.pf-bt-settings-page__plugin-section',
        m(CardStack, [
          m(BigTraceSettingsCard, {
            title: 'Trace limit',
            description:
              'Maximum number of traces this query fans out to, applied ' +
              'after the trace selection.',
            controls: m(TextInput, {
              type: 'number',
              value: String(this.bindings.getTraceLimit()),
              onInput: (value: string) => {
                const n = parseInt(value, 10);
                if (!isNaN(n) && n > 0) this.bindings.setTraceLimit(n);
              },
            }),
          }),
        ]),
      ),
      Array.from(categories.entries()).map(([category, catSettings]) =>
        m(
          '.pf-bt-settings-page__plugin-section',
          m('h2.pf-bt-settings-page__plugin-title', category),
          m(
            CardStack,
            catSettings.map((setting) =>
              this.renderBigTraceSettingCard(setting),
            ),
          ),
        ),
      ),
    ];
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

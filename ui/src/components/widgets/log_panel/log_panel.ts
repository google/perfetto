// Copyright (C) 2024 The Android Open Source Project
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
import {time, Time, TimeSpan} from '../../../base/time';
import {DetailsShell} from '../../../widgets/details_shell';
import {Timestamp} from '../timestamp';
import {Engine} from '../../../trace_processor/engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  Row,
  STR,
} from '../../../trace_processor/query_result';
import {
  escapeQuery,
  escapeSearchQuery,
} from '../../../trace_processor/query_utils';
import {Select} from '../../../widgets/select';
import {Button} from '../../../widgets/button';
import {TextInput} from '../../../widgets/text_input';
import {
  Grid,
  GridColumn,
  GridRow,
  GridHeaderCell,
  GridCell,
} from '../../../widgets/grid';
import {classNames} from '../../../base/classnames';
import {TagInput} from '../../../widgets/tag_input';
import {Store} from '../../../base/store';
import {Trace} from '../../../public/trace';
import {Icons} from '../../../base/semantic_icons';
import {SerialTaskQueue, QuerySlot} from '../../../base/query_slot';
import {Anchor} from '../../../widgets/anchor';
import {getThreadUriPrefix} from '../../../public/utils';

export const ROW_H = 24;

// ---------------------------------------------------------------------------
// Shared filter state (base — each plugin may extend this)
// ---------------------------------------------------------------------------

export interface BaseLogFilteringCriteria {
  readonly minimumLevel: number;
  readonly tags: string[];
  readonly isTagRegex?: boolean;
  readonly textEntry: string;
  readonly hideNonMatching: boolean;
}

// ---------------------------------------------------------------------------
// Shared log entries produced by updateLogEntries
// ---------------------------------------------------------------------------

export interface SharedLogEntries {
  readonly offset: number;
  readonly timestamps: time[];
  readonly pids: bigint[];
  readonly tids: bigint[];
  readonly priorities: number[];
  readonly tags: string[];
  readonly messages: string[];
  readonly isHighlighted: boolean[];
  readonly processName: string[];
  /** Resolved utid/upid for each row (null when not resolvable). */
  readonly utids: (number | null)[];
  readonly upids: (number | null)[];
  readonly totalEvents: number;
  /** Plugin-specific extra arrays keyed by column name. */
  readonly extra: Record<string, unknown[]>;
}

// ---------------------------------------------------------------------------
// Config that each plugin provides to parameterise the shared panel
// ---------------------------------------------------------------------------

export interface LogPanelConfig {
  /** Displayed in the DetailsShell title bar. */
  readonly title: string;
  /** Priority label array; index = numeric priority value. */
  readonly priorities: string[];
  /**
   * How many leading entries in priorities[] to skip in the selector UI.
   * Android = 2 (skips '-','-'), Journald = 0.
   */
  readonly ignoredPriorityStates: number;
  /**
   * Name of the perfetto table created by buildSelectedRows, e.g.
   * 'filtered_logs' or 'filtered_journald_logs'.
   */
  readonly filteredTableName: string;
  /**
   * Builds the SQL SELECT expression used inside
   *   `create perfetto table <filteredTableName> as select * from (<here>) ...`
   * The returned string should already include the globMatch columns.
   */
  readonly buildSelectedRows: (
    filter: BaseLogFilteringCriteria,
    globMatch: string,
  ) => string;
  /**
   * SQL columns to fetch in addition to the common set.
   * Each entry is `<sql expression> as <alias>`.
   */
  readonly extraSelectColumns?: string[];
  /**
   * Schema entries for the extra columns (same keys as aliases above).
   * Used with rowsResult.iter().
   */
  readonly extraSchema?: Row;
  /**
   * Called once per row to collect extra data into arrays stored in
   * SharedLogEntries.extra[alias].
   */
  readonly collectExtraRow?: (
    it: Row,
    extra: Record<string, unknown[]>,
  ) => void;
  /**
   * Renders additional GridColumn definitions (header cells).
   * Called once when the entries are available.
   */
  readonly extraColumns?: (entries: SharedLogEntries) => GridColumn[];
  /**
   * Renders extra GridCells for row i.  The returned cells are inserted
   * after the Tag cell and before the Message cell.
   */
  readonly renderExtraCells?: (
    entries: SharedLogEntries,
    i: number,
    className: string,
    trace: Trace,
  ) => m.Children[];
  /** CSS class prefix for grid and priority rows, e.g. 'pf-logs-panel'. */
  readonly classPrefix: string;
  /** Maps numeric priority → CSS suffix string used for row colouring. */
  readonly classForPriority: (prio: number) => string | undefined;
  /**
   * Renders extra filter widgets appended after the standard ones.
   * Return null/undefined if not needed.
   */
  readonly renderExtraFilters?: (
    store: Store<BaseLogFilteringCriteria>,
  ) => m.Children;
}

// ---------------------------------------------------------------------------
// Public attrs for the LogPanel component
// ---------------------------------------------------------------------------

export interface LogPanelAttrs {
  readonly config: LogPanelConfig;
  readonly filterStore: Store<BaseLogFilteringCriteria>;
  readonly trace: Trace;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

interface Pagination {
  readonly offset: number;
  readonly count: number;
}

// ---------------------------------------------------------------------------
// Priority selector widget
// ---------------------------------------------------------------------------

interface LogPriorityWidgetAttrs {
  readonly options: string[];
  readonly ignoredStates: number;
  readonly selectedIndex: number;
  readonly onSelect: (id: number) => void;
}

class LogPriorityWidget implements m.ClassComponent<LogPriorityWidgetAttrs> {
  view(vnode: m.Vnode<LogPriorityWidgetAttrs>) {
    const {options, ignoredStates, selectedIndex, onSelect} = vnode.attrs;
    const optionComponents = [];
    for (let i = ignoredStates; i < options.length; i++) {
      optionComponents.push(
        m('option', {value: i, selected: i === selectedIndex}, options[i]),
      );
    }
    return m(
      Select,
      {
        onchange: (e: Event) => {
          onSelect(Number((e.target as HTMLSelectElement).value));
        },
      },
      optionComponents,
    );
  }
}

// ---------------------------------------------------------------------------
// Text search widget
// ---------------------------------------------------------------------------

interface LogTextWidgetAttrs {
  readonly onChange: (value: string) => void;
}

class LogTextWidget implements m.ClassComponent<LogTextWidgetAttrs> {
  view({attrs}: m.CVnode<LogTextWidgetAttrs>) {
    return m(TextInput, {
      placeholder: 'Search logs...',
      onkeyup: (e: KeyboardEvent) => {
        attrs.onChange((e.target as HTMLInputElement).value);
      },
    });
  }
}

// ---------------------------------------------------------------------------
// Filter-by-text toggle
// ---------------------------------------------------------------------------

interface FilterByTextWidgetAttrs {
  readonly hideNonMatching: boolean;
  readonly disabled: boolean;
  readonly onClick: () => void;
}

class FilterByTextWidget implements m.ClassComponent<FilterByTextWidgetAttrs> {
  view({attrs}: m.Vnode<FilterByTextWidgetAttrs>) {
    const icon = attrs.hideNonMatching ? Icons.Filter : Icons.FilterOff;
    const tooltip = attrs.hideNonMatching
      ? 'Show all logs and highlight matches'
      : 'Show only matching logs';
    return m(Button, {
      icon,
      title: tooltip,
      disabled: attrs.disabled,
      onclick: attrs.onClick,
    });
  }
}

// ---------------------------------------------------------------------------
// Shared filter bar
// ---------------------------------------------------------------------------

interface LogsFiltersAttrs {
  readonly config: LogPanelConfig;
  readonly store: Store<BaseLogFilteringCriteria>;
  readonly trace: Trace;
}

class LogsFilters implements m.ClassComponent<LogsFiltersAttrs> {
  view({attrs}: m.CVnode<LogsFiltersAttrs>) {
    const {config, store} = attrs;
    return [
      m('span', 'Log Level'),
      m(LogPriorityWidget, {
        options: config.priorities,
        ignoredStates: config.ignoredPriorityStates,
        selectedIndex: store.state.minimumLevel,
        onSelect: (minimumLevel) => {
          store.edit((draft) => {
            draft.minimumLevel = minimumLevel;
          });
        },
      }),
      m(TagInput, {
        placeholder: 'Filter by tag...',
        tags: store.state.tags,
        onTagAdd: (tag) => {
          store.edit((draft) => {
            draft.tags.push(tag);
          });
        },
        onTagRemove: (index) => {
          store.edit((draft) => {
            draft.tags.splice(index, 1);
          });
        },
      }),
      m(Button, {
        icon: 'regular_expression',
        title: 'Use regular expression',
        active: !!store.state.isTagRegex,
        onclick: () => {
          store.edit((draft) => {
            draft.isTagRegex = !draft.isTagRegex;
          });
        },
      }),
      m(LogTextWidget, {
        onChange: (text) => {
          store.edit((draft) => {
            draft.textEntry = text;
          });
        },
      }),
      m(FilterByTextWidget, {
        hideNonMatching: store.state.hideNonMatching,
        onClick: () => {
          store.edit((draft) => {
            draft.hideNonMatching = !draft.hideNonMatching;
          });
        },
        disabled: store.state.textEntry === '',
      }),
      config.renderExtraFilters?.(store),
    ];
  }
}

// ---------------------------------------------------------------------------
// Shared LogPanel component
// ---------------------------------------------------------------------------

export class LogPanel implements m.ClassComponent<LogPanelAttrs> {
  private readonly executor = new SerialTaskQueue();
  private readonly viewQuery = new QuerySlot<AsyncDisposable>(this.executor);
  private readonly entriesQuery = new QuerySlot<SharedLogEntries>(
    this.executor,
  );
  private pagination: Pagination = {offset: 0, count: 0};

  onremove() {
    this.viewQuery.dispose();
    this.entriesQuery.dispose();
  }

  view({attrs}: m.CVnode<LogPanelAttrs>) {
    const {config, filterStore, trace} = attrs;
    const visibleSpan = trace.timeline.visibleWindow.toTimeSpan();
    const filters = filterStore.state;
    const pagination = this.pagination;
    const engine = trace.engine;

    const viewResult = this.viewQuery.use({
      key: {filters},
      queryFn: () => updateLogView(engine, filters, config),
    });

    const entriesResult = this.entriesQuery.use({
      key: {
        filters,
        viewport: {start: visibleSpan.start, end: visibleSpan.end},
        pagination,
      },
      retainOn: ['pagination', 'viewport'],
      queryFn: () => updateLogEntries(engine, visibleSpan, pagination, config),
      enabled: !!viewResult.data,
    });

    const entries = entriesResult.data;
    const totalEvents = entries?.totalEvents ?? 0;

    return m(
      DetailsShell,
      {
        title: config.title,
        description: `Total messages: ${totalEvents}`,
        buttons: m(LogsFilters, {config, store: filterStore, trace}),
      },
      this.renderGrid(trace, config, entries),
    );
  }

  private renderGrid(
    trace: Trace,
    config: LogPanelConfig,
    entries: SharedLogEntries | undefined,
  ) {
    if (!entries) return null;

    const hasProcessNames = entries.processName.some((name) => name);
    const extraCols = config.extraColumns?.(entries) ?? [];

    const columns: GridColumn[] = [
      {key: 'timestamp', header: m(GridHeaderCell, 'Timestamp')},
      {key: 'pid', header: m(GridHeaderCell, 'PID')},
      {key: 'tid', header: m(GridHeaderCell, 'TID')},
      {key: 'level', header: m(GridHeaderCell, 'Level')},
      ...(hasProcessNames
        ? [{key: 'process', header: m(GridHeaderCell, 'Process')}]
        : []),
      {key: 'tag', header: m(GridHeaderCell, 'Tag')},
      ...extraCols,
      {
        key: 'message',
        maxInitialWidthPx: Infinity,
        header: m(GridHeaderCell, 'Message'),
      },
    ];

    return m(Grid, {
      className: config.classPrefix,
      columns,
      rowData: {
        data: this.renderRows(trace, config, entries, hasProcessNames),
        total: entries.totalEvents,
        offset: entries.offset,
        onLoadData: (offset, count) => {
          this.pagination = {offset, count};
          m.redraw();
        },
      },
      virtualization: {rowHeightPx: ROW_H},
      fillHeight: true,
      onRowHover: (rowIndex) => {
        const actualIndex = rowIndex - entries.offset;
        const timestamp = entries.timestamps[actualIndex];
        if (timestamp !== undefined) {
          trace.timeline.hoverCursorTimestamp = timestamp;
        }
      },
      onRowOut: () => {
        trace.timeline.hoverCursorTimestamp = undefined;
      },
    });
  }

  private renderRows(
    trace: Trace,
    config: LogPanelConfig,
    entries: SharedLogEntries,
    hasProcessNames: boolean,
  ): ReadonlyArray<GridRow> {
    const rows: GridRow[] = [];
    for (let i = 0; i < entries.timestamps.length; i++) {
      const priority = entries.priorities[i];
      const priorityLetter = config.priorities[priority]?.[0] ?? '?';
      const ts = entries.timestamps[i];
      const prioritySuffix = config.classForPriority(priority);
      const priorityClass = prioritySuffix
        ? `${config.classPrefix}__row--${prioritySuffix}`
        : undefined;
      const isHighlighted = entries.isHighlighted[i];
      const className = classNames(
        priorityClass,
        isHighlighted && `${config.classPrefix}__row--highlighted`,
      );

      const extraCells =
        config.renderExtraCells?.(entries, i, className ?? '', trace) ?? [];

      const row = [
        m(GridCell, {className}, m(Timestamp, {trace, ts})),
        m(
          GridCell,
          {className, align: 'right'},
          entries.upids[i] !== null
            ? m(
                Anchor,
                {
                  onclick: () =>
                    trace.scrollTo({
                      track: {
                        uri: `/process_${entries.upids[i]}`,
                        expandGroup: true,
                      },
                    }),
                },
                String(entries.pids[i]),
              )
            : String(entries.pids[i]),
        ),
        m(
          GridCell,
          {className, align: 'right'},
          entries.utids[i] !== null
            ? m(
                Anchor,
                {
                  onclick: () => {
                    const uri = `${getThreadUriPrefix(entries.upids[i], entries.utids[i]!)}_state`;
                    trace.scrollTo({track: {uri, expandGroup: true}});
                  },
                },
                String(entries.tids[i]),
              )
            : String(entries.tids[i]),
        ),
        m(GridCell, {className}, priorityLetter),
        hasProcessNames && m(GridCell, {className}, entries.processName[i]),
        m(GridCell, {className}, entries.tags[i]),
        ...extraCells,
        m(GridCell, {className}, entries.messages[i]),
      ].filter(Boolean);

      rows.push(row);
    }
    return rows;
  }
}

// ---------------------------------------------------------------------------
// Shared SQL helpers
// ---------------------------------------------------------------------------

export function serializeTags(tags: string[]): string {
  return tags.map((tag) => escapeQuery(tag)).join();
}

export function composeGlobMatch(
  isCollapsed: boolean,
  textEntry: string,
): string {
  if (isCollapsed) {
    return `msg glob ${escapeSearchQuery(textEntry)} as is_msg_chosen,
      (process.name is not null and process.name glob ${escapeSearchQuery(
        textEntry,
      )}) as is_process_chosen,
      0 as is_msg_highlighted,
      0 as is_process_highlighted`;
  } else if (!textEntry) {
    return `1 as is_msg_chosen,
      1 as is_process_chosen,
      0 as is_msg_highlighted,
      0 as is_process_highlighted`;
  } else {
    return `1 as is_msg_chosen,
      1 as is_process_chosen,
      msg glob ${escapeSearchQuery(textEntry)} as is_msg_highlighted,
      (process.name is not null and process.name glob ${escapeSearchQuery(
        textEntry,
      )}) as is_process_highlighted`;
  }
}

// ---------------------------------------------------------------------------
// Shared query functions
// ---------------------------------------------------------------------------

async function updateLogView(
  engine: Engine,
  filter: BaseLogFilteringCriteria,
  config: LogPanelConfig,
): Promise<AsyncDisposable> {
  const globMatch = composeGlobMatch(filter.hideNonMatching, filter.textEntry);
  const selectedRows = config.buildSelectedRows(filter, globMatch);
  const tableName = config.filteredTableName;

  await engine.query(`create perfetto table ${tableName} as select *
    from (${selectedRows})
    where is_msg_chosen is 1 or is_process_chosen is 1`);

  return {
    async [Symbol.asyncDispose]() {
      await engine.query(`drop table ${tableName}`);
    },
  };
}

async function updateLogEntries(
  engine: Engine,
  span: TimeSpan,
  pagination: Pagination,
  config: LogPanelConfig,
): Promise<SharedLogEntries> {
  const tableName = config.filteredTableName;
  const extraSelect =
    config.extraSelectColumns && config.extraSelectColumns.length > 0
      ? ',\n          ' + config.extraSelectColumns.join(',\n          ')
      : '';

  const rowsResult = await engine.query(`
        select
          ts,
          pid,
          tid,
          prio,
          ifnull(tag, '[NULL]') as tag,
          ifnull(msg, '[NULL]') as msg,
          is_msg_highlighted as isMsgHighlighted,
          is_process_highlighted as isProcessHighlighted,
          ifnull(process_name, '') as processName,
          utid,
          upid${extraSelect}
        from ${tableName}
        where ts >= ${span.start} and ts <= ${span.end}
        order by ts
        limit ${pagination.offset}, ${pagination.count}
    `);

  const timestamps: time[] = [];
  const pids: bigint[] = [];
  const tids: bigint[] = [];
  const priorities: number[] = [];
  const tags: string[] = [];
  const messages: string[] = [];
  const isHighlighted: boolean[] = [];
  const processName: string[] = [];
  const utids: (number | null)[] = [];
  const upids: (number | null)[] = [];
  const extra: Record<string, unknown[]> = {};

  const schema: Row = {
    ts: LONG,
    pid: LONG_NULL,
    tid: LONG_NULL,
    prio: NUM,
    tag: STR,
    msg: STR,
    isMsgHighlighted: NUM_NULL,
    isProcessHighlighted: NUM,
    processName: STR,
    utid: NUM_NULL,
    upid: NUM_NULL,
    ...(config.extraSchema ?? {}),
  };

  const it = rowsResult.iter(schema);
  for (; it.valid(); it.next()) {
    const row = it as Row;
    timestamps.push(Time.fromRaw(row.ts as bigint));
    pids.push((row.pid as bigint | null) ?? 0n);
    tids.push((row.tid as bigint | null) ?? 0n);
    priorities.push(row.prio as number);
    tags.push(row.tag as string);
    messages.push(row.msg as string);
    isHighlighted.push(
      row.isMsgHighlighted === 1 || row.isProcessHighlighted === 1,
    );
    processName.push(row.processName as string);
    utids.push((row.utid as number | null) ?? null);
    upids.push((row.upid as number | null) ?? null);
    config.collectExtraRow?.(row, extra);
  }

  const queryRes = await engine.query(`
    select count(*) as totalEvents
    from ${tableName}
    where ts >= ${span.start} and ts <= ${span.end}
  `);
  const {totalEvents} = queryRes.firstRow({totalEvents: NUM});

  return {
    offset: pagination.offset,
    timestamps,
    pids,
    tids,
    priorities,
    tags,
    messages,
    isHighlighted,
    processName,
    utids,
    upids,
    totalEvents,
    extra,
  };
}

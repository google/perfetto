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
import {time, Time, TimeSpan} from '../../base/time';
import {DetailsShell} from '../../widgets/details_shell';
import {Timestamp} from '../../components/widgets/timestamp';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {
  escapeQuery,
  escapeSearchQuery,
  escapeRegexQuery,
} from '../../trace_processor/query_utils';
import {Select} from '../../widgets/select';
import {Button} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {
  Grid,
  GridColumn,
  GridRow,
  GridHeaderCell,
  GridCell,
} from '../../widgets/grid';
import {classNames} from '../../base/classnames';
import {TagInput} from '../../widgets/tag_input';
import {Store} from '../../base/store';
import {Trace} from '../../public/trace';
import {Icons} from '../../base/semantic_icons';
import {SerialTaskQueue, QuerySlot} from '../../base/query_slot';
import {Anchor} from '../../widgets/anchor';

const ROW_H = 24;

export interface JournaldLogFilteringCriteria {
  readonly minimumLevel: number;
  readonly tags: string[];
  readonly isTagRegex?: boolean;
  readonly textEntry: string;
  readonly hideNonMatching: boolean;
}

// JournaldLogPanelCache is intentionally left empty (no machine IDs needed),
// but kept for API consistency with the Android log panel.
export interface JournaldLogPanelCache {}

export interface JournaldLogPanelAttrs {
  readonly cache: JournaldLogPanelCache;
  readonly filterStore: Store<JournaldLogFilteringCriteria>;
  readonly trace: Trace;
}

interface Pagination {
  readonly offset: number;
  readonly count: number;
}

interface LogEntries {
  readonly offset: number;
  readonly timestamps: time[];
  readonly pids: bigint[];
  readonly tids: bigint[];
  readonly upids: Array<number | null>;
  readonly utids: Array<number | null>;
  readonly priorities: number[];
  readonly tags: string[];
  readonly messages: string[];
  readonly isHighlighted: boolean[];
  readonly processName: string[];
  readonly systemdUnits: string[];
  readonly totalEvents: number;
}

export class JournaldLogPanel
  implements m.ClassComponent<JournaldLogPanelAttrs>
{
  private readonly trace: Trace;
  private readonly executor = new SerialTaskQueue();
  private readonly viewQuery = new QuerySlot<AsyncDisposable>(this.executor);
  private readonly entriesQuery = new QuerySlot<LogEntries>(this.executor);
  private pagination: Pagination = {
    offset: 0,
    count: 0,
  };

  constructor({attrs}: m.CVnode<JournaldLogPanelAttrs>) {
    this.trace = attrs.trace;
  }

  onremove() {
    this.viewQuery.dispose();
    this.entriesQuery.dispose();
  }

  view({attrs}: m.CVnode<JournaldLogPanelAttrs>) {
    const visibleSpan = attrs.trace.timeline.visibleWindow.toTimeSpan();
    const filters = attrs.filterStore.state;
    const pagination = this.pagination;
    const engine = attrs.trace.engine;

    // Query 1: Create the filtered_journald_logs table
    const viewResult = this.viewQuery.use({
      key: {filters},
      queryFn: () => updateLogView(engine, filters),
    });

    // Query 2: Read from the table
    const entriesResult = this.entriesQuery.use({
      key: {
        filters,
        viewport: {start: visibleSpan.start, end: visibleSpan.end},
        pagination,
      },
      retainOn: ['pagination', 'viewport'],
      queryFn: () => updateLogEntries(engine, visibleSpan, pagination),
      enabled: !!viewResult.data,
    });

    const entries = entriesResult.data;
    const totalEvents = entries?.totalEvents ?? 0;

    return m(
      DetailsShell,
      {
        title: 'Journald Logs',
        description: `Total messages: ${totalEvents}`,
        buttons: m(JournaldLogsFilters, {
          trace: attrs.trace,
          store: attrs.filterStore,
        }),
      },
      this.renderGrid(attrs.trace, entries),
    );
  }

  private renderGrid(trace: Trace, entries: LogEntries | undefined) {
    if (entries) {
      const hasProcessNames =
        entries.processName.filter((name) => name).length > 0;
      const hasSystemdUnits =
        entries.systemdUnits.filter((unit) => unit).length > 0;

      const columns: GridColumn[] = [
        {key: 'timestamp', header: m(GridHeaderCell, 'Timestamp')},
        {key: 'pid', header: m(GridHeaderCell, 'PID')},
        {key: 'tid', header: m(GridHeaderCell, 'TID')},
        {key: 'level', header: m(GridHeaderCell, 'Level')},
        ...(hasProcessNames
          ? [{key: 'process', header: m(GridHeaderCell, 'Process')}]
          : []),
        {key: 'tag', header: m(GridHeaderCell, 'Tag')},
        ...(hasSystemdUnits
          ? [{key: 'unit', header: m(GridHeaderCell, 'Unit')}]
          : []),
        {
          key: 'message',
          maxInitialWidthPx: Infinity,
          header: m(GridHeaderCell, 'Message'),
        },
      ];

      return m(Grid, {
        className: 'pf-journald-logs-panel',
        columns,
        rowData: {
          data: this.renderRows(entries, hasProcessNames, hasSystemdUnits),
          total: entries?.totalEvents ?? 0,
          offset: entries?.offset ?? 0,
          onLoadData: (offset, count) => {
            this.pagination = {offset, count};
            m.redraw();
          },
        },
        virtualization: {
          rowHeightPx: ROW_H,
        },
        fillHeight: true,
        onRowHover: (rowIndex) => {
          const actualIndex = rowIndex - (entries?.offset ?? 0);
          const timestamp = entries?.timestamps[actualIndex];
          if (timestamp !== undefined) {
            trace.timeline.hoverCursorTimestamp = timestamp;
          }
        },
        onRowOut: () => {
          trace.timeline.hoverCursorTimestamp = undefined;
        },
      });
    } else {
      return null;
    }
  }

  private renderRows(
    entries: LogEntries,
    hasProcessNames: boolean | undefined,
    hasSystemdUnits: boolean | undefined,
  ): ReadonlyArray<GridRow> {
    const trace = this.trace;
    const timestamps = entries.timestamps;
    const pids = entries.pids;
    const tids = entries.tids;
    const upids = entries.upids;
    const utids = entries.utids;
    const priorities = entries.priorities;
    const tags = entries.tags;
    const messages = entries.messages;
    const processNames = entries.processName;
    const systemdUnits = entries.systemdUnits;

    const rows: GridRow[] = [];
    for (let i = 0; i < entries.timestamps.length; i++) {
      const priority = priorities[i];
      const priorityLetter = JOURNALD_PRIORITIES[priority]?.[0] ?? '?';
      const ts = timestamps[i];
      const priorityClass = `pf-journald-logs-panel__row--${classForPriority(priority)}`;
      const isHighlighted = entries.isHighlighted[i];
      const className = classNames(
        priorityClass,
        isHighlighted && 'pf-journald-logs-panel__row--highlighted',
      );

      const row = [
        m(GridCell, {className}, m(Timestamp, {trace, ts})),
        m(
          GridCell,
          {className, align: 'right'},
          renderIdCell(trace, String(pids[i]), 'upid', upids[i]),
        ),
        m(
          GridCell,
          {className, align: 'right'},
          renderIdCell(trace, String(tids[i]), 'utid', utids[i]),
        ),
        m(GridCell, {className}, priorityLetter),
        hasProcessNames && m(GridCell, {className}, processNames[i]),
        m(GridCell, {className}, tags[i]),
        hasSystemdUnits && m(GridCell, {className}, systemdUnits[i]),
        m(GridCell, {className}, messages[i]),
      ].filter(Boolean);

      rows.push(row);
    }

    return rows;
  }
}

function renderIdCell(
  trace: Trace,
  label: string,
  tagKey: 'upid' | 'utid',
  tagValue: number | null,
): m.Children {
  if (tagValue === null) {
    return label;
  }
  return m(
    Anchor,
    {
      onclick: () => {
        const track = trace.tracks.findTrack(
          (t) => t.tags?.[tagKey] === tagValue,
        );
        if (track) {
          trace.scrollTo({track: {uri: track.uri, expandGroup: true}});
        }
      },
    },
    label,
  );
}

function classForPriority(priority: number) {
  switch (priority) {
    case 0:
    case 1:
    case 2:
      return 'fatal'; // EMERG, ALERT, CRIT
    case 3:
      return 'error'; // ERR
    case 4:
      return 'warn'; // WARNING
    case 5:
    case 6:
      return 'info'; // NOTICE, INFO
    case 7:
      return 'debug'; // DEBUG
    default:
      return undefined;
  }
}

// Journald priorities: index = numeric syslog priority value.
export const JOURNALD_PRIORITIES = [
  'Emergency', // 0 — EMERG
  'Alert', // 1 — ALERT
  'Critical', // 2 — CRIT
  'Error', // 3 — ERR
  'Warning', // 4 — WARNING
  'Notice', // 5 — NOTICE
  'Info', // 6 — INFO
  'Debug', // 7 — DEBUG
];

interface JournaldLogPriorityWidgetAttrs {
  readonly trace: Trace;
  readonly options: string[];
  readonly selectedIndex: number;
  readonly onSelect: (id: number) => void;
}

class JournaldLogPriorityWidget
  implements m.ClassComponent<JournaldLogPriorityWidgetAttrs>
{
  view(vnode: m.Vnode<JournaldLogPriorityWidgetAttrs>) {
    const attrs = vnode.attrs;
    const optionComponents = [];
    for (let i = 0; i < attrs.options.length; i++) {
      const selected = i === attrs.selectedIndex;
      optionComponents.push(
        m('option', {value: i, selected}, attrs.options[i]),
      );
    }
    return m(
      Select,
      {
        onchange: (e: Event) => {
          const selectionValue = (e.target as HTMLSelectElement).value;
          attrs.onSelect(Number(selectionValue));
        },
      },
      optionComponents,
    );
  }
}

interface LogTextWidgetAttrs {
  readonly trace: Trace;
  readonly onChange: (value: string) => void;
}

class LogTextWidget implements m.ClassComponent<LogTextWidgetAttrs> {
  view({attrs}: m.CVnode<LogTextWidgetAttrs>) {
    return m(TextInput, {
      placeholder: 'Search logs...',
      onkeyup: (e: KeyboardEvent) => {
        const htmlElement = e.target as HTMLInputElement;
        attrs.onChange(htmlElement.value);
      },
    });
  }
}

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

interface JournaldLogsFiltersAttrs {
  readonly trace: Trace;
  readonly store: Store<JournaldLogFilteringCriteria>;
}

export class JournaldLogsFilters
  implements m.ClassComponent<JournaldLogsFiltersAttrs>
{
  view({attrs}: m.CVnode<JournaldLogsFiltersAttrs>) {
    return [
      m('span', 'Log Level'),
      m(JournaldLogPriorityWidget, {
        trace: attrs.trace,
        options: JOURNALD_PRIORITIES,
        selectedIndex: attrs.store.state.minimumLevel,
        onSelect: (minimumLevel) => {
          attrs.store.edit((draft) => {
            draft.minimumLevel = minimumLevel;
          });
        },
      }),
      m(TagInput, {
        placeholder: 'Filter by tag...',
        tags: attrs.store.state.tags,
        onTagAdd: (tag) => {
          attrs.store.edit((draft) => {
            draft.tags.push(tag);
          });
        },
        onTagRemove: (index) => {
          attrs.store.edit((draft) => {
            draft.tags.splice(index, 1);
          });
        },
      }),
      m(Button, {
        icon: 'regular_expression',
        title: 'Use regular expression',
        active: !!attrs.store.state.isTagRegex,
        onclick: () => {
          attrs.store.edit((draft) => {
            draft.isTagRegex = !draft.isTagRegex;
          });
        },
      }),
      m(LogTextWidget, {
        trace: attrs.trace,
        onChange: (text) => {
          attrs.store.edit((draft) => {
            draft.textEntry = text;
          });
        },
      }),
      m(FilterByTextWidget, {
        hideNonMatching: attrs.store.state.hideNonMatching,
        onClick: () => {
          attrs.store.edit((draft) => {
            draft.hideNonMatching = !draft.hideNonMatching;
          });
        },
        disabled: attrs.store.state.textEntry === '',
      }),
    ];
  }
}

async function updateLogEntries(
  engine: Engine,
  span: TimeSpan,
  pagination: Pagination,
): Promise<LogEntries> {
  const rowsResult = await engine.query(`
        select
          ts,
          pid,
          tid,
          upid,
          utid,
          prio,
          ifnull(tag, '[NULL]') as tag,
          ifnull(msg, '[NULL]') as msg,
          is_msg_highlighted as isMsgHighlighted,
          is_process_highlighted as isProcessHighlighted,
          ifnull(process_name, '') as processName,
          ifnull(systemd_unit, '') as systemdUnit
        from filtered_journald_logs
        where ts >= ${span.start} and ts <= ${span.end}
        order by ts
        limit ${pagination.offset}, ${pagination.count}
    `);

  const timestamps: time[] = [];
  const pids = [];
  const tids = [];
  const upids: Array<number | null> = [];
  const utids: Array<number | null> = [];
  const priorities = [];
  const tags = [];
  const messages = [];
  const isHighlighted = [];
  const processName = [];
  const systemdUnits = [];

  const it = rowsResult.iter({
    ts: LONG,
    pid: LONG,
    tid: LONG,
    upid: NUM_NULL,
    utid: NUM_NULL,
    prio: NUM,
    tag: STR,
    msg: STR,
    isMsgHighlighted: NUM_NULL,
    isProcessHighlighted: NUM,
    processName: STR,
    systemdUnit: STR,
  });
  for (; it.valid(); it.next()) {
    timestamps.push(Time.fromRaw(it.ts));
    pids.push(it.pid);
    tids.push(it.tid);
    upids.push(it.upid);
    utids.push(it.utid);
    priorities.push(it.prio);
    tags.push(it.tag);
    messages.push(it.msg);
    isHighlighted.push(
      it.isMsgHighlighted === 1 || it.isProcessHighlighted === 1,
    );
    processName.push(it.processName);
    systemdUnits.push(it.systemdUnit);
  }

  const queryRes = await engine.query(`
    select
      count(*) as totalEvents
    from filtered_journald_logs
    where ts >= ${span.start} and ts <= ${span.end}
  `);
  const {totalEvents} = queryRes.firstRow({totalEvents: NUM});

  return {
    offset: pagination.offset,
    timestamps,
    pids,
    tids,
    upids,
    utids,
    priorities,
    tags,
    messages,
    isHighlighted,
    processName,
    systemdUnits,
    totalEvents,
  };
}

async function updateLogView(
  engine: Engine,
  filter: JournaldLogFilteringCriteria,
): Promise<AsyncDisposable> {
  const globMatch = composeGlobMatch(filter.hideNonMatching, filter.textEntry);
  let selectedRows = `select prio, ts, pid, tid,
      process.upid as upid, thread.utid as utid,
      tag, msg,
      process.name as process_name,
      ifnull(journald_logs.systemd_unit, '') as systemd_unit, ${globMatch}
      from journald_logs
      left join thread using(utid)
      left join process using(upid)
      where prio <= ${filter.minimumLevel}`;
  if (filter.tags.length) {
    if (filter.isTagRegex) {
      const tagGlobClauses = filter.tags.map(
        (pattern) => `tag glob ${escapeRegexQuery(pattern)}`,
      );
      selectedRows += ` and (${tagGlobClauses.join(' OR ')})`;
    } else {
      selectedRows += ` and tag in (${serializeTags(filter.tags)})`;
    }
  }

  // We extract only the rows which will be visible.
  await engine.query(`create perfetto table filtered_journald_logs as select *
    from (${selectedRows})
    where is_msg_chosen is 1 or is_process_chosen is 1`);

  return {
    async [Symbol.asyncDispose]() {
      await engine.query('drop table filtered_journald_logs');
    },
  };
}

function serializeTags(tags: string[]) {
  return tags.map((tag) => escapeQuery(tag)).join();
}

function composeGlobMatch(isCollapsed: boolean, textEntry: string) {
  if (isCollapsed) {
    // If the entries are collapsed, we won't highlight any lines.
    return `msg glob ${escapeSearchQuery(textEntry)} as is_msg_chosen,
      (process.name is not null and process.name glob ${escapeSearchQuery(
        textEntry,
      )}) as is_process_chosen,
      0 as is_msg_highlighted,
      0 as is_process_highlighted`;
  } else if (!textEntry) {
    // If there is no text entry, we will show all lines, but won't highlight any.
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

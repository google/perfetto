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

import './logs_panel.scss';
import m from 'mithril';
import {type time, Time, type TimeSpan} from '../../base/time';
import {DetailsShell} from '../../widgets/details_shell';
import {Timestamp} from '../../components/widgets/timestamp';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
} from '../../trace_processor/query_result';
import {
  escapeQuery,
  escapeRegexQuery,
  escapeSearchQuery,
} from '../../trace_processor/query_utils';
import {Select} from '../../widgets/select';
import {Button} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {
  Grid,
  type GridColumn,
  type GridRow,
  GridCell,
  GridHeaderCell,
} from '../../widgets/grid';
import {classNames} from '../../base/classnames';
import {TagInput} from '../../widgets/tag_input';
import type {Store} from '../../base/store';
import type {Trace} from '../../public/trace';
import {Icons} from '../../base/semantic_icons';
import {MenuItem} from '../../widgets/menu';
import {SerialTaskQueue, QuerySlot} from '../../base/query_slot';
import {Anchor} from '../../widgets/anchor';
import {getThreadUriPrefix} from '../../public/utils';

const ROW_H = 24;

export interface JournaldLogFilteringCriteria {
  readonly minimumLevel: number;
  readonly tags: string[];
  readonly isTagRegex?: boolean;
  readonly textEntry: string;
  readonly hideNonMatching: boolean;
}

export interface JournaldLogPanelAttrs {
  readonly filterStore: Store<JournaldLogFilteringCriteria>;
  readonly trace: Trace;
}

interface Pagination {
  readonly offset: number;
  readonly count: number;
}

interface LogEntries {
  readonly offset: number;
  readonly ids: number[];
  readonly timestamps: time[];
  readonly pids: (bigint | null)[];
  readonly tids: (bigint | null)[];
  readonly priorities: number[];
  readonly upids: (number | null)[];
  readonly utids: (number | null)[];
  readonly tags: string[];
  readonly units: string[];
  readonly messages: string[];
  readonly isHighlighted: boolean[];
  readonly processName: string[];
  readonly totalEvents: number;
}

export const JOURNALD_PRIORITIES = [
  'Emergency',
  'Alert',
  'Critical',
  'Error',
  'Warning',
  'Notice',
  'Info',
  'Debug',
];

export class JournaldLogPanel implements m.ClassComponent<JournaldLogPanelAttrs> {
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

    const viewResult = this.viewQuery.use({
      key: {filters},
      queryFn: () => updateLogView(engine, filters),
    });

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
        buttons: m(LogsFilters, {
          store: attrs.filterStore,
        }),
      },
      this.renderGrid(attrs.trace, entries),
    );
  }

  private renderGrid(trace: Trace, entries: LogEntries | undefined) {
    if (!entries) return null;

    const hasProcessNames = entries.processName.some((name) => name);
    const hasUnits = entries.units.some((u) => u);

    const columns: GridColumn[] = [
      {key: 'timestamp', header: m(GridHeaderCell, 'Timestamp')},
      {key: 'pid', header: m(GridHeaderCell, 'PID')},
      {key: 'tid', header: m(GridHeaderCell, 'TID')},
      {key: 'level', header: m(GridHeaderCell, 'Level')},
      ...(hasProcessNames
        ? [{key: 'process', header: m(GridHeaderCell, 'Process')}]
        : []),
      {key: 'tag', header: m(GridHeaderCell, 'Tag')},
      ...(hasUnits ? [{key: 'unit', header: m(GridHeaderCell, 'Unit')}] : []),
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
        data: this.renderRows(entries, hasProcessNames, hasUnits),
        total: entries.totalEvents,
        offset: entries.offset,
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
    entries: LogEntries,
    hasProcessNames: boolean,
    hasUnits: boolean,
  ): ReadonlyArray<GridRow> {
    const trace = this.trace;
    const rows: GridRow[] = [];
    for (let i = 0; i < entries.timestamps.length; i++) {
      const priority = entries.priorities[i];
      const priorityName = JOURNALD_PRIORITIES[priority] ?? '?';
      const ts = entries.timestamps[i];
      const eventId = entries.ids[i];
      const priorityClass = classForPriority(priority);
      const isHighlighted = entries.isHighlighted[i];
      const className = classNames(
        priorityClass && `pf-journald-logs-panel__row--${priorityClass}`,
        isHighlighted && 'pf-journald-logs-panel__row--highlighted',
      );

      const row = [
        m(
          GridCell,
          {
            className,
            menuItems: m(MenuItem, {
              label: 'Go to event on timeline',
              icon: Icons.UpdateSelection,
              onclick: () => {
                trace.selection.selectSqlEvent(
                  'linux_systemd_journald_logs',
                  eventId,
                  {
                    scrollToSelection: true,
                    switchToCurrentSelectionTab: true,
                  },
                );
              },
            }),
          },
          m(Timestamp, {trace, ts}),
        ),
        m(
          GridCell,
          {className, align: 'right'},
          entries.pids[i] !== null
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
            : '',
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
            : '',
        ),
        m(GridCell, {className}, priorityName),
        hasProcessNames && m(GridCell, {className}, entries.processName[i]),
        m(GridCell, {className}, entries.tags[i]),
        hasUnits && m(GridCell, {className}, entries.units[i]),
        m(GridCell, {className}, entries.messages[i]),
      ].filter(Boolean);

      rows.push(row);
    }

    return rows;
  }
}

function classForPriority(priority: number): string | undefined {
  switch (priority) {
    case 0:
      return 'emergency';
    case 1:
      return 'alert';
    case 2:
      return 'critical';
    case 3:
      return 'error';
    case 4:
      return 'warning';
    case 5:
      return 'notice';
    case 6:
      return 'info';
    case 7:
      return 'debug';
    default:
      return undefined;
  }
}

interface LogPriorityWidgetAttrs {
  readonly options: string[];
  readonly selectedIndex: number;
  readonly onSelect: (id: number) => void;
}

class LogPriorityWidget implements m.ClassComponent<LogPriorityWidgetAttrs> {
  view(vnode: m.Vnode<LogPriorityWidgetAttrs>) {
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
  readonly onChange: (value: string) => void;
}

class LogTextWidget implements m.ClassComponent<LogTextWidgetAttrs> {
  view({attrs}: m.CVnode<LogTextWidgetAttrs>) {
    return m(TextInput, {
      leftIcon: 'search',
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
      tooltip,
      onclick: attrs.onClick,
    });
  }
}

interface LogsFiltersAttrs {
  readonly store: Store<JournaldLogFilteringCriteria>;
}

class LogsFilters implements m.ClassComponent<LogsFiltersAttrs> {
  view({attrs}: m.CVnode<LogsFiltersAttrs>) {
    return [
      m('span', 'Log Level'),
      m(LogPriorityWidget, {
        options: JOURNALD_PRIORITIES,
        selectedIndex: attrs.store.state.minimumLevel,
        onSelect: (minimumLevel) => {
          attrs.store.edit((draft) => {
            draft.minimumLevel = minimumLevel;
          });
        },
      }),
      m(TagInput, {
        leftIcon: 'label',
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
        tooltip: 'Use regex',
        active: !!attrs.store.state.isTagRegex,
        onclick: () => {
          attrs.store.edit((draft) => {
            draft.isTagRegex = !draft.isTagRegex;
          });
        },
      }),
      m(LogTextWidget, {
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
          id,
          ts,
          pid,
          tid,
          prio,
          upid,
          utid,
          ifnull(tag, '[NULL]') as tag,
          ifnull(msg, '[NULL]') as msg,
          is_msg_highlighted as isMsgHighlighted,
          is_process_highlighted as isProcessHighlighted,
          ifnull(process_name, '') as processName,
          ifnull(systemd_unit, '') as systemdUnit
        from filtered_linux_systemd_journald_logs
        where ts >= ${span.start} and ts <= ${span.end}
        order by ts
        limit ${pagination.offset}, ${pagination.count}
    `);

  const ids: number[] = [];
  const timestamps: time[] = [];
  const pids = [];
  const tids = [];
  const priorities: number[] = [];
  const tags: string[] = [];
  const units: string[] = [];
  const messages: string[] = [];
  const isHighlighted: boolean[] = [];
  const processName: string[] = [];
  const upids = [];
  const utids = [];

  const it = rowsResult.iter({
    id: NUM,
    ts: LONG,
    pid: LONG_NULL,
    tid: LONG_NULL,
    prio: NUM,
    upid: NUM_NULL,
    utid: NUM_NULL,
    tag: STR,
    msg: STR,
    isMsgHighlighted: NUM_NULL,
    isProcessHighlighted: NUM,
    processName: STR,
    systemdUnit: STR,
  });
  for (; it.valid(); it.next()) {
    ids.push(it.id);
    timestamps.push(Time.fromRaw(it.ts));
    pids.push(it.pid);
    tids.push(it.tid);
    priorities.push(it.prio);
    upids.push(it.upid);
    utids.push(it.utid);
    tags.push(it.tag);
    units.push(it.systemdUnit);
    messages.push(it.msg);
    isHighlighted.push(
      it.isMsgHighlighted === 1 || it.isProcessHighlighted === 1,
    );
    processName.push(it.processName);
  }

  const queryRes = await engine.query(`
    select
      count(*) as totalEvents
    from filtered_linux_systemd_journald_logs
    where ts >= ${span.start} and ts <= ${span.end}
  `);
  const {totalEvents} = queryRes.firstRow({totalEvents: NUM});

  return {
    offset: pagination.offset,
    ids,
    timestamps,
    pids,
    tids,
    priorities,
    upids,
    utids,
    tags,
    units,
    messages,
    isHighlighted,
    processName,
    totalEvents,
  };
}

async function updateLogView(
  engine: Engine,
  filter: JournaldLogFilteringCriteria,
): Promise<AsyncDisposable> {
  const globMatch = composeGlobMatch(filter.hideNonMatching, filter.textEntry);
  let selectedRows = `select linux_systemd_journald_logs.id, prio, ts, pid, tid, tag, msg,
      process.name as process_name,
      ifnull(linux_systemd_journald_logs.systemd_unit, '') as systemd_unit,
      thread.upid as upid, thread.utid as utid,
      ${globMatch}
      from linux_systemd_journald_logs
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

  await engine.query(
    `create perfetto table filtered_linux_systemd_journald_logs as select *
      from (${selectedRows})
      where is_msg_chosen is 1 or is_process_chosen is 1`,
  );

  return {
    async [Symbol.asyncDispose]() {
      await engine.query('drop table filtered_linux_systemd_journald_logs');
    },
  };
}

function serializeTags(tags: string[]) {
  return tags.map((tag) => escapeQuery(tag)).join();
}

function composeGlobMatch(isCollapsed: boolean, textEntry: string) {
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

export {JournaldLogPanel as LogPanel};

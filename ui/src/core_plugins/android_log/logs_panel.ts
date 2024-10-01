// Copyright (C) 2019 The Android Open Source Project
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
import {raf} from '../../core/raf_scheduler';
import {DetailsShell} from '../../widgets/details_shell';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {Monitor} from '../../base/monitor';
import {AsyncLimiter} from '../../base/async_limiter';
import {escapeGlob, escapeQuery} from '../../trace_processor/query_utils';
import {Select} from '../../widgets/select';
import {Button} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {VirtualTable, VirtualTableRow} from '../../widgets/virtual_table';
import {classNames} from '../../base/classnames';
import {TagInput} from '../../widgets/tag_input';
import {Store} from '../../base/store';
import {Trace} from '../../public/trace';

const ROW_H = 20;

export interface LogFilteringCriteria {
  minimumLevel: number;
  tags: string[];
  textEntry: string;
  hideNonMatching: boolean;
}

export interface LogPanelAttrs {
  filterStore: Store<LogFilteringCriteria>;
  trace: Trace;
}

interface Pagination {
  offset: number;
  count: number;
}

interface LogEntries {
  offset: number;
  timestamps: time[];
  priorities: number[];
  tags: string[];
  messages: string[];
  isHighlighted: boolean[];
  processName: string[];
  totalEvents: number; // Count of the total number of events within this window
}

export class LogPanel implements m.ClassComponent<LogPanelAttrs> {
  private entries?: LogEntries;

  private pagination: Pagination = {
    offset: 0,
    count: 0,
  };
  private readonly rowsMonitor: Monitor;
  private readonly filterMonitor: Monitor;
  private readonly queryLimiter = new AsyncLimiter();

  constructor({attrs}: m.CVnode<LogPanelAttrs>) {
    this.rowsMonitor = new Monitor([
      () => attrs.filterStore.state,
      () => attrs.trace.timeline.visibleWindow.toTimeSpan().start,
      () => attrs.trace.timeline.visibleWindow.toTimeSpan().end,
    ]);

    this.filterMonitor = new Monitor([() => attrs.filterStore.state]);
  }

  view({attrs}: m.CVnode<LogPanelAttrs>) {
    if (this.rowsMonitor.ifStateChanged()) {
      this.reloadData(attrs);
    }

    const hasProcessNames =
      this.entries &&
      this.entries.processName.filter((name) => name).length > 0;
    const totalEvents = this.entries?.totalEvents ?? 0;

    return m(
      DetailsShell,
      {
        title: 'Android Logs',
        description: `Total messages: ${totalEvents}`,
        buttons: m(LogsFilters, {store: attrs.filterStore}),
      },
      m(VirtualTable, {
        className: 'pf-android-logs-table',
        columns: [
          {header: 'Timestamp', width: '13em'},
          {header: 'Level', width: '4em'},
          {header: 'Tag', width: '13em'},
          ...(hasProcessNames ? [{header: 'Process', width: '18em'}] : []),
          // '' means column width can vary depending on the content.
          // This works as this is the last column, but using this for other
          // columns will pull the columns to the right out of line.
          {header: 'Message', width: ''},
        ],
        rows: this.renderRows(hasProcessNames),
        firstRowOffset: this.entries?.offset ?? 0,
        numRows: this.entries?.totalEvents ?? 0,
        rowHeight: ROW_H,
        onReload: (offset, count) => {
          this.pagination = {offset, count};
          this.reloadData(attrs);
        },
        onRowHover: (id) => {
          const timestamp = this.entries?.timestamps[id];
          if (timestamp !== undefined) {
            attrs.trace.timeline.hoverCursorTimestamp = timestamp;
          }
        },
        onRowOut: () => {
          attrs.trace.timeline.hoverCursorTimestamp = undefined;
        },
      }),
    );
  }

  private reloadData(attrs: LogPanelAttrs) {
    this.queryLimiter.schedule(async () => {
      const visibleSpan = attrs.trace.timeline.visibleWindow.toTimeSpan();

      if (this.filterMonitor.ifStateChanged()) {
        await updateLogView(attrs.trace.engine, attrs.filterStore.state);
      }

      this.entries = await updateLogEntries(
        attrs.trace.engine,
        visibleSpan,
        this.pagination,
      );

      raf.scheduleFullRedraw();
    });
  }

  private renderRows(hasProcessNames: boolean | undefined): VirtualTableRow[] {
    if (!this.entries) {
      return [];
    }

    const timestamps = this.entries.timestamps;
    const priorities = this.entries.priorities;
    const tags = this.entries.tags;
    const messages = this.entries.messages;
    const processNames = this.entries.processName;

    const rows: VirtualTableRow[] = [];
    for (let i = 0; i < this.entries.timestamps.length; i++) {
      const priorityLetter = LOG_PRIORITIES[priorities[i]][0];
      const ts = timestamps[i];
      const prioClass = priorityLetter ?? '';

      rows.push({
        id: i,
        className: classNames(
          prioClass,
          this.entries.isHighlighted[i] && 'pf-highlighted',
        ),
        cells: [
          m(Timestamp, {ts}),
          priorityLetter || '?',
          tags[i],
          ...(hasProcessNames ? [processNames[i]] : []),
          messages[i],
        ],
      });
    }

    return rows;
  }
}

export const LOG_PRIORITIES = [
  '-',
  '-',
  'Verbose',
  'Debug',
  'Info',
  'Warn',
  'Error',
  'Fatal',
];
const IGNORED_STATES = 2;

interface LogPriorityWidgetAttrs {
  options: string[];
  selectedIndex: number;
  onSelect: (id: number) => void;
}

class LogPriorityWidget implements m.ClassComponent<LogPriorityWidgetAttrs> {
  view(vnode: m.Vnode<LogPriorityWidgetAttrs>) {
    const attrs = vnode.attrs;
    const optionComponents = [];
    for (let i = IGNORED_STATES; i < attrs.options.length; i++) {
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
  onChange: (value: string) => void;
}

class LogTextWidget implements m.ClassComponent<LogTextWidgetAttrs> {
  view({attrs}: m.CVnode<LogTextWidgetAttrs>) {
    return m(TextInput, {
      placeholder: 'Search logs...',
      onkeyup: (e: KeyboardEvent) => {
        // We want to use the value of the input field after it has been
        // updated with the latest key (onkeyup).
        const htmlElement = e.target as HTMLInputElement;
        attrs.onChange(htmlElement.value);
      },
    });
  }
}

interface FilterByTextWidgetAttrs {
  hideNonMatching: boolean;
  disabled: boolean;
  onClick: () => void;
}

class FilterByTextWidget implements m.ClassComponent<FilterByTextWidgetAttrs> {
  view({attrs}: m.Vnode<FilterByTextWidgetAttrs>) {
    const icon = attrs.hideNonMatching ? 'unfold_less' : 'unfold_more';
    const tooltip = attrs.hideNonMatching
      ? 'Expand all and view highlighted'
      : 'Collapse all';
    return m(Button, {
      icon,
      title: tooltip,
      disabled: attrs.disabled,
      onclick: attrs.onClick,
    });
  }
}

interface LogsFiltersAttrs {
  store: Store<LogFilteringCriteria>;
}

export class LogsFilters implements m.ClassComponent<LogsFiltersAttrs> {
  view({attrs}: m.CVnode<LogsFiltersAttrs>) {
    return [
      m('.log-label', 'Log Level'),
      m(LogPriorityWidget, {
        options: LOG_PRIORITIES,
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
          prio,
          ifnull(tag, '[NULL]') as tag,
          ifnull(msg, '[NULL]') as msg,
          is_msg_highlighted as isMsgHighlighted,
          is_process_highlighted as isProcessHighlighted,
          ifnull(process_name, '') as processName
        from filtered_logs
        where ts >= ${span.start} and ts <= ${span.end}
        order by ts
        limit ${pagination.offset}, ${pagination.count}
    `);

  const timestamps: time[] = [];
  const priorities = [];
  const tags = [];
  const messages = [];
  const isHighlighted = [];
  const processName = [];

  const it = rowsResult.iter({
    ts: LONG,
    prio: NUM,
    tag: STR,
    msg: STR,
    isMsgHighlighted: NUM_NULL,
    isProcessHighlighted: NUM,
    processName: STR,
  });
  for (; it.valid(); it.next()) {
    timestamps.push(Time.fromRaw(it.ts));
    priorities.push(it.prio);
    tags.push(it.tag);
    messages.push(it.msg);
    isHighlighted.push(
      it.isMsgHighlighted === 1 || it.isProcessHighlighted === 1,
    );
    processName.push(it.processName);
  }

  const queryRes = await engine.query(`
    select
      count(*) as totalEvents
    from filtered_logs
    where ts >= ${span.start} and ts <= ${span.end}
  `);
  const {totalEvents} = queryRes.firstRow({totalEvents: NUM});

  return {
    offset: pagination.offset,
    timestamps,
    priorities,
    tags,
    messages,
    isHighlighted,
    processName,
    totalEvents,
  };
}

async function updateLogView(engine: Engine, filter: LogFilteringCriteria) {
  await engine.query('drop view if exists filtered_logs');

  const globMatch = composeGlobMatch(filter.hideNonMatching, filter.textEntry);
  let selectedRows = `select prio, ts, tag, msg,
      process.name as process_name, ${globMatch}
      from android_logs
      left join thread using(utid)
      left join process using(upid)
      where prio >= ${filter.minimumLevel}`;
  if (filter.tags.length) {
    selectedRows += ` and tag in (${serializeTags(filter.tags)})`;
  }

  // We extract only the rows which will be visible.
  await engine.query(`create view filtered_logs as select *
    from (${selectedRows})
    where is_msg_chosen is 1 or is_process_chosen is 1`);
}

function serializeTags(tags: string[]) {
  return tags.map((tag) => escapeQuery(tag)).join();
}

function composeGlobMatch(isCollaped: boolean, textEntry: string) {
  if (isCollaped) {
    // If the entries are collapsed, we won't highlight any lines.
    return `msg glob ${escapeGlob(textEntry)} as is_msg_chosen,
      (process.name is not null and process.name glob ${escapeGlob(
        textEntry,
      )}) as is_process_chosen,
      0 as is_msg_highlighted,
      0 as is_process_highlighted`;
  } else if (!textEntry) {
    // If there is no text entry, we will show all lines, but won't highlight.
    // any.
    return `1 as is_msg_chosen,
      1 as is_process_chosen,
      0 as is_msg_highlighted,
      0 as is_process_highlighted`;
  } else {
    return `1 as is_msg_chosen,
      1 as is_process_chosen,
      msg glob ${escapeGlob(textEntry)} as is_msg_highlighted,
      (process.name is not null and process.name glob ${escapeGlob(
        textEntry,
      )}) as is_process_highlighted`;
  }
}

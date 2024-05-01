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

import {duration, Span, time, Time, TimeSpan} from '../../base/time';
import {Actions} from '../../common/actions';
import {raf} from '../../core/raf_scheduler';
import {DetailsShell} from '../../widgets/details_shell';
import {VirtualScrollContainer} from '../../widgets/virtual_scroll_container';

import {SELECTED_LOG_ROWS_COLOR} from '../../frontend/css_constants';
import {globals} from '../../frontend/globals';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {createStore, EngineProxy, LONG, NUM, Store, STR} from '../../public';
import {Monitor} from '../../base/monitor';
import {AsyncLimiter} from '../../base/async_limiter';
import {escapeGlob, escapeQuery} from '../../trace_processor/query_utils';
import {Select} from '../../widgets/select';
import {Button} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {Intent} from '../../widgets/common';

const ROW_H = 20;

export interface LogFilteringCriteria {
  minimumLevel: number;
  tags: string[];
  textEntry: string;
  hideNonMatching: boolean;
}

export interface LogPanelAttrs {
  filterStore: Store<LogFilteringCriteria>;
  engine: EngineProxy;
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
  private readonly SKIRT_SIZE = 50;
  private entries?: LogEntries;
  private isStale = true;
  private viewportBounds = {top: 0, bottom: 0};

  private readonly paginationStore = createStore<Pagination>({
    offset: 0,
    count: 0,
  });
  private readonly rowsMonitor: Monitor;
  private readonly filterMonitor: Monitor;
  private readonly queryLimiter = new AsyncLimiter();

  constructor({attrs}: m.CVnode<LogPanelAttrs>) {
    this.rowsMonitor = new Monitor([
      () => attrs.filterStore.state,
      () => globals.state.frontendLocalState.visibleState.start,
      () => globals.state.frontendLocalState.visibleState.end,
      () => this.paginationStore.state,
    ]);

    this.filterMonitor = new Monitor([() => attrs.filterStore.state]);
  }

  view({attrs}: m.CVnode<LogPanelAttrs>) {
    if (this.rowsMonitor.ifStateChanged()) {
      this.queryLimiter.schedule(async () => {
        this.isStale = true;
        raf.scheduleFullRedraw();

        const visibleState = globals.state.frontendLocalState.visibleState;
        const visibleSpan = new TimeSpan(visibleState.start, visibleState.end);

        if (this.filterMonitor.ifStateChanged()) {
          await updateLogView(attrs.engine, attrs.filterStore.state);
        }

        this.entries = await updateLogEntries(
          attrs.engine,
          visibleSpan,
          this.paginationStore.state,
        );

        raf.scheduleFullRedraw();
        this.isStale = false;
      });
    }

    const hasProcessNames =
      this.entries &&
      this.entries.processName.filter((name) => name).length > 0;

    const rows: m.Children = [];
    rows.push(
      m(
        `.row`,
        m('.cell.row-header', 'Timestamp'),
        m('.cell.row-header', 'Level'),
        m('.cell.row-header', 'Tag'),
        hasProcessNames
          ? m('.cell.with-process.row-header', 'Process name')
          : undefined,
        hasProcessNames
          ? m('.cell.with-process.row-header', 'Message')
          : m('.cell.no-process.row-header', 'Message'),
        m('br'),
      ),
    );
    if (this.entries) {
      const offset = this.entries.offset;
      const timestamps = this.entries.timestamps;
      const priorities = this.entries.priorities;
      const tags = this.entries.tags;
      const messages = this.entries.messages;
      const processNames = this.entries.processName;
      const totalEvents = this.entries.totalEvents;

      for (let i = 0; i < this.entries.timestamps.length; i++) {
        const priorityLetter = LOG_PRIORITIES[priorities[i]][0];
        const ts = timestamps[i];
        const prioClass = priorityLetter || '';
        const style: {top: string; backgroundColor?: string} = {
          // 1.5 is for the width of the header
          top: `${(offset + i + 1.5) * ROW_H}px`,
        };
        if (this.entries.isHighlighted[i]) {
          style.backgroundColor = SELECTED_LOG_ROWS_COLOR;
        }

        rows.push(
          m(
            `.row.${prioClass}`,
            {
              class: this.isStale ? 'stale' : '',
              style,
              onmouseover: () => {
                globals.dispatch(Actions.setHoverCursorTimestamp({ts}));
              },
              onmouseout: () => {
                globals.dispatch(
                  Actions.setHoverCursorTimestamp({ts: Time.INVALID}),
                );
              },
            },
            m('.cell', m(Timestamp, {ts})),
            m('.cell', priorityLetter || '?'),
            m('.cell', tags[i]),
            hasProcessNames
              ? m('.cell.with-process', processNames[i])
              : undefined,
            hasProcessNames
              ? m('.cell.with-process', messages[i])
              : m('.cell.no-process', messages[i]),
            m('br'),
          ),
        );
      }

      return m(
        DetailsShell,
        {
          title: 'Android Logs',
          description: `[${this.viewportBounds.top}, ${this.viewportBounds.bottom}] / ${totalEvents}`,
          buttons: m(LogsFilters, {store: attrs.filterStore}),
        },
        m(
          VirtualScrollContainer,
          {
            onScroll: (scrollContainer: HTMLElement) => {
              this.recomputeVisibleRowsAndUpdate(scrollContainer);
              raf.scheduleFullRedraw();
            },
          },
          m(
            '.log-panel',
            m('.rows', {style: {height: `${totalEvents * ROW_H}px`}}, rows),
          ),
        ),
      );
    }

    return null;
  }

  recomputeVisibleRowsAndUpdate(scrollContainer: HTMLElement) {
    const viewportTop = Math.floor(scrollContainer.scrollTop / ROW_H);
    const viewportHeight = Math.ceil(scrollContainer.clientHeight / ROW_H);
    const viewportBottom = viewportTop + viewportHeight;

    this.viewportBounds = {
      top: viewportTop,
      bottom: viewportBottom,
    };

    const curPage = this.paginationStore.state;

    if (
      viewportTop < curPage.offset ||
      viewportBottom >= curPage.offset + curPage.count
    ) {
      this.paginationStore.edit((draft) => {
        const offset = Math.max(0, viewportTop - this.SKIRT_SIZE);
        // Make it even so alternating coloured rows line up
        const offsetEven = Math.floor(offset / 2) * 2;
        draft.offset = offsetEven;
        draft.count = viewportHeight + this.SKIRT_SIZE * 2;
      });
    }
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

interface LogTagChipAttrs {
  name: string;
  removeTag: (name: string) => void;
}

class LogTagChip implements m.ClassComponent<LogTagChipAttrs> {
  view({attrs}: m.CVnode<LogTagChipAttrs>) {
    return m(Button, {
      label: attrs.name,
      rightIcon: 'close',
      onclick: () => attrs.removeTag(attrs.name),
      intent: Intent.Primary,
    });
  }
}

interface LogTagsWidgetAttrs {
  tags: string[];
  onRemoveTag: (tag: string) => void;
  onAddTag: (tag: string) => void;
}

class LogTagsWidget implements m.ClassComponent<LogTagsWidgetAttrs> {
  view(vnode: m.Vnode<LogTagsWidgetAttrs>) {
    const tags = vnode.attrs.tags;
    return [
      tags.map((tag) =>
        m(LogTagChip, {
          name: tag,
          removeTag: (tag) => vnode.attrs.onRemoveTag(tag),
        }),
      ),
      m(TextInput, {
        placeholder: 'Filter by tag...',
        onkeydown: (e: KeyboardEvent) => {
          // This is to avoid zooming on 'w'(and other unexpected effects
          // of key presses in this input field).
          e.stopPropagation();
          const htmlElement = e.target as HTMLInputElement;

          // When the user clicks 'Backspace' we delete the previous tag.
          if (
            e.key === 'Backspace' &&
            tags.length > 0 &&
            htmlElement.value === ''
          ) {
            vnode.attrs.onRemoveTag(tags[tags.length - 1]);
            return;
          }

          if (e.key !== 'Enter') {
            return;
          }
          if (htmlElement.value === '') {
            return;
          }
          vnode.attrs.onAddTag(htmlElement.value.trim());
          htmlElement.value = '';
        },
      }),
    ];
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
      m(LogTagsWidget, {
        tags: attrs.store.state.tags,
        onAddTag: (tag) => {
          attrs.store.edit((draft) => {
            draft.tags.push(tag);
          });
        },
        onRemoveTag: (tag) => {
          attrs.store.edit((draft) => {
            draft.tags = draft.tags.filter((t) => t !== tag);
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
  engine: EngineProxy,
  span: Span<time, duration>,
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
    isMsgHighlighted: NUM,
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

async function updateLogView(
  engine: EngineProxy,
  filter: LogFilteringCriteria,
) {
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

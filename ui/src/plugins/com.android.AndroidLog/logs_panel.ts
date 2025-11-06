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
import {DetailsShell} from '../../widgets/details_shell';
import {Timestamp} from '../../components/widgets/timestamp';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {Monitor} from '../../base/monitor';
import {AsyncLimiter} from '../../base/async_limiter';
import {
  escapeQuery,
  escapeSearchQuery,
} from '../../trace_processor/query_utils';
import {
  Grid,
  GridColumn,
  GridRow,
  GridHeaderCell,
  GridCell,
} from '../../widgets/grid';
import {classNames} from '../../base/classnames';
import {
  FilterInput,
  TagDefinition,
  SelectedTag,
} from '../../widgets/filter_input';
import {Store} from '../../base/store';
import {Trace} from '../../public/trace';
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';

const ROW_H = 24;

export interface LogFilteringCriteria {
  readonly minimumLevel?: number;
  readonly tags: string[];
  readonly pids: bigint[];
  readonly textEntry: string;
  readonly hideNonMatching: boolean;
  readonly machineExcludeList: number[];
}

export interface LogPanelCache {
  readonly uniqueMachineIds: number[];
}

export interface LogPanelAttrs {
  readonly cache: LogPanelCache;
  readonly filterStore: Store<LogFilteringCriteria>;
  readonly trace: Trace;
}

interface Pagination {
  readonly offset: number;
  readonly count: number;
}

interface LogEntries {
  readonly offset: number;
  readonly machineIds: number[];
  readonly timestamps: time[];
  readonly pids: bigint[];
  readonly tids: bigint[];
  readonly priorities: number[];
  readonly tags: string[];
  readonly messages: string[];
  readonly isHighlighted: boolean[];
  readonly processName: string[];
  readonly totalEvents: number;
}

export class LogPanel implements m.ClassComponent<LogPanelAttrs> {
  private readonly trace: Trace;
  private entries?: LogEntries;
  private pagination: Pagination = {
    offset: 0,
    count: 0,
  };
  private readonly rowsMonitor: Monitor;
  private readonly filterMonitor: Monitor;
  private readonly queryLimiter = new AsyncLimiter();

  constructor({attrs}: m.CVnode<LogPanelAttrs>) {
    this.trace = attrs.trace;
    this.rowsMonitor = new Monitor([
      () => attrs.filterStore.state,
      () => attrs.trace.timeline.visibleWindow.toTimeSpan().start,
      () => attrs.trace.timeline.visibleWindow.toTimeSpan().end,
    ]);

    this.filterMonitor = new Monitor([() => attrs.filterStore.state]);
  }

  view({attrs}: m.CVnode<LogPanelAttrs>) {
    if (this.rowsMonitor.ifStateChanged()) {
      this.scheduleDataReload(attrs);
    }

    const hasMachineIds = attrs.cache.uniqueMachineIds.length > 1;
    const hasProcessNames =
      this.entries &&
      this.entries.processName.filter((name) => name).length > 0;
    const totalEvents = this.entries?.totalEvents ?? 0;

    const columns: GridColumn[] = [
      ...(hasMachineIds
        ? [{key: 'machine', header: m(GridHeaderCell, 'Machine')}]
        : []),
      {key: 'timestamp', header: m(GridHeaderCell, 'Timestamp')},
      {key: 'pid', header: m(GridHeaderCell, 'PID')},
      {key: 'tid', header: m(GridHeaderCell, 'TID')},
      {key: 'level', header: m(GridHeaderCell, 'Level')},
      ...(hasProcessNames
        ? [{key: 'process', header: m(GridHeaderCell, 'Process')}]
        : []),
      {key: 'tag', header: m(GridHeaderCell, 'Tag')},
      {
        key: 'message',
        maxInitialWidthPx: Infinity,
        header: m(GridHeaderCell, 'Message'),
      },
    ];

    return m(
      DetailsShell,
      {
        title: 'Android Logs',
        description: `Total messages: ${totalEvents}`,
        fillButtonsSpace: true,
        buttons: m(LogsFilters, {
          trace: attrs.trace,
          cache: attrs.cache,
          store: attrs.filterStore,
        }),
      },
      m(Grid, {
        className: 'pf-logs-panel',
        columns,
        rowData: {
          data: this.renderRows(hasMachineIds, hasProcessNames),
          total: this.entries?.totalEvents ?? 0,
          offset: this.entries?.offset ?? 0,
          onLoadData: (offset, count) => {
            this.pagination = {offset, count};
            this.scheduleDataReload(attrs);
          },
        },
        virtualization: {
          rowHeightPx: ROW_H,
        },
        fillHeight: true,
        onRowHover: (rowIndex) => {
          const actualIndex = rowIndex - (this.entries?.offset ?? 0);
          const timestamp = this.entries?.timestamps[actualIndex];
          if (timestamp !== undefined) {
            attrs.trace.timeline.hoverCursorTimestamp = timestamp;
          }
        },
        onRowOut: () => {
          attrs.trace.timeline.hoverCursorTimestamp = undefined;
        },
        emptyState: this.renderEmptyState(attrs),
      }),
    );
  }

  private scheduleDataReload(attrs: LogPanelAttrs) {
    const visibleSpan = attrs.trace.timeline.visibleWindow.toTimeSpan();
    const filterStateChanged = this.filterMonitor.ifStateChanged();
    const filterStoreState = attrs.filterStore.state;
    const engine = attrs.trace.engine;
    const pagination = this.pagination;

    this.queryLimiter.schedule(async () => {
      if (filterStateChanged) {
        await updateLogView(engine, filterStoreState);
      }

      this.entries = await updateLogEntries(engine, visibleSpan, pagination);
    });
  }

  private renderRows(
    hasMachineIds: boolean | undefined,
    hasProcessNames: boolean | undefined,
  ): ReadonlyArray<GridRow> {
    if (!this.entries) {
      return [];
    }

    const trace = this.trace;
    const machineIds = this.entries.machineIds;
    const timestamps = this.entries.timestamps;
    const pids = this.entries.pids;
    const tids = this.entries.tids;
    const priorities = this.entries.priorities;
    const tags = this.entries.tags;
    const messages = this.entries.messages;
    const processNames = this.entries.processName;

    const rows: GridRow[] = [];
    for (let i = 0; i < this.entries.timestamps.length; i++) {
      const priority = priorities[i];
      const priorityLetter = LOG_PRIORITIES[priority][0];
      const ts = timestamps[i];
      const priorityClass = `pf-logs-panel__row--${classForPriority(priority)}`;
      const isHighlighted = this.entries.isHighlighted[i];
      const className = classNames(
        priorityClass,
        isHighlighted && 'pf-logs-panel__row--highlighted',
      );

      const row = [
        hasMachineIds &&
          m(GridCell, {className, align: 'right'}, machineIds[i]),
        m(GridCell, {className}, m(Timestamp, {trace, ts})),
        m(GridCell, {className, align: 'right'}, String(pids[i])),
        m(GridCell, {className, align: 'right'}, String(tids[i])),
        m(GridCell, {className}, priorityLetter || '?'),
        hasProcessNames && m(GridCell, {className}, processNames[i]),
        m(GridCell, {className}, tags[i]),
        m(GridCell, {className}, messages[i]),
      ].filter(Boolean);

      rows.push(row);
    }

    return rows;
  }

  private renderEmptyState(attrs: LogPanelAttrs): m.Children {
    const totalEvents = this.entries?.totalEvents ?? 0;
    if (totalEvents > 0) {
      return undefined;
    }

    const filterState = attrs.filterStore.state;
    const hasFilters =
      filterState.minimumLevel !== undefined ||
      filterState.tags.length > 0 ||
      filterState.pids.length > 0 ||
      filterState.textEntry !== '' ||
      filterState.machineExcludeList.length > 0;

    return m(
      EmptyState,
      {
        icon: hasFilters ? 'filter_list_off' : 'article',
        title: hasFilters ? 'No logs match your filters' : 'No logs available',
        fillHeight: true,
      },
      hasFilters &&
        m(Button, {
          label: 'Clear all filters',
          onclick: () => {
            attrs.filterStore.edit((draft) => {
              draft.minimumLevel = undefined;
              draft.tags = [];
              draft.pids = [];
              draft.textEntry = '';
              draft.hideNonMatching = false;
              draft.machineExcludeList = [];
            });
          },
        }),
    );
  }
}

function classForPriority(priority: number) {
  switch (priority) {
    case 2:
      return 'verbose';
    case 3:
      return 'debug';
    case 4:
      return 'info';
    case 5:
      return 'warn';
    case 6:
      return 'error';
    case 7:
      return 'fatal';
    default:
      return undefined;
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

interface LogsFiltersAttrs {
  readonly trace: Trace;
  readonly cache: LogPanelCache;
  readonly store: Store<LogFilteringCriteria>;
}

interface FilterByTextWidgetAttrs {
  readonly hideNonMatching: boolean;
  readonly disabled: boolean;
  readonly onClick: () => void;
}

class FilterByTextWidget implements m.ClassComponent<FilterByTextWidgetAttrs> {
  view({attrs}: m.Vnode<FilterByTextWidgetAttrs>) {
    const icon = attrs.hideNonMatching ? 'filter_alt' : 'filter_alt_off';
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

export class LogsFilters implements m.ClassComponent<LogsFiltersAttrs> {
  private uniqueTags: string[] = [];
  private uniquePids: bigint[] = [];

  async oncreate({attrs}: m.CVnode<LogsFiltersAttrs>) {
    const engine = attrs.trace.engine;

    // Fetch unique tags from the database
    const tagsResult = await engine.query(`
      select distinct tag
      from android_logs
      where tag is not null
      order by tag
    `);
    const tags: string[] = [];
    const tagsIt = tagsResult.iter({tag: STR});
    for (; tagsIt.valid(); tagsIt.next()) {
      tags.push(tagsIt.tag);
    }
    this.uniqueTags = tags;

    // Fetch unique PIDs from the database
    const pidsResult = await engine.query(`
      select distinct pid
      from android_logs
      left join thread using(utid)
      left join process using(upid)
      where pid is not null
      order by pid
    `);
    const pids: bigint[] = [];
    const pidsIt = pidsResult.iter({pid: LONG});
    for (; pidsIt.valid(); pidsIt.next()) {
      pids.push(pidsIt.pid);
    }
    this.uniquePids = pids;

    m.redraw();
  }

  view({attrs}: m.CVnode<LogsFiltersAttrs>) {
    const tagDefinitions = this.buildTagDefinitions(attrs);
    const selectedTags = this.convertStateToTags(attrs.store.state, attrs);

    return [
      m(FilterInput, {
        className: 'pf-logs-panel__filter',
        tags: tagDefinitions,
        selectedTags,
        placeholder:
          'Add filters (e.g., level:info, tag:ActivityManager) or type to search...',
        onTagAdd: (tag: SelectedTag) => {
          attrs.store.edit((draft) => {
            this.applyTagAddition(tag, draft, attrs);
          });
        },
        onTagRemove: (tag: SelectedTag) => {
          attrs.store.edit((draft) => {
            this.applyTagRemoval(tag, draft, attrs);
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

  private buildTagDefinitions(attrs: LogsFiltersAttrs): TagDefinition[] {
    const definitions: TagDefinition[] = [
      {
        key: 'search',
        freeform: true,
        isDefault: true, // search is the default tag
      },
      {
        key: 'level',
        values: LOG_PRIORITIES.slice(IGNORED_STATES).map((name, idx) => ({
          key: String(idx + IGNORED_STATES),
          label: name,
        })),
      },
      {
        key: 'tag',
        values: this.uniqueTags.map((tag) => ({
          key: tag,
        })),
      },
      {
        key: 'pid',
        values: this.uniquePids.map((pid) => ({
          key: String(pid),
        })),
      },
    ];

    if (attrs.cache.uniqueMachineIds.length > 1) {
      definitions.push({
        key: 'machine',
        values: attrs.cache.uniqueMachineIds.map((id) => ({
          key: String(id),
          label: `${id}`,
        })),
      });
    }

    return definitions;
  }

  private convertStateToTags(
    state: LogFilteringCriteria,
    attrs: LogsFiltersAttrs,
  ): SelectedTag[] {
    const tags: SelectedTag[] = [];

    // Add level only if it's set
    if (state.minimumLevel !== undefined) {
      tags.push({tagKey: 'level', valueKey: String(state.minimumLevel)});
    }

    // Add tags
    for (const tag of state.tags) {
      tags.push({tagKey: 'tag', valueKey: tag});
    }

    // Add PIDs
    for (const pid of state.pids) {
      tags.push({tagKey: 'pid', valueKey: String(pid)});
    }

    // Add search text as a tag if present
    if (state.textEntry) {
      tags.push({tagKey: 'search', valueKey: state.textEntry});
    }

    // Add included machines (opposite of exclude list)
    if (attrs.cache.uniqueMachineIds.length > 1) {
      for (const machineId of attrs.cache.uniqueMachineIds) {
        if (!state.machineExcludeList.includes(machineId)) {
          tags.push({tagKey: 'machine', valueKey: String(machineId)});
        }
      }
    }

    return tags;
  }

  private applyTagAddition(
    tag: SelectedTag,
    draft: {
      -readonly [K in keyof LogFilteringCriteria]: LogFilteringCriteria[K];
    },
    _attrs: LogsFiltersAttrs,
  ): void {
    if (tag.tagKey === 'level') {
      draft.minimumLevel = Number(tag.valueKey);
    } else if (tag.tagKey === 'tag') {
      if (!draft.tags.includes(tag.valueKey)) {
        draft.tags.push(tag.valueKey);
      }
    } else if (tag.tagKey === 'pid') {
      const pid = BigInt(tag.valueKey);
      if (!draft.pids.includes(pid)) {
        draft.pids.push(pid);
      }
    } else if (tag.tagKey === 'search') {
      draft.textEntry = tag.valueKey;
    } else if (tag.tagKey === 'machine') {
      const machineId = Number(tag.valueKey);
      const idx = draft.machineExcludeList.indexOf(machineId);
      if (idx !== -1) {
        draft.machineExcludeList.splice(idx, 1);
      }
    }
  }

  private applyTagRemoval(
    tag: SelectedTag,
    draft: {
      -readonly [K in keyof LogFilteringCriteria]: LogFilteringCriteria[K];
    },
    _attrs: LogsFiltersAttrs,
  ): void {
    if (tag.tagKey === 'level') {
      draft.minimumLevel = undefined; // Remove level filter
    } else if (tag.tagKey === 'tag') {
      const idx = draft.tags.indexOf(tag.valueKey);
      if (idx !== -1) {
        draft.tags.splice(idx, 1);
      }
    } else if (tag.tagKey === 'pid') {
      const pid = BigInt(tag.valueKey);
      const idx = draft.pids.indexOf(pid);
      if (idx !== -1) {
        draft.pids.splice(idx, 1);
      }
    } else if (tag.tagKey === 'search') {
      draft.textEntry = '';
    } else if (tag.tagKey === 'machine') {
      const machineId = Number(tag.valueKey);
      if (!draft.machineExcludeList.includes(machineId)) {
        draft.machineExcludeList.push(machineId);
      }
    }
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
          prio,
          ifnull(tag, '[NULL]') as tag,
          ifnull(msg, '[NULL]') as msg,
          is_msg_highlighted as isMsgHighlighted,
          is_process_highlighted as isProcessHighlighted,
          ifnull(process_name, '') as processName,
          machine_id as machineId
        from filtered_logs
        where ts >= ${span.start} and ts <= ${span.end}
        order by ts
        limit ${pagination.offset}, ${pagination.count}
    `);

  const machineIds = [];
  const timestamps: time[] = [];
  const pids = [];
  const tids = [];
  const priorities = [];
  const tags = [];
  const messages = [];
  const isHighlighted = [];
  const processName = [];

  const it = rowsResult.iter({
    ts: LONG,
    pid: LONG,
    tid: LONG,
    prio: NUM,
    tag: STR,
    msg: STR,
    isMsgHighlighted: NUM_NULL,
    isProcessHighlighted: NUM,
    processName: STR,
    machineId: NUM_NULL,
  });
  for (; it.valid(); it.next()) {
    timestamps.push(Time.fromRaw(it.ts));
    pids.push(it.pid);
    tids.push(it.tid);
    priorities.push(it.prio);
    tags.push(it.tag);
    messages.push(it.msg);
    isHighlighted.push(
      it.isMsgHighlighted === 1 || it.isProcessHighlighted === 1,
    );
    processName.push(it.processName);
    machineIds.push(it.machineId ?? 0);
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
    machineIds,
    timestamps,
    pids,
    tids,
    priorities,
    tags,
    messages,
    isHighlighted,
    processName,
    totalEvents,
  };
}

function getMinimumLevel(filter: LogFilteringCriteria): number {
  return filter.minimumLevel ?? 2; // Default to Verbose if not set
}

async function updateLogView(engine: Engine, filter: LogFilteringCriteria) {
  await engine.query('drop view if exists filtered_logs');

  const minLevel = getMinimumLevel(filter);
  const globMatch = composeGlobMatch(filter.hideNonMatching, filter.textEntry);
  let selectedRows = `select prio, ts, pid, tid, tag, msg,
      process.name as process_name,
      process.machine_id as machine_id, ${globMatch}
      from android_logs
      left join thread using(utid)
      left join process using(upid)
      where prio >= ${minLevel}`;
  if (filter.tags.length) {
    selectedRows += ` and tag in (${serializeTags(filter.tags)})`;
  }
  if (filter.pids.length) {
    selectedRows += ` and pid in (${filter.pids.join(',')})`;
  }
  if (filter.machineExcludeList.length) {
    selectedRows += ` and ifnull(process.machine_id, 0) not in (${filter.machineExcludeList.join(',')})`;
  }

  await engine.query(`create view filtered_logs as select *
    from (${selectedRows})
    where is_msg_chosen is 1 or is_process_chosen is 1`);
}

function serializeTags(tags: string[]) {
  return tags.map((tag) => escapeQuery(tag)).join();
}

function composeGlobMatch(isCollaped: boolean, textEntry: string) {
  if (isCollaped) {
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

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
import {AtomicTaskQueue, AsyncMemo} from '../../base/async_memo';
import {type time, Time} from '../../base/time';
import {materialColorScheme} from '../../components/colorizer';
import {Timestamp} from '../../components/widgets/timestamp';
import type {Cpu} from '../../components/cpu';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {
  type MultiSelectDiff,
  type MultiSelectOption,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {PopupPosition} from '../../widgets/popup';
import {
  Grid,
  type GridColumn,
  GridHeaderCell,
  GridCell,
  type GridRow,
} from '../../widgets/grid';
import type {FtraceStat} from './common';
import {Icons} from '../../base/semantic_icons';
import {ExportButton, type ExportFormat} from '../../widgets/export_button';
import {
  formatAsTSV,
  formatAsJSON,
  formatAsMarkdown,
} from '../../base/export_formatters';
import {MenuItem} from '../../widgets/menu';

const ROW_H = 24;

// Describes how the explorer scopes events by cpu, which differs based on
// whether the explorer is used as part of an area selection, or as a standalne
// tab.
export type FtraceCpuFilter =
  | {
      // Show only these ucpus. Used by the area selection tab, where the cpus
      // come from the selected tracks.
      readonly kind: 'fixed';
      readonly show: ReadonlyArray<number>;
    }
  | {
      // Show only these ucpus, where the set can be updated using a selector.
      // Used by the standalone ftrace tab.
      readonly kind: 'selectable';
      readonly show: ReadonlyArray<number>;
      readonly onChange: (show: ReadonlyArray<number>) => void;
    };

interface FtraceExplorerAttrs {
  readonly trace: Trace;
  readonly cache: FtraceExplorerCache;
  // All cpus with ftrace data, including multi-machine information.
  readonly cpus: ReadonlyArray<Cpu>;
  // Optional time bounds. If unset, the current viewport is used.
  readonly bounds?: {readonly start: time; readonly end: time};
  // The excluded event names and a callback to change them.
  readonly excludeList: ReadonlyArray<string>;
  readonly onExcludeListChange: (excludeList: ReadonlyArray<string>) => void;
  // How events are scoped by cpu, and whether the scope is settable.
  readonly cpuFilter: FtraceCpuFilter;
}

interface FtraceEvent {
  readonly id: number;
  readonly ts: time;
  readonly name: string;
  readonly cpu: number;
  readonly ucpu: number;
  readonly thread: string | null;
  readonly process: string | null;
  readonly args: string;
}

interface FtracePanelData {
  readonly events: FtraceEvent[];
  readonly offset: number;
}

interface Pagination {
  readonly offset: number;
  readonly count: number;
}

export interface FtraceExplorerCache {
  state: 'blank' | 'loading' | 'valid';
  counters: FtraceStat[];
}

// The set of filters applied to the ftrace_event query.
interface FtraceEventFilters {
  readonly excludeEvents: ReadonlyArray<string>;
  readonly includeUcpus: ReadonlyArray<number>;
}

async function getFtraceCounters(engine: Engine): Promise<FtraceStat[]> {
  // TODO(stevegolton): this is an extraordinarily slow query on large traces
  // as it goes through every ftrace event which can be a lot on big traces.
  // Consider if we can have some different UX which avoids needing these
  // counts
  const result = await engine.query(`
    select
      name,
      count(1) as cnt
    from ftrace_event
    group by name
    order by cnt desc
  `);
  const counters: FtraceStat[] = [];
  const it = result.iter({name: STR, cnt: NUM});
  for (let row = 0; it.valid(); it.next(), row++) {
    counters.push({name: it.name, count: it.cnt});
  }
  return counters;
}

export class FtraceExplorer implements m.ClassComponent<FtraceExplorerAttrs> {
  private readonly trace: Trace;
  private pagination: Pagination = {
    offset: 0,
    count: 0,
  };

  // Query slots for declarative data fetching
  private readonly executor = new AtomicTaskQueue();
  private readonly countSlot = new AsyncMemo<number>(this.executor);
  private readonly eventsSlot = new AsyncMemo<FtracePanelData>(this.executor);

  constructor({attrs}: m.CVnode<FtraceExplorerAttrs>) {
    this.trace = attrs.trace;

    if (attrs.cache.state === 'blank') {
      getFtraceCounters(attrs.trace.engine)
        .then((counters) => {
          attrs.cache.counters = counters;
          attrs.cache.state = 'valid';
        })
        .catch(() => {
          attrs.cache.state = 'blank';
        });
      attrs.cache.state = 'loading';
    }
  }

  onremove() {
    this.countSlot.dispose();
    this.eventsSlot.dispose();
  }

  view({attrs}: m.CVnode<FtraceExplorerAttrs>) {
    const {start, end} =
      attrs.bounds ?? attrs.trace.timeline.visibleWindow.toTimeSpan();
    const filters: FtraceEventFilters = {
      excludeEvents: attrs.excludeList,
      includeUcpus: attrs.cpuFilter.show,
    };
    const cpuByUcpu = new Map(attrs.cpus.map((c) => [c.ucpu, c]));
    const pagination = this.pagination;
    const engine = attrs.trace.engine;

    // Count query - always fresh (no staleOn)
    const {data: numEvents} = this.countSlot.use({
      key: {viewport: {start, end}, filters},
      retainOn: ['viewport'],
      compute: () => fetchFtraceEventCount(engine, start, end, filters),
    });

    // Events query - stale on pagination for smooth scrolling
    const {data} = this.eventsSlot.use({
      key: {viewport: {start, end}, filters, pagination},
      retainOn: ['pagination', 'viewport'],
      compute: () =>
        fetchFtraceEvents(
          engine,
          pagination.offset,
          pagination.count,
          start,
          end,
          filters,
        ),
    });

    const columns: GridColumn[] = [
      {key: 'id', header: m(GridHeaderCell, 'ID')},
      {key: 'timestamp', header: m(GridHeaderCell, 'Timestamp')},
      {key: 'name', header: m(GridHeaderCell, 'Name')},
      {key: 'cpu', header: m(GridHeaderCell, 'CPU')},
      {key: 'process', header: m(GridHeaderCell, 'Process')},
      {
        key: 'args',
        header: m(GridHeaderCell, 'Args'),
        maxInitialWidthPx: Infinity,
      },
    ];

    return m(
      DetailsShell,
      {
        title: `Ftrace Events (${numEvents ?? '...'})`,
        buttons: this.renderFilterPanel(attrs),
        fillHeight: true,
      },
      m(Grid, {
        className: 'pf-ftrace-explorer',
        columns,
        rowData: {
          data: this.renderData(data, cpuByUcpu),
          total: numEvents ?? 0,
          offset: data?.offset ?? 0,
          onLoadData: (offset, count) => {
            this.pagination = {offset, count};
          },
        },
        virtualization: {
          rowHeightPx: ROW_H,
        },
        fillHeight: true,
        onRowHover: (rowIndex) => {
          // Calculate the actual row index from virtualization offset
          const actualIndex = rowIndex - (data?.offset ?? 0);
          const event = data?.events[actualIndex];
          if (event) {
            attrs.trace.timeline.hoverCursorTimestamp = event.ts;
          }
        },
        onRowOut: () => {
          attrs.trace.timeline.hoverCursorTimestamp = undefined;
        },
      }),
    );
  }

  private renderData(
    data: FtracePanelData | undefined,
    cpuByUcpu: ReadonlyMap<number, Cpu>,
  ): ReadonlyArray<GridRow> {
    if (!data) {
      return [];
    }

    return data.events.map((event) => {
      const {ts, name, cpu, ucpu, process, args, id} = event;
      const color = materialColorScheme(name).base.cssString;
      const cpuLabel = cpuByUcpu.get(ucpu)?.toString() ?? String(cpu);

      return [
        m(GridCell, {align: 'right'}, id),
        m(
          GridCell,
          {
            menuItems: m(MenuItem, {
              label: 'Go to event on timeline',
              icon: Icons.UpdateSelection,
              onclick: () => {
                this.trace.selection.selectSqlEvent('ftrace_event', id, {
                  scrollToSelection: true,
                  switchToCurrentSelectionTab: true,
                });
              },
            }),
          },
          m(Timestamp, {trace: this.trace, ts}),
        ),
        m(
          GridCell,
          m(
            '.pf-ftrace-namebox',
            m('.pf-ftrace-colorbox', {style: {background: color}}),
            name,
          ),
        ),
        m(GridCell, cpuLabel),
        m(GridCell, process ?? ''),
        m(GridCell, args),
      ];
    });
  }

  private renderFilterPanel(attrs: FtraceExplorerAttrs) {
    const {cpuFilter} = attrs;

    if (attrs.cache.state !== 'valid') {
      return [m(Button, {label: 'Events', disabled: true, loading: true})];
    }

    // If the cpu set is settable, render the selector.
    const cpuFilterButton =
      cpuFilter.kind === 'selectable'
        ? m(PopupMultiSelect, {
            label: 'CPU',
            icon: 'memory',
            position: PopupPosition.Top,
            options: attrs.cpus.map((cpu) => ({
              id: String(cpu.ucpu),
              name: cpu.toString(),
              checked: cpuFilter.show.includes(cpu.ucpu),
            })),
            onChange: (diffs: MultiSelectDiff[]) => {
              const next = new Set<number>(cpuFilter.show);
              diffs.forEach(({checked, id}) => {
                const ucpu = Number(id);
                if (checked) {
                  next.add(ucpu);
                } else {
                  next.delete(ucpu);
                }
              });
              cpuFilter.onChange(Array.from(next));
            },
          })
        : undefined;

    const eventOptions: MultiSelectOption[] = attrs.cache.counters.map(
      ({name, count}) => ({
        id: name,
        name: `${name} (${count})`,
        checked: !attrs.excludeList.some((excluded) => excluded === name),
      }),
    );

    const eventFilterButton = m(PopupMultiSelect, {
      label: 'Events',
      icon: Icons.Filter,
      position: PopupPosition.Top,
      options: eventOptions,
      onChange: (diffs: MultiSelectDiff[]) => {
        const next = new Set<string>(attrs.excludeList);
        diffs.forEach(({checked, id}) => {
          if (checked) {
            next.delete(id);
          } else {
            next.add(id);
          }
        });
        attrs.onExcludeListChange(Array.from(next));
      },
    });

    const onExportData = async (format: ExportFormat): Promise<string> => {
      const {start, end} =
        attrs.bounds ?? attrs.trace.timeline.visibleWindow.toTimeSpan();
      const filters: FtraceEventFilters = {
        excludeEvents: attrs.excludeList,
        includeUcpus: cpuFilter.show,
      };
      const events = await fetchAllFtraceEvents(
        attrs.trace.engine,
        start,
        end,
        filters,
      );
      const columns = ['id', 'ts', 'name', 'cpu', 'process', 'args'];
      const columnNames: Record<string, string> = {
        id: 'ID',
        ts: 'Timestamp',
        name: 'Name',
        cpu: 'CPU',
        process: 'Process',
        args: 'Args',
      };
      const rows = events.map((e: FtraceEvent) => ({
        id: String(e.id),
        ts: String(e.ts),
        name: e.name,
        cpu: String(e.cpu),
        process: e.process ?? '',
        args: e.args,
      }));
      if (format === 'tsv') return formatAsTSV(columns, columnNames, rows);
      if (format === 'json') return formatAsJSON(columns, columnNames, rows);
      return formatAsMarkdown(columns, columnNames, rows);
    };

    return [
      cpuFilterButton,
      eventFilterButton,
      m(ExportButton, {onExportData}),
    ];
  }
}

// Builds the shared WHERE clause for the ftrace_event queries.
function ftraceWhere(
  filters: FtraceEventFilters,
  start: time,
  end: time,
): string {
  const excludeListSql = filters.excludeEvents.map((s) => `'${s}'`).join(',');
  // An empty inclusion list must match no rows; `ucpu in (null)` does that.
  const includeSql = filters.includeUcpus.join(',') || 'null';
  return [
    `ftrace_event.name not in (${excludeListSql})`,
    `ftrace_event.ucpu in (${includeSql})`,
    `ts >= ${start} and ts <= ${end}`,
  ].join(' and ');
}

async function fetchFtraceEventCount(
  engine: Engine,
  start: time,
  end: time,
  filters: FtraceEventFilters,
): Promise<number> {
  const queryRes = await engine.query(`
    select count(id) as numEvents
    from ftrace_event
    where ${ftraceWhere(filters, start, end)}
  `);
  return queryRes.firstRow({numEvents: NUM}).numEvents;
}

async function queryFtraceEvents(
  engine: Engine,
  start: time,
  end: time,
  filters: FtraceEventFilters,
  pagination?: {offset: number; count: number},
): Promise<FtraceEvent[]> {
  const limitClause = pagination
    ? `limit ${pagination.count} offset ${pagination.offset}`
    : '';

  const queryRes = await engine.query(`
    select
      ftrace_event.id as id,
      ftrace_event.ts as ts,
      ftrace_event.name as name,
      ftrace_event.cpu as cpu,
      ftrace_event.ucpu as ucpu,
      thread.name as thread,
      process.name as process,
      to_ftrace(ftrace_event.id) as args
    from ftrace_event
    join thread using (utid)
    left join process on thread.upid = process.upid
    where ${ftraceWhere(filters, start, end)}
    order by id
    ${limitClause};`);
  const events: FtraceEvent[] = [];
  const it = queryRes.iter({
    id: NUM,
    ts: LONG,
    name: STR,
    cpu: NUM,
    ucpu: NUM,
    thread: STR_NULL,
    process: STR_NULL,
    args: STR,
  });
  for (; it.valid(); it.next()) {
    events.push({
      id: it.id,
      ts: Time.fromRaw(it.ts),
      name: it.name,
      cpu: it.cpu,
      ucpu: it.ucpu,
      thread: it.thread,
      process: it.process,
      args: it.args,
    });
  }
  return events;
}

async function fetchFtraceEvents(
  engine: Engine,
  offset: number,
  count: number,
  start: time,
  end: time,
  filters: FtraceEventFilters,
): Promise<FtracePanelData> {
  const events = await queryFtraceEvents(engine, start, end, filters, {
    offset,
    count,
  });
  return {events, offset};
}

async function fetchAllFtraceEvents(
  engine: Engine,
  start: time,
  end: time,
  filters: FtraceEventFilters,
): Promise<FtraceEvent[]> {
  return queryFtraceEvents(engine, start, end, filters);
}

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
import {AsyncLimiter} from '../../base/async_limiter';
import {Monitor} from '../../base/monitor';
import {Store} from '../../base/store';
import {time, Time} from '../../base/time';
import {materialColorScheme} from '../../components/colorizer';
import {Timestamp} from '../../components/widgets/timestamp';
import {Trace} from '../../public/trace';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {
  MultiSelectDiff,
  MultiSelectOption,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {PopupPosition} from '../../widgets/popup';
import {
  Grid,
  GridColumn,
  GridHeaderCell,
  GridCell,
  GridRow,
} from '../../widgets/grid';
import {FtraceFilter, FtraceStat} from './common';
import {Icons} from '../../base/semantic_icons';

const ROW_H = 24;

interface FtraceExplorerAttrs {
  readonly cache: FtraceExplorerCache;
  readonly filterStore: Store<FtraceFilter>;
  readonly trace: Trace;
}

interface FtraceEvent {
  readonly id: number;
  readonly ts: time;
  readonly name: string;
  readonly cpu: number;
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

async function getFtraceCounters(engine: Engine): Promise<FtraceStat[]> {
  // TODO(stevegolton): this is an extraordinarily slow query on large traces
  // as it goes through every ftrace event which can be a lot on big traces.
  // Consider if we can have some different UX which avoids needing these
  // counts
  // TODO(mayzner): the +name below is an awful hack to workaround
  // extraordinarily slow sorting of strings. However, even with this hack,
  // this is just a slow query. There are various ways we can improve this
  // (e.g. with using the vtab_distinct APIs of SQLite).
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
  private readonly filterAndWindowStateMonitor: Monitor;
  private readonly queryLimiter = new AsyncLimiter();

  // A cache of the data we have most recently loaded from our store
  private data?: FtracePanelData;
  private numEvents: number = 0;

  constructor({attrs}: m.CVnode<FtraceExplorerAttrs>) {
    this.trace = attrs.trace;
    this.filterAndWindowStateMonitor = new Monitor([
      () => attrs.trace.timeline.visibleWindow.toTimeSpan().start,
      () => attrs.trace.timeline.visibleWindow.toTimeSpan().end,
      () => attrs.filterStore.state,
    ]);

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

  view({attrs}: m.CVnode<FtraceExplorerAttrs>) {
    this.filterAndWindowStateMonitor.ifStateChanged(() => {
      this.scheduleDataReload(attrs, true);
    });

    const columns: GridColumn[] = [
      {key: 'id', header: m(GridHeaderCell, 'ID')},
      {key: 'timestamp', header: m(GridHeaderCell, 'Timestamp')},
      {key: 'name', header: m(GridHeaderCell, 'Name')},
      {key: 'cpu', header: m(GridHeaderCell, 'CPU')},
      {key: 'process', header: m(GridHeaderCell, 'Process')},
      {key: 'args', header: m(GridHeaderCell, 'Args')},
    ];

    return m(
      DetailsShell,
      {
        title: this.renderTitle(),
        buttons: this.renderFilterPanel(attrs),
        fillHeight: true,
      },
      m(Grid, {
        className: 'pf-ftrace-explorer',
        columns,
        rowData: {
          data: this.renderData(),
          total: this.numEvents,
          offset: this.data?.offset ?? 0,
          onLoadData: (offset, count) => {
            this.pagination = {offset, count};
            this.scheduleDataReload(attrs, false);
          },
        },
        virtualization: {
          rowHeightPx: ROW_H,
        },
        fillHeight: true,
        onRowHover: (rowIndex) => {
          // Calculate the actual row index from virtualization offset
          const actualIndex = rowIndex - (this.data?.offset ?? 0);
          const event = this.data?.events[actualIndex];
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

  private scheduleDataReload(
    {filterStore, trace}: FtraceExplorerAttrs,
    filterOrTimeWindowChanged: boolean,
  ): void {
    const {offset, count} = this.pagination;
    const {start, end} = trace.timeline.visibleWindow.toTimeSpan();
    const excludeList = filterStore.state.excludeList;

    this.queryLimiter.schedule(async () => {
      if (filterOrTimeWindowChanged) {
        this.numEvents = await fetchFtraceEventCount(
          trace.engine,
          start,
          end,
          excludeList,
        );
      }
      this.data = await fetchFtraceEvents(
        trace.engine,
        offset,
        count,
        start,
        end,
        excludeList,
      );
    });
  }

  private renderData(): ReadonlyArray<GridRow> {
    if (!this.data) {
      return [];
    }

    return this.data.events.map((event) => {
      const {ts, name, cpu, process, args, id} = event;
      const color = materialColorScheme(name).base.cssString;

      return [
        m(GridCell, {align: 'right'}, id),
        m(GridCell, m(Timestamp, {trace: this.trace, ts})),
        m(
          GridCell,
          m(
            '.pf-ftrace-namebox',
            m('.pf-ftrace-colorbox', {style: {background: color}}),
            name,
          ),
        ),
        m(GridCell, {align: 'right'}, cpu),
        m(GridCell, process ?? ''),
        m(GridCell, args),
      ];
    });
  }

  private renderTitle() {
    if (this.data) {
      return `Ftrace Events (${this.numEvents})`;
    } else {
      return 'Ftrace Events';
    }
  }

  private renderFilterPanel(attrs: FtraceExplorerAttrs) {
    if (attrs.cache.state !== 'valid') {
      return m(Button, {
        label: 'Filter',
        disabled: true,
        loading: true,
      });
    }

    const excludeList = attrs.filterStore.state.excludeList;
    const options: MultiSelectOption[] = attrs.cache.counters.map(
      ({name, count}) => {
        return {
          id: name,
          name: `${name} (${count})`,
          checked: !excludeList.some((excluded: string) => excluded === name),
        };
      },
    );

    return m(PopupMultiSelect, {
      label: 'Filter',
      icon: Icons.Filter,
      position: PopupPosition.Top,
      options,
      onChange: (diffs: MultiSelectDiff[]) => {
        const newList = new Set<string>(excludeList);
        diffs.forEach(({checked, id}) => {
          if (checked) {
            newList.delete(id);
          } else {
            newList.add(id);
          }
        });
        attrs.filterStore.edit((draft) => {
          draft.excludeList = Array.from(newList);
        });
      },
    });
  }
}

async function fetchFtraceEventCount(
  engine: Engine,
  start: time,
  end: time,
  excludeList: ReadonlyArray<string>,
): Promise<number> {
  const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');

  const queryRes = await engine.query(`
    select count(id) as numEvents
    from ftrace_event
    where
      ftrace_event.name not in (${excludeListSql}) and
      ts >= ${start} and ts <= ${end}
    `);
  return queryRes.firstRow({numEvents: NUM}).numEvents;
}

async function fetchFtraceEvents(
  engine: Engine,
  offset: number,
  count: number,
  start: time,
  end: time,
  excludeList: ReadonlyArray<string>,
): Promise<FtracePanelData> {
  const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');

  const queryRes = await engine.query(`
    select
      ftrace_event.id as id,
      ftrace_event.ts as ts,
      ftrace_event.name as name,
      ftrace_event.cpu as cpu,
      thread.name as thread,
      process.name as process,
      to_ftrace(ftrace_event.id) as args
    from ftrace_event
    join thread using (utid)
    left join process on thread.upid = process.upid
    where
      ftrace_event.name not in (${excludeListSql}) and
      ts >= ${start} and ts <= ${end}
    order by id
    limit ${count} offset ${offset};`);
  const events: FtraceEvent[] = [];
  const it = queryRes.iter({
    id: NUM,
    ts: LONG,
    name: STR,
    cpu: NUM,
    thread: STR_NULL,
    process: STR_NULL,
    args: STR,
  });
  for (let row = 0; it.valid(); it.next(), row++) {
    events.push({
      id: it.id,
      ts: Time.fromRaw(it.ts),
      name: it.name,
      cpu: it.cpu,
      thread: it.thread,
      process: it.process,
      args: it.args,
    });
  }
  return {events, offset};
}

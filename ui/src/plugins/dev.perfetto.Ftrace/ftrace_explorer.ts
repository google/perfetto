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
import {time, Time} from '../../base/time';
import {colorForFtrace} from '../../public/lib/colorizer';
import {DetailsShell} from '../../widgets/details_shell';
import {
  MultiSelectDiff,
  Option as MultiSelectOption,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {PopupPosition} from '../../widgets/popup';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {FtraceFilter, FtraceStat} from './common';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {AsyncLimiter} from '../../base/async_limiter';
import {Monitor} from '../../base/monitor';
import {Button} from '../../widgets/button';
import {VirtualTable, VirtualTableRow} from '../../widgets/virtual_table';
import {Store} from '../../base/store';
import {Trace} from '../../public/trace';

const ROW_H = 20;

interface FtraceExplorerAttrs {
  cache: FtraceExplorerCache;
  filterStore: Store<FtraceFilter>;
  trace: Trace;
}

interface FtraceEvent {
  id: number;
  ts: time;
  name: string;
  cpu: number;
  thread: string | null;
  process: string | null;
  args: string;
}

interface FtracePanelData {
  events: FtraceEvent[];
  offset: number;
  numEvents: number; // Number of events in the visible window
}

interface Pagination {
  offset: number;
  count: number;
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
  private pagination: Pagination = {
    offset: 0,
    count: 0,
  };
  private readonly monitor: Monitor;
  private readonly queryLimiter = new AsyncLimiter();

  // A cache of the data we have most recently loaded from our store
  private data?: FtracePanelData;

  constructor({attrs}: m.CVnode<FtraceExplorerAttrs>) {
    this.monitor = new Monitor([
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
    this.monitor.ifStateChanged(() => {
      this.reloadData(attrs);
    });

    return m(
      DetailsShell,
      {
        title: this.renderTitle(),
        buttons: this.renderFilterPanel(attrs),
        fillParent: true,
      },
      m(VirtualTable, {
        className: 'pf-ftrace-explorer',
        columns: [
          {header: 'ID', width: '5em'},
          {header: 'Timestamp', width: '13em'},
          {header: 'Name', width: '24em'},
          {header: 'CPU', width: '3em'},
          {header: 'Process', width: '24em'},
          {header: 'Args', width: '200em'},
        ],
        firstRowOffset: this.data?.offset ?? 0,
        numRows: this.data?.numEvents ?? 0,
        rowHeight: ROW_H,
        rows: this.renderData(),
        onReload: (offset, count) => {
          this.pagination = {offset, count};
          this.reloadData(attrs);
        },
        onRowHover: (id) => {
          const event = this.data?.events.find((event) => event.id === id);
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

  private reloadData(attrs: FtraceExplorerAttrs): void {
    this.queryLimiter.schedule(async () => {
      this.data = await lookupFtraceEvents(
        attrs.trace,
        this.pagination.offset,
        this.pagination.count,
        attrs.filterStore.state,
      );
      attrs.trace.scheduleFullRedraw();
    });
  }

  private renderData(): VirtualTableRow[] {
    if (!this.data) {
      return [];
    }

    return this.data.events.map((event) => {
      const {ts, name, cpu, process, args, id} = event;
      const timestamp = m(Timestamp, {ts});
      const color = colorForFtrace(name).base.cssString;

      return {
        id,
        cells: [
          id,
          timestamp,
          m(
            '.pf-ftrace-namebox',
            m('.pf-ftrace-colorbox', {style: {background: color}}),
            name,
          ),
          cpu,
          process,
          args,
        ],
      };
    });
  }

  private renderTitle() {
    if (this.data) {
      const {numEvents} = this.data;
      return `Ftrace Events (${numEvents})`;
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
      icon: 'filter_list_alt',
      popupPosition: PopupPosition.Top,
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

async function lookupFtraceEvents(
  trace: Trace,
  offset: number,
  count: number,
  filter: FtraceFilter,
): Promise<FtracePanelData> {
  const {start, end} = trace.timeline.visibleWindow.toTimeSpan();

  const excludeList = filter.excludeList;
  const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');

  // TODO(stevegolton): This query can be slow when traces are huge.
  // The number of events is only used for correctly sizing the panel's
  // scroll container so that the scrollbar works as if the panel were fully
  // populated.
  // Perhaps we could work out some UX that doesn't need this.
  let queryRes = await trace.engine.query(`
    select count(id) as numEvents
    from ftrace_event
    where
      ftrace_event.name not in (${excludeListSql}) and
      ts >= ${start} and ts <= ${end}
    `);
  const {numEvents} = queryRes.firstRow({numEvents: NUM});

  queryRes = await trace.engine.query(`
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
  return {events, offset, numEvents};
}

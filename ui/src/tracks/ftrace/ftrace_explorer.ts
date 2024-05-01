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
import {Actions} from '../../common/actions';
import {colorForFtrace} from '../../core/colorizer';
import {DetailsShell} from '../../widgets/details_shell';
import {
  MultiSelectDiff,
  Option as MultiSelectOption,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {PopupPosition} from '../../widgets/popup';
import {VirtualScrollContainer} from '../../widgets/virtual_scroll_container';

import {globals} from '../../frontend/globals';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {FtraceFilter, FtraceStat} from './common';
import {
  createStore,
  EngineProxy,
  LONG,
  NUM,
  Store,
  STR,
  STR_NULL,
} from '../../public';
import {raf} from '../../core/raf_scheduler';
import {AsyncLimiter} from '../../base/async_limiter';
import {Monitor} from '../../base/monitor';
import {Button} from '../../widgets/button';

const ROW_H = 20;
const PAGE_SIZE = 250;

interface FtraceExplorerAttrs {
  cache: FtraceExplorerCache;
  filterStore: Store<FtraceFilter>;
  engine: EngineProxy;
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
  page: number;
  pageCount: number;
}

export interface FtraceExplorerCache {
  state: 'blank' | 'loading' | 'valid';
  counters: FtraceStat[];
}

async function getFtraceCounters(engine: EngineProxy): Promise<FtraceStat[]> {
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
  private readonly paginationStore = createStore<Pagination>({
    page: 0,
    pageCount: 0,
  });
  private readonly monitor: Monitor;
  private readonly queryLimiter = new AsyncLimiter();

  // A cache of the data we have most recently loaded from our store
  private data?: FtracePanelData;

  constructor({attrs}: m.CVnode<FtraceExplorerAttrs>) {
    this.monitor = new Monitor([
      () => globals.state.frontendLocalState.visibleState.start,
      () => globals.state.frontendLocalState.visibleState.end,
      () => attrs.filterStore.state,
      () => this.paginationStore.state,
    ]);

    if (attrs.cache.state === 'blank') {
      getFtraceCounters(attrs.engine)
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
    this.monitor.ifStateChanged(() =>
      this.queryLimiter.schedule(async () => {
        this.data = await lookupFtraceEvents(
          attrs.engine,
          this.paginationStore.state.page * PAGE_SIZE,
          this.paginationStore.state.pageCount * PAGE_SIZE,
          attrs.filterStore.state,
        );
        raf.scheduleFullRedraw();
      }),
    );

    return m(
      DetailsShell,
      {
        title: this.renderTitle(),
        buttons: this.renderFilterPanel(attrs),
      },
      m(
        VirtualScrollContainer,
        {
          onScroll: this.onScroll.bind(this),
        },
        m('.ftrace-panel', this.renderRows()),
      ),
    );
  }

  onScroll(scrollContainer: HTMLElement) {
    const paginationState = this.paginationStore.state;
    const prevPage = paginationState.page;
    const prevPageCount = paginationState.pageCount;

    const visibleRowOffset = Math.floor(scrollContainer.scrollTop / ROW_H);
    const visibleRowCount = Math.ceil(scrollContainer.clientHeight / ROW_H);

    // Work out which "page" we're on
    const page = Math.max(0, Math.floor(visibleRowOffset / PAGE_SIZE) - 1);
    const pageCount = Math.ceil(visibleRowCount / PAGE_SIZE) + 2;

    if (page !== prevPage || pageCount !== prevPageCount) {
      this.paginationStore.edit((draft) => {
        draft.page = page;
        draft.pageCount = pageCount;
      });
      raf.scheduleFullRedraw();
    }
  }

  onRowOver(ts: time) {
    globals.dispatch(Actions.setHoverCursorTimestamp({ts}));
  }

  onRowOut() {
    globals.dispatch(Actions.setHoverCursorTimestamp({ts: Time.INVALID}));
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

  // Render all the rows including the first title row
  private renderRows() {
    const data = this.data;
    const rows: m.Children = [];

    rows.push(
      m(
        `.row`,
        m('.cell.row-header', 'Timestamp'),
        m('.cell.row-header', 'Name'),
        m('.cell.row-header', 'CPU'),
        m('.cell.row-header', 'Process'),
        m('.cell.row-header', 'Args'),
      ),
    );

    if (data) {
      const {events, offset, numEvents} = data;
      for (let i = 0; i < events.length; i++) {
        const {ts, name, cpu, process, args} = events[i];

        const timestamp = m(Timestamp, {ts});

        const rank = i + offset;

        const color = colorForFtrace(name).base.cssString;

        rows.push(
          m(
            `.row`,
            {
              style: {top: `${(rank + 1.0) * ROW_H}px`},
              onmouseover: this.onRowOver.bind(this, ts),
              onmouseout: this.onRowOut.bind(this),
            },
            m('.cell', timestamp),
            m('.cell', m('span.colour', {style: {background: color}}), name),
            m('.cell', cpu),
            m('.cell', process),
            m('.cell', args),
          ),
        );
      }
      return m('.rows', {style: {height: `${numEvents * ROW_H}px`}}, rows);
    } else {
      return m('.rows', rows);
    }
  }
}

async function lookupFtraceEvents(
  engine: EngineProxy,
  offset: number,
  count: number,
  filter: FtraceFilter,
): Promise<FtracePanelData> {
  const {start, end} = globals.stateVisibleTime();

  const excludeList = filter.excludeList;
  const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');

  // TODO(stevegolton): This query can be slow when traces are huge.
  // The number of events is only used for correctly sizing the panel's
  // scroll container so that the scrollbar works as if the panel were fully
  // populated.
  // Perhaps we could work out some UX that doesn't need this.
  let queryRes = await engine.query(`
    select count(id) as numEvents
    from ftrace_event
    where
      ftrace_event.name not in (${excludeListSql}) and
      ts >= ${start} and ts <= ${end}
    `);
  const {numEvents} = queryRes.firstRow({numEvents: NUM});

  queryRes = await engine.query(`
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

// Copyright (C) 2023 The Android Open Source Project
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

import {Span, Time} from '../base/time';
import {Engine} from '../common/engine';
import {
  HighPrecisionTime,
  HighPrecisionTimeSpan,
} from '../common/high_precision_time';
import {LONG, NUM, STR, STR_NULL} from '../common/query_result';
import {FtraceFilterState, Pagination} from '../common/state';
import {FtraceEvent, globals} from '../frontend/globals';
import {publishFtracePanelData} from '../frontend/publish';
import {ratelimit} from '../frontend/rate_limiters';

import {Controller} from './controller';

export interface FtraceControllerArgs {
  engine: Engine;
}

interface RetVal {
  events: FtraceEvent[];
  offset: number;
  numEvents: number;
}

export class FtraceController extends Controller<'main'> {
  private engine: Engine;
  private oldSpan: Span<HighPrecisionTime> = HighPrecisionTimeSpan.ZERO;
  private oldFtraceFilter?: FtraceFilterState;
  private oldPagination?: Pagination;

  constructor({engine}: FtraceControllerArgs) {
    super('main');
    this.engine = engine;
  }

  run() {
    if (this.shouldUpdate()) {
      this.oldSpan = globals.frontendLocalState.visibleWindowTime;
      this.oldFtraceFilter = globals.state.ftraceFilter;
      this.oldPagination = globals.state.ftracePagination;
      if (globals.state.ftracePagination.count > 0) {
        this.lookupFtraceEventsRateLimited();
      }
    }
  }

  private lookupFtraceEventsRateLimited = ratelimit(() => {
    const {offset, count} = globals.state.ftracePagination;
    // The formatter doesn't like formatted chained methods :(
    const promise = this.lookupFtraceEvents(offset, count);
    promise.then(({events, offset, numEvents}: RetVal) => {
      publishFtracePanelData({events, offset, numEvents});
    });
  }, 250);

  private shouldUpdate(): boolean {
    // Has the visible window moved?
    const visibleWindow = globals.frontendLocalState.visibleWindowTime;
    if (!this.oldSpan.equals(visibleWindow)) {
      return true;
    }

    // Has the pagination changed?
    if (this.oldPagination !== globals.state.ftracePagination) {
      return true;
    }

    // Has the filter changed?
    if (this.oldFtraceFilter !== globals.state.ftraceFilter) {
      return true;
    }

    return false;
  }

  async lookupFtraceEvents(offset: number, count: number): Promise<RetVal> {
    const appState = globals.state;
    const {start, end} = globals.stateVisibleTime();

    const excludeList = appState.ftraceFilter.excludedNames;
    const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');

    // TODO(stevegolton): This query can be slow when traces are huge.
    // The number of events is only used for correctly sizing the panel's
    // scroll container so that the scrollbar works as if the panel were fully
    // populated.
    // Perhaps we could work out some UX that doesn't need this.
    let queryRes = await this.engine.query(`
      select count(id) as numEvents
      from ftrace_event
      where
        ftrace_event.name not in (${excludeListSql}) and
        ts >= ${start} and ts <= ${end}
      `);
    const {numEvents} = queryRes.firstRow({numEvents: NUM});

    queryRes = await this.engine.query(`
      select
        ftrace_event.id as id,
        ftrace_event.ts as ts,
        ftrace_event.name as name,
        ftrace_event.cpu as cpu,
        thread.name as thread,
        process.name as process,
        to_ftrace(ftrace_event.id) as args
      from ftrace_event
      left join thread
      on ftrace_event.utid = thread.utid
      left join process
      on thread.upid = process.upid
      where
        ftrace_event.name not in (${excludeListSql}) and
        ts >= ${start} and ts <= ${end}
      order by id
      limit ${count} offset ${offset};`);
    const events: FtraceEvent[] = [];
    const it = queryRes.iter(
        {
          id: NUM,
          ts: LONG,
          name: STR,
          cpu: NUM,
          thread: STR_NULL,
          process: STR_NULL,
          args: STR,
        },
    );
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
};

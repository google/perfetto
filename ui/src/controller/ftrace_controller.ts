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

import {FtraceFilterState} from 'src/common/state';

import {Engine} from '../common/engine';
import {NUM, STR, STR_NULL} from '../common/query_result';
import {TimeSpan, toNsCeil, toNsFloor} from '../common/time';
import {FtraceEvent, globals as frontendGlobals} from '../frontend/globals';
import {globals} from '../frontend/globals';
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

function cloneFtraceFilterState(other: FtraceFilterState): FtraceFilterState {
  return {
    excludedNames: [...other.excludedNames],
  };
}

function ftraceFilterStateEq(
    a: FtraceFilterState, b: FtraceFilterState): boolean {
  if (a.excludedNames === b.excludedNames) return true;
  if (a.excludedNames.length !== b.excludedNames.length) return false;

  for (let i = 0; i < a.excludedNames.length; ++i) {
    if (a.excludedNames[i] !== b.excludedNames[i]) return false;
  }

  return true;
}

export class FtraceController extends Controller<'main'> {
  private engine: Engine;
  private oldSpan: TimeSpan = new TimeSpan(0, 0);
  private oldFtraceFilter: FtraceFilterState = {
    excludedNames: [],
  };
  constructor({engine}: FtraceControllerArgs) {
    super('main');
    this.engine = engine;
  }

  run() {
    if (this.shouldUpdate()) {
      this.updateEverything();
    }
  }

  private updateEverything = ratelimit(() => {
    const {offset, count} = globals.state.ftracePagination;
    this.oldSpan = frontendGlobals.frontendLocalState.visibleWindowTime;
    this.oldFtraceFilter =
        cloneFtraceFilterState(frontendGlobals.state.ftraceFilter);
    this.lookupFtraceEvents(offset, count).then(({events,
                                                  offset,
                                                  numEvents}: RetVal) => {
      publishFtracePanelData({events, offset, numEvents});
    });
  }, 250);

  private shouldUpdate(): boolean {
    if (this.oldSpan != frontendGlobals.frontendLocalState.visibleWindowTime) {
      // The visible window has changed, definitely update
      return true;
    }

    const globalPanelData = frontendGlobals.ftracePanelData;
    if (!globalPanelData) {
      // No state has been written yet, so we definitely need to update
      return true;
    }

    // Work out whether we've scrolled near our rendered bounds
    const {offset, count} = globals.state.ftracePagination;
    if (offset != globalPanelData.offset ||
        count != globalPanelData.events.length) {
      return true;
    }

    // Work out of the ftrace filter has changed
    const filter = frontendGlobals.state.ftraceFilter;
    if (!ftraceFilterStateEq(this.oldFtraceFilter, filter)) {
      return true;
    }

    return false;
  }

  async lookupFtraceEvents(offset: number, count: number): Promise<RetVal> {
    const appState = frontendGlobals.state;
    const frontendState = frontendGlobals.frontendLocalState;
    const {start, end} = frontendState.visibleWindowTime;

    const startNs = toNsFloor(start);
    const endNs = toNsCeil(end);

    const excludeList = appState.ftraceFilter.excludedNames;
    const excludeListSql = excludeList.map((s) => `'${s}'`).join(',');

    let queryRes = await this.engine.query(`
      select count(id) as numEvents
      from ftrace_event
      where
        ftrace_event.name not in (${excludeListSql}) and
        ts >= ${startNs} and ts <= ${endNs}
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
        ts >= ${startNs} and ts <= ${endNs}
      order by id
      limit ${count} offset ${offset};`);
    const events: FtraceEvent[] = [];
    const it = queryRes.iter(
        {
          id: NUM,
          ts: NUM,
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
        ts: it.ts,
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

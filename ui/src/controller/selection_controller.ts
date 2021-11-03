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

import {assertTrue} from '../base/logging';
import {Arg, Args} from '../common/arg_types';
import {Engine} from '../common/engine';
import {
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../common/query_result';
import {ChromeSliceSelection} from '../common/state';
import {translateState} from '../common/thread_state';
import {fromNs, toNs} from '../common/time';
import {SliceDetails, ThreadStateDetails} from '../frontend/globals';
import {
  publishCounterDetails,
  publishSliceDetails,
  publishThreadStateDetails
} from '../frontend/publish';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices/common';

import {parseArgs} from './args_parser';
import {Controller} from './controller';
import {globals} from './globals';

export interface SelectionControllerArgs {
  engine: Engine;
}

// This class queries the TP for the details on a specific slice that has
// been clicked.
export class SelectionController extends Controller<'main'> {
  private lastSelectedId?: number|string;
  private lastSelectedKind?: string;
  constructor(private args: SelectionControllerArgs) {
    super('main');
  }

  run() {
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind === 'AREA') return;

    const selectWithId =
        ['SLICE', 'COUNTER', 'CHROME_SLICE', 'HEAP_PROFILE', 'THREAD_STATE'];
    if (!selectWithId.includes(selection.kind) ||
        (selectWithId.includes(selection.kind) &&
         selection.id === this.lastSelectedId &&
         selection.kind === this.lastSelectedKind)) {
      return;
    }
    const selectedId = selection.id;
    const selectedKind = selection.kind;
    this.lastSelectedId = selectedId;
    this.lastSelectedKind = selectedKind;

    if (selectedId === undefined) return;

    if (selection.kind === 'COUNTER') {
      this.counterDetails(selection.leftTs, selection.rightTs, selection.id)
          .then(results => {
            if (results !== undefined && selection &&
                selection.kind === selectedKind &&
                selection.id === selectedId) {
              publishCounterDetails(results);
            }
          });
    } else if (selection.kind === 'SLICE') {
      this.sliceDetails(selectedId as number);
    } else if (selection.kind === 'THREAD_STATE') {
      this.threadStateDetails(selection.id);
    } else if (selection.kind === 'CHROME_SLICE') {
      this.chromeSliceDetails(selection);
    }
  }

  async chromeSliceDetails(selection: ChromeSliceSelection) {
    const selectedId = selection.id;
    const table = selection.table;

    let leafTable: string;
    let promisedDescription: Promise<Map<string, string>>;
    let promisedArgs: Promise<Args>;
    // TODO(b/155483804): This is a hack to ensure annotation slices are
    // selectable for now. We should tidy this up when improving this class.
    if (table === 'annotation') {
      leafTable = 'annotation_slice';
      promisedDescription = Promise.resolve(new Map());
      promisedArgs = Promise.resolve(new Map());
    } else {
      const result = await this.args.engine.query(`
        SELECT
          type as leafTable,
          arg_set_id as argSetId
        FROM slice WHERE id = ${selectedId}`);

      if (result.numRows() === 0) {
        return;
      }

      const row = result.firstRow({
        leafTable: STR,
        argSetId: NUM,
      });

      leafTable = row.leafTable;
      const argSetId = row.argSetId;
      promisedDescription = this.describeSlice(selectedId);
      promisedArgs = this.getArgs(argSetId);
    }

    const promisedDetails = this.args.engine.query(`
      SELECT * FROM ${leafTable} WHERE id = ${selectedId};
    `);

    const [details, args, description] =
        await Promise.all([promisedDetails, promisedArgs, promisedDescription]);

    if (details.numRows() <= 0) return;
    const rowIter = details.iter({});
    assertTrue(rowIter.valid());

    // A few columns are hard coded as part of the SliceDetails interface.
    // Long term these should be handled generically as args but for now
    // handle them specially:
    let ts = undefined;
    let dur = undefined;
    let name = undefined;
    let category = undefined;

    for (const k of details.columns()) {
      const v = rowIter.get(k);
      switch (k) {
        case 'id':
          break;
        case 'ts':
          ts = fromNs(Number(v)) - globals.state.traceTime.startSec;
          break;
        case 'name':
          name = `${v}`;
          break;
        case 'dur':
          dur = fromNs(Number(v));
          break;
        case 'category':
        case 'cat':
          category = `${v}`;
          break;
        default:
          args.set(k, `${v}`);
      }
    }

    const argsTree = parseArgs(args);
    const selected: SliceDetails = {
      id: selectedId,
      ts,
      dur,
      name,
      category,
      args,
      argsTree,
      description,
    };

    // Check selection is still the same on completion of query.
    if (selection === globals.state.currentSelection) {
      publishSliceDetails(selected);
    }
  }

  async describeSlice(id: number): Promise<Map<string, string>> {
    const map = new Map<string, string>();
    if (id === -1) return map;
    const query = `
      select
        ifnull(description, '') as description,
        ifnull(doc_link, '') as docLink
      from describe_slice
      where slice_id = ${id}
    `;
    const result = await this.args.engine.query(query);
    const it = result.iter({description: STR, docLink: STR});
    for (; it.valid(); it.next()) {
      const description = it.description;
      const docLink = it.docLink;
      map.set('Description', description);
      map.set('Documentation', docLink);
    }
    return map;
  }

  async getArgs(argId: number): Promise<Args> {
    const args = new Map<string, Arg>();
    const query = `
      select
        key AS name,
        CAST(COALESCE(int_value, string_value, real_value) AS text) AS value
      FROM args
      WHERE arg_set_id = ${argId}
    `;
    const result = await this.args.engine.query(query);
    const it = result.iter({
      name: STR,
      value: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const name = it.name;
      const value = it.value || 'NULL';
      if (name === 'destination slice id' && !isNaN(Number(value))) {
        const destTrackId = await this.getDestTrackId(value);
        args.set(
            'Destination Slice',
            {kind: 'SLICE', trackId: destTrackId, sliceId: Number(value)});
      } else {
        args.set(name, value);
      }
    }
    return args;
  }

  async getDestTrackId(sliceId: string): Promise<string> {
    const trackIdQuery = `select track_id as trackId from slice
    where slice_id = ${sliceId}`;
    const result = await this.args.engine.query(trackIdQuery);
    const trackIdTp = result.firstRow({trackId: NUM}).trackId;
    // TODO(hjd): If we had a consistent mapping from TP track_id
    // UI track id for slice tracks this would be unnecessary.
    let trackId = '';
    for (const track of Object.values(globals.state.tracks)) {
      if (track.kind === SLICE_TRACK_KIND &&
          (track.config as {trackId: number}).trackId === Number(trackIdTp)) {
        trackId = track.id;
        break;
      }
    }
    return trackId;
  }

  async threadStateDetails(id: number) {
    const query = `
      SELECT
        ts,
        thread_state.dur as dur,
        state,
        io_wait as ioWait,
        thread_state.utid as utid,
        thread_state.cpu as cpu,
        sched.id as id,
        thread_state.blocked_function as blockedFunction
      from thread_state
      left join sched using(ts) where thread_state.id = ${id}
    `;
    const result = await this.args.engine.query(query);

    const selection = globals.state.currentSelection;
    if (result.numRows() > 0 && selection) {
      const row = result.firstRow({
        ts: NUM,
        dur: NUM,
        state: STR,
        ioWait: NUM_NULL,
        utid: NUM,
        cpu: NUM_NULL,
        id: NUM_NULL,
        blockedFunction: STR_NULL,
      });
      const ts = row.ts;
      const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
      const dur = fromNs(row.dur);
      const ioWait = row.ioWait === null ? undefined : row.ioWait > 0;
      const state = translateState(row.state, ioWait);
      const utid = row.utid;
      const cpu = row.cpu === null ? undefined : row.cpu;
      const sliceId = row.id === null ? undefined : row.id;
      const blockedFunction =
          row.blockedFunction === null ? undefined : row.blockedFunction;
      const selected: ThreadStateDetails =
          {ts: timeFromStart, dur, state, utid, cpu, sliceId, blockedFunction};
      publishThreadStateDetails(selected);
    }
  }

  async sliceDetails(id: number) {
    const sqlQuery = `SELECT
      ts,
      dur,
      priority,
      end_state as endState,
      utid,
      cpu,
      thread_state.id as threadStateId
    FROM sched join thread_state using(ts, utid, dur, cpu)
    WHERE sched.id = ${id}`;
    const result = await this.args.engine.query(sqlQuery);
    // Check selection is still the same on completion of query.
    const selection = globals.state.currentSelection;
    if (result.numRows() > 0 && selection) {
      const row = result.firstRow({
        ts: NUM,
        dur: NUM,
        priority: NUM,
        endState: STR,
        utid: NUM,
        cpu: NUM,
        threadStateId: NUM,
      });
      const ts = row.ts;
      const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
      const dur = fromNs(row.dur);
      const priority = row.priority;
      const endState = row.endState;
      const utid = row.utid;
      const cpu = row.cpu;
      const threadStateId = row.threadStateId;
      const selected: SliceDetails = {
        ts: timeFromStart,
        dur,
        priority,
        endState,
        cpu,
        id,
        utid,
        threadStateId
      };
      this.schedulingDetails(ts, utid)
          .then(wakeResult => {
            Object.assign(selected, wakeResult);
          })
          .finally(() => {
            publishSliceDetails(selected);
          });
    }
  }

  async counterDetails(ts: number, rightTs: number, id: number) {
    const counter = await this.args.engine.query(
        `SELECT value, track_id as trackId FROM counter WHERE id = ${id}`);
    const row = counter.iter({
      value: NUM,
      trackId: NUM,
    });
    const value = row.value;
    const trackId = row.trackId;
    // Finding previous value. If there isn't previous one, it will return 0 for
    // ts and value.
    const previous = await this.args.engine.query(`SELECT
          MAX(ts),
          IFNULL(value, 0) as value
        FROM counter WHERE ts < ${ts} and track_id = ${trackId}`);
    const previousValue = previous.firstRow({value: NUM}).value;
    const endTs =
        rightTs !== -1 ? rightTs : toNs(globals.state.traceTime.endSec);
    const delta = value - previousValue;
    const duration = endTs - ts;
    const startTime = fromNs(ts) - globals.state.traceTime.startSec;
    const name = globals.state.tracks[trackId].name;
    return {startTime, value, delta, duration, name};
  }

  async schedulingDetails(ts: number, utid: number|Long) {
    let event = 'sched_waking';
    const waking = await this.args.engine.query(
        `select * from instants where name = 'sched_waking' limit 1`);
    const wakeup = await this.args.engine.query(
        `select * from instants where name = 'sched_wakeup' limit 1`);
    if (waking.numRows() === 0) {
      if (wakeup.numRows() === 0) return undefined;
      // Only use sched_wakeup if waking is not in the trace.
      event = 'sched_wakeup';
    }

    // Find the ts of the first sched_wakeup before the current slice.
    const queryWakeupTs = `select ts from instants where name = '${event}'
    and ref = ${utid} and ts < ${ts} order by ts desc limit 1`;
    const wakeResult = await this.args.engine.query(queryWakeupTs);
    if (wakeResult.numRows() === 0) {
      return undefined;
    }
    const wakeupTs = wakeResult.firstRow({ts: NUM}).ts;

    // Find the previous sched slice for the current utid.
    const queryPrevSched = `select ts from sched where utid = ${utid}
    and ts < ${ts} order by ts desc limit 1`;
    const prevSchedResult = await this.args.engine.query(queryPrevSched);

    // If this is the first sched slice for this utid or if the wakeup found
    // was after the previous slice then we know the wakeup was for this slice.
    if (prevSchedResult.numRows() === 0 ||
        wakeupTs < prevSchedResult.firstRow({ts: NUM}).ts) {
      return undefined;
    }
    // Find the sched slice with the utid of the waker running when the
    // sched wakeup occurred. This is the waker.
    const queryWaker = `select utid, cpu from sched where utid =
    (select utid from raw where name = '${event}' and ts = ${wakeupTs})
    and ts < ${wakeupTs} and ts + dur >= ${wakeupTs};`;
    const wakerResult = await this.args.engine.query(queryWaker);
    if (wakerResult.numRows() === 0) {
      return undefined;
    }
    const wakerRow = wakerResult.firstRow({utid: NUM, cpu: NUM});
    return {
      wakeupTs: fromNs(wakeupTs),
      wakerUtid: wakerRow.utid,
      wakerCpu: wakerRow.cpu
    };
  }
}

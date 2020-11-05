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

import {Engine} from '../common/engine';
import {slowlyCountRows} from '../common/query_iterator';
import {translateState} from '../common/thread_state';
import {fromNs, toNs} from '../common/time';
import {
  Arg,
  Args,
  CounterDetails,
  SliceDetails,
  ThreadStateDetails
} from '../frontend/globals';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices/common';

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
      const selected: CounterDetails = {};
      this.counterDetails(selection.leftTs, selection.rightTs, selection.id)
          .then(results => {
            if (results !== undefined && selection &&
                selection.kind === selectedKind &&
                selection.id === selectedId) {
              Object.assign(selected, results);
              globals.publish('CounterDetails', selected);
            }
          });
    } else if (selection.kind === 'SLICE') {
      this.sliceDetails(selectedId as number);
    } else if (selection.kind === 'THREAD_STATE') {
      this.threadStateDetails(selection.id);
    } else if (selection.kind === 'CHROME_SLICE') {
      const table = selection.table;
      let sqlQuery = `
        SELECT ts, dur, name, cat, arg_set_id
        FROM slice
        WHERE id = ${selectedId}
      `;
      // TODO(b/155483804): This is a hack to ensure annotation slices are
      // selectable for now. We should tidy this up when improving this class.
      if (table === 'annotation') {
        sqlQuery = `
        select ts, dur, name, cat, -1
        from annotation_slice
        where id = ${selectedId}`;
      }
      this.args.engine.query(sqlQuery).then(result => {
        // Check selection is still the same on completion of query.
        const selection = globals.state.currentSelection;
        if (slowlyCountRows(result) === 1 && selection &&
            selection.kind === selectedKind && selection.id === selectedId) {
          const ts = result.columns[0].longValues![0];
          const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
          const name = result.columns[2].stringValues![0];
          const dur = fromNs(result.columns[1].longValues![0]);
          const category = result.columns[3].stringValues![0];
          const argId = result.columns[4].longValues![0];
          const argsAsync = this.getArgs(argId);
          // Don't fetch descriptions for annotation slices.
          const describeId = table === 'annotation' ? -1 : +selectedId;
          const descriptionAsync = this.describeSlice(describeId);
          Promise.all([argsAsync, descriptionAsync])
              .then(([args, description]) => {
                const selected: SliceDetails = {
                  ts: timeFromStart,
                  dur,
                  category,
                  name,
                  id: selectedId as number,
                  args,
                  description,
                };
                globals.publish('SliceDetails', selected);
              });
        }
      });
    }
  }

  async describeSlice(id: number): Promise<Map<string, string>> {
    const map = new Map<string, string>();
    if (id === -1) return map;
    const query = `
      select description, doc_link
      from describe_slice
      where slice_id = ${id}
    `;
    const result = await this.args.engine.query(query);
    for (let i = 0; i < slowlyCountRows(result); i++) {
      const description = result.columns[0].stringValues![i];
      const docLink = result.columns[1].stringValues![i];
      map.set('Description', description);
      map.set('Documentation', docLink);
    }
    return map;
  }

  async getArgs(argId: number): Promise<Args> {
    const args = new Map<string, Arg>();
    const query = `
      select
        flat_key AS name,
        CAST(COALESCE(int_value, string_value, real_value) AS text) AS value
      FROM args
      WHERE arg_set_id = ${argId}
    `;
    const result = await this.args.engine.query(query);
    for (let i = 0; i < slowlyCountRows(result); i++) {
      const name = result.columns[0].stringValues![i];
      const value = result.columns[1].stringValues![i];
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
    const trackIdQuery = `select track_id from slice
    where slice_id = ${sliceId}`;
    const destResult = await this.args.engine.query(trackIdQuery);
    const trackIdTp = destResult.columns[0].longValues![0];
    // TODO(taylori): If we had a consistent mapping from TP track_id
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
    const query = `SELECT ts, thread_state.dur, state, io_wait,
    thread_state.utid, thread_state.cpu, sched.id from thread_state
    left join sched using(ts) where thread_state.id = ${id}`;
    this.args.engine.query(query).then(result => {
      const selection = globals.state.currentSelection;
      const cols = result.columns;
      if (slowlyCountRows(result) === 1 && selection) {
        const ts = cols[0].longValues![0];
        const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
        const dur = fromNs(cols[1].longValues![0]);
        const stateStr = cols[2].stringValues![0];
        const ioWait =
            cols[3].isNulls![0] ? undefined : !!cols[3].longValues![0];
        const state = translateState(stateStr, ioWait);
        const utid = cols[4].longValues![0];
        const cpu = cols[5].isNulls![0] ? undefined : cols[5].longValues![0];
        const sliceId =
            cols[6].isNulls![0] ? undefined : cols[6].longValues![0];
        const selected: ThreadStateDetails =
            {ts: timeFromStart, dur, state, utid, cpu, sliceId};
        globals.publish('ThreadStateDetails', selected);
      }
    });
  }

  async sliceDetails(id: number) {
    const sqlQuery = `SELECT ts, dur, priority, end_state, utid, cpu,
    thread_state.id FROM sched join thread_state using(ts, utid, dur, cpu)
    WHERE sched.id = ${id}`;
    this.args.engine.query(sqlQuery).then(result => {
      // Check selection is still the same on completion of query.
      const selection = globals.state.currentSelection;
      if (slowlyCountRows(result) === 1 && selection) {
        const ts = result.columns[0].longValues![0];
        const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
        const dur = fromNs(result.columns[1].longValues![0]);
        const priority = result.columns[2].longValues![0];
        const endState = result.columns[3].stringValues![0];
        const utid = result.columns[4].longValues![0];
        const cpu = result.columns[5].longValues![0];
        const threadStateId = result.columns[6].longValues![0];
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
              globals.publish('SliceDetails', selected);
            });
      }
    });
  }

  async counterDetails(ts: number, rightTs: number, id: number) {
    const counter = await this.args.engine.query(
        `SELECT value, track_id FROM counter WHERE id = ${id}`);
    const value = counter.columns[0].doubleValues![0];
    const trackId = counter.columns[1].longValues![0];
    // Finding previous value. If there isn't previous one, it will return 0 for
    // ts and value.
    const previous = await this.args.engine.query(
        `SELECT MAX(ts), value FROM counter WHERE ts < ${ts} and track_id = ${
            trackId}`);
    const previousValue = previous.columns[1].doubleValues![0];
    const endTs =
        rightTs !== -1 ? rightTs : toNs(globals.state.traceTime.endSec);
    const delta = value - previousValue;
    const duration = endTs - ts;
    const startTime = fromNs(ts) - globals.state.traceTime.startSec;
    return {startTime, value, delta, duration};
  }

  async schedulingDetails(ts: number, utid: number|Long) {
    let event = 'sched_waking';
    const waking = await this.args.engine.query(
        `select * from instants where name = 'sched_waking' limit 1`);
    const wakeup = await this.args.engine.query(
        `select * from instants where name = 'sched_wakeup' limit 1`);
    if (slowlyCountRows(waking) === 0) {
      if (slowlyCountRows(wakeup) === 0) return undefined;
      // Only use sched_wakeup if waking is not in the trace.
      event = 'sched_wakeup';
    }

    // Find the ts of the first sched_wakeup before the current slice.
    const queryWakeupTs = `select ts from instants where name = '${event}'
    and ref = ${utid} and ts < ${ts} order by ts desc limit 1`;
    const wakeupRow = await this.args.engine.queryOneRow(queryWakeupTs);
    // Find the previous sched slice for the current utid.
    const queryPrevSched = `select ts from sched where utid = ${utid}
    and ts < ${ts} order by ts desc limit 1`;
    const prevSchedRow = await this.args.engine.queryOneRow(queryPrevSched);
    // If this is the first sched slice for this utid or if the wakeup found
    // was after the previous slice then we know the wakeup was for this slice.
    if (wakeupRow[0] === undefined ||
        (prevSchedRow[0] !== undefined && wakeupRow[0] < prevSchedRow[0])) {
      return undefined;
    }
    const wakeupTs = wakeupRow[0];
    // Find the sched slice with the utid of the waker running when the
    // sched wakeup occurred. This is the waker.
    const queryWaker = `select utid, cpu from sched where utid =
    (select utid from raw where name = '${event}' and ts = ${wakeupTs})
    and ts < ${wakeupTs} and ts + dur >= ${wakeupTs};`;
    const wakerRow = await this.args.engine.queryOneRow(queryWaker);
    if (wakerRow) {
      return {
        wakeupTs: fromNs(wakeupTs),
        wakerUtid: wakerRow[0],
        wakerCpu: wakerRow[1]
      };
    } else {
      return undefined;
    }
  }
}

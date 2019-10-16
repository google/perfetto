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
import {fromNs, toNs} from '../common/time';
import {
  CounterDetails,
  HeapProfileDetails,
  SliceDetails
} from '../frontend/globals';

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
    if (!selection) return;
    // TODO(taylori): Ideally thread_state should not be special cased, it
    // should have some form of id like everything else.
    if (selection.kind === 'THREAD_STATE') {
      const sqlQuery = `SELECT row_id FROM sched WHERE utid = ${selection.utid}
                        and ts = ${toNs(selection.ts)}`;
      this.args.engine.query(sqlQuery).then(result => {
        const id = result.columns[0].longValues![0] as number;
        this.sliceDetails(id);
      });
      return;
    }

    const selectWithId = ['SLICE', 'COUNTER', 'CHROME_SLICE', 'HEAP_PROFILE'];
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

    if (selection.kind === 'HEAP_PROFILE') {
      const selected: HeapProfileDetails = {};
      const ts = selection.ts;
      const upid = selection.upid;
      this.heapDumpDetails(ts, upid).then(results => {
        if (results !== undefined && selection &&
            selection.kind === selectedKind && selection.id === selectedId) {
          Object.assign(selected, results);
          globals.publish('HeapDumpDetails', selected);
        }
      });
    } else if (selection.kind === 'COUNTER') {
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
    } else if (selectedKind === 'SLICE') {
      this.sliceDetails(selectedId as number);
    } else if (selectedKind === 'CHROME_SLICE') {
      if (selectedId === -1) {
        globals.publish('SliceDetails', {ts: 0, name: 'Summarized slice'});
        return;
      }
      const sqlQuery = `SELECT ts, dur, name, cat FROM slices
      WHERE slice_id = ${selectedId}`;
      this.args.engine.query(sqlQuery).then(result => {
        // Check selection is still the same on completion of query.
        const selection = globals.state.currentSelection;
        if (result.numRecords === 1 && selection &&
            selection.kind === selectedKind && selection.id === selectedId) {
          const ts = result.columns[0].longValues![0] as number;
          const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
          const name = result.columns[2].stringValues![0];
          const dur = fromNs(result.columns[1].longValues![0] as number);
          const category = result.columns[3].stringValues![0];
          // TODO(nicomazz): Add arguments and thread timestamps
          const selected: SliceDetails =
              {ts: timeFromStart, dur, category, name, id: selectedId};
          globals.publish('SliceDetails', selected);
        }
      });
    }
  }

  async sliceDetails(id: number) {
    const sqlQuery = `SELECT ts, dur, priority, end_state, utid, cpu FROM sched
    WHERE row_id = ${id}`;
    this.args.engine.query(sqlQuery).then(result => {
      // Check selection is still the same on completion of query.
      const selection = globals.state.currentSelection;
      if (result.numRecords === 1 && selection) {
        const ts = result.columns[0].longValues![0] as number;
        const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
        const dur = fromNs(result.columns[1].longValues![0] as number);
        const priority = result.columns[2].longValues![0] as number;
        const endState = result.columns[3].stringValues![0];
        const utid = result.columns[4].longValues![0] as number;
        const cpu = result.columns[5].longValues![0] as number;
        const selected: SliceDetails =
            {ts: timeFromStart, dur, priority, endState, cpu, id, utid};
        this.schedulingDetails(ts, utid).then(wakeResult => {
          Object.assign(selected, wakeResult);
          globals.publish('SliceDetails', selected);
        });
      }
    });
  }

  async heapDumpDetails(ts: number, upid: number) {
    // Collecting data for more information about heap profile, such as:
    // total memory allocated, memory that is allocated and not freed.
    const pidValue = await this.args.engine.query(
        `select pid from process where upid = ${upid}`);
    const pid = pidValue.columns[0].longValues![0];
    const allocatedMemory = await this.args.engine.query(
        `select sum(size) from heap_profile_allocation where ts <= ${
            ts} and size > 0 and upid = ${upid}`);
    const allocated = allocatedMemory.columns[0].longValues![0];
    const allocatedNotFreedMemory = await this.args.engine.query(
        `select sum(size) from heap_profile_allocation where ts <= ${
            ts} and upid = ${upid}`);
    const allocatedNotFreed = allocatedNotFreedMemory.columns[0].longValues![0];
    const startTime = fromNs(ts) - globals.state.traceTime.startSec;
    return {
      ts: startTime,
      allocated,
      allocatedNotFreed,
      tsNs: ts,
      pid,
    };
  }

  async counterDetails(ts: number, rightTs: number, id: number) {
    const counter = await this.args.engine.query(
        `SELECT value FROM counter_values WHERE ts = ${ts} AND counter_id = ${
            id}`);
    const value = counter.columns[0].doubleValues![0];
    // Finding previous value. If there isn't previous one, it will return 0 for
    // ts and value.
    const previous = await this.args.engine.query(
        `SELECT MAX(ts), value FROM counter_values WHERE ts < ${
            ts} and counter_id = ${id}`);
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
    if (waking.numRecords === 0) {
      if (wakeup.numRecords === 0) return undefined;
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
    if (prevSchedRow[0] && wakeupRow[0] < prevSchedRow[0]) {
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

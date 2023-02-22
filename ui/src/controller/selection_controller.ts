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
import {fromNs, toNs} from '../common/time';
import {SliceDetails, ThreadStateDetails} from '../frontend/globals';
import {
  publishCounterDetails,
  publishSliceDetails,
  publishThreadStateDetails,
} from '../frontend/publish';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices';

import {parseArgs} from './args_parser';
import {Controller} from './controller';
import {globals} from './globals';

export interface SelectionControllerArgs {
  engine: Engine;
}

interface ThreadDetails {
  tid: number;
  threadName?: string;
}

interface ProcessDetails {
  pid?: number;
  processName?: string;
  uid?: number;
  packageName?: string;
  versionCode?: number;
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
          .then((results) => {
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
    let promisedArgs: Promise<Args>;
    // TODO(b/155483804): This is a hack to ensure annotation slices are
    // selectable for now. We should tidy this up when improving this class.
    if (table === 'annotation') {
      leafTable = 'annotation_slice';
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
      promisedArgs = this.getArgs(argSetId);
    }

    const promisedDetails = this.args.engine.query(`
      SELECT *, ABS_TIME_STR(ts) as absTime FROM ${leafTable} WHERE id = ${
        selectedId};
    `);

    const [details, args] = await Promise.all([promisedDetails, promisedArgs]);

    if (details.numRows() <= 0) return;
    const rowIter = details.iter({});
    assertTrue(rowIter.valid());

    // A few columns are hard coded as part of the SliceDetails interface.
    // Long term these should be handled generically as args but for now
    // handle them specially:
    let ts = undefined;
    let absTime = undefined;
    let dur = undefined;
    let name = undefined;
    let category = undefined;
    let threadDur = undefined;
    let threadTs = undefined;
    let trackId = undefined;

    // We select all columns from the leafTable to ensure that we include
    // additional fields from the child tables (like `thread_dur` from
    // `thread_slice` or `frame_number` from `frame_slice`).
    // However, this also includes some basic columns (especially from `slice`)
    // that are not interesting (i.e. `arg_set_id`, which has already been used
    // to resolve and show the arguments) and should not be shown to the user.
    const ignoredColumns = [
      'type',
      'depth',
      'parent_id',
      'stack_id',
      'parent_stack_id',
      'arg_set_id',
      'thread_instruction_count',
      'thread_instruction_delta',
    ];

    for (const k of details.columns()) {
      const v = rowIter.get(k);
      switch (k) {
        case 'id':
          break;
        case 'ts':
          ts = fromNs(Number(v)) - globals.state.traceTime.startSec;
          break;
        case 'thread_ts':
          threadTs = fromNs(Number(v));
          break;
        case 'absTime':
          if (v) absTime = `${v}`;
          break;
        case 'name':
          name = `${v}`;
          break;
        case 'dur':
          dur = fromNs(Number(v));
          break;
        case 'thread_dur':
          threadDur = fromNs(Number(v));
          break;
        case 'category':
        case 'cat':
          category = `${v}`;
          break;
        case 'track_id':
          trackId = Number(v);
          break;
        default:
          if (!ignoredColumns.includes(k)) args.set(k, `${v}`);
      }
    }

    const argsTree = parseArgs(args);
    const selected: SliceDetails = {
      id: selectedId,
      ts,
      threadTs,
      absTime,
      dur,
      threadDur,
      name,
      category,
      args,
      argsTree,
    };

    if (trackId !== undefined) {
      const columnInfo = (await this.args.engine.query(`
        WITH
           leafTrackTable AS (SELECT type FROM track WHERE id = ${trackId}),
           cols AS (
                SELECT name
                FROM pragma_table_info((SELECT type FROM leafTrackTable))
            )
        SELECT
           type as leafTrackTable,
          'upid' in cols AS hasUpid,
          'utid' in cols AS hasUtid
        FROM leafTrackTable
      `)).firstRow({hasUpid: NUM, hasUtid: NUM, leafTrackTable: STR});
      const hasUpid = columnInfo.hasUpid !== 0;
      const hasUtid = columnInfo.hasUtid !== 0;

      if (hasUtid) {
        const utid = (await this.args.engine.query(`
            SELECT utid
            FROM ${columnInfo.leafTrackTable}
            WHERE id = ${trackId};
        `)).firstRow({
             utid: NUM,
           }).utid;
        Object.assign(selected, await this.computeThreadDetails(utid));
      } else if (hasUpid) {
        const upid = (await this.args.engine.query(`
            SELECT upid
            FROM ${columnInfo.leafTrackTable}
            WHERE id = ${trackId};
        `)).firstRow({
             upid: NUM,
           }).upid;
        Object.assign(selected, await this.computeProcessDetails(upid));
      }
    }

    // Check selection is still the same on completion of query.
    if (selection === globals.state.currentSelection) {
      publishSliceDetails(selected);
    }
  }

  async getArgs(argId: number): Promise<Args> {
    const args = new Map<string, Arg>();
    const query = `
      select
        key AS name,
        display_value AS value
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

  // TODO(altimin): We currently rely on the ThreadStateDetails for supporting
  // marking the area (the rest goes is handled by ThreadStateTab
  // directly. Refactor it to be plugin-friendly and remove this.
  async threadStateDetails(id: number) {
    const query = `
      SELECT
        ts,
        thread_state.dur as dur
      from thread_state
      where thread_state.id = ${id}
    `;
    const result = await this.args.engine.query(query);

    const selection = globals.state.currentSelection;
    if (result.numRows() > 0 && selection) {
      const row = result.firstRow({
        ts: NUM,
        dur: NUM,
      });
      const ts = row.ts;
      const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
      const dur = fromNs(row.dur);
      const selected: ThreadStateDetails = {ts: timeFromStart, dur};
      publishThreadStateDetails(selected);
    }
  }

  async sliceDetails(id: number) {
    const sqlQuery = `SELECT
      sched.ts,
      sched.dur,
      sched.priority,
      sched.end_state as endState,
      sched.utid,
      sched.cpu,
      thread_state.id as threadStateId
    FROM sched left join thread_state using(ts, utid, cpu)
    WHERE sched.id = ${id}`;
    const result = await this.args.engine.query(sqlQuery);
    // Check selection is still the same on completion of query.
    const selection = globals.state.currentSelection;
    if (result.numRows() > 0 && selection) {
      const row = result.firstRow({
        ts: NUM,
        dur: NUM,
        priority: NUM,
        endState: STR_NULL,
        utid: NUM,
        cpu: NUM,
        threadStateId: NUM_NULL,
      });
      const ts = row.ts;
      const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
      const dur = fromNs(row.dur);
      const priority = row.priority;
      const endState = row.endState;
      const utid = row.utid;
      const cpu = row.cpu;
      const threadStateId = row.threadStateId || undefined;
      const selected: SliceDetails = {
        ts: timeFromStart,
        dur,
        priority,
        endState,
        cpu,
        id,
        utid,
        threadStateId,
      };
      Object.assign(selected, await this.computeThreadDetails(utid));

      this.schedulingDetails(ts, utid)
          .then((wakeResult) => {
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
    const uiTrackId = globals.state.uiTrackIdByTraceTrackId[trackId];
    const name = uiTrackId ? globals.state.tracks[uiTrackId].name : undefined;
    return {startTime, value, delta, duration, name};
  }

  async schedulingDetails(ts: number, utid: number|Long) {
    // Find the ts of the first wakeup before the current slice.
    const wakeResult = await this.args.engine.query(`
      select ts, waker_utid as wakerUtid
      from thread_state
      where utid = ${utid} and ts < ${ts} and state = 'R'
      order by ts desc
      limit 1
    `);
    if (wakeResult.numRows() === 0) {
      return undefined;
    }

    const wakeFirstRow = wakeResult.firstRow({ts: NUM, wakerUtid: NUM_NULL});
    const wakeupTs = wakeFirstRow.ts;
    const wakerUtid = wakeFirstRow.wakerUtid;
    if (wakerUtid === null) {
      return undefined;
    }

    // Find the previous sched slice for the current utid.
    const prevSchedResult = await this.args.engine.query(`
      select ts
      from sched
      where utid = ${utid} and ts < ${ts}
      order by ts desc
      limit 1
    `);

    // If this is the first sched slice for this utid or if the wakeup found
    // was after the previous slice then we know the wakeup was for this slice.
    if (prevSchedResult.numRows() !== 0 &&
        wakeupTs < prevSchedResult.firstRow({ts: NUM}).ts) {
      return undefined;
    }

    // Find the sched slice with the utid of the waker running when the
    // sched wakeup occurred. This is the waker.
    const wakerResult = await this.args.engine.query(`
      select cpu
      from sched
      where
        utid = ${wakerUtid} and
        ts < ${wakeupTs} and
        ts + dur >= ${wakeupTs};
    `);
    if (wakerResult.numRows() === 0) {
      return undefined;
    }

    const wakerRow = wakerResult.firstRow({cpu: NUM});
    return {wakeupTs: fromNs(wakeupTs), wakerUtid, wakerCpu: wakerRow.cpu};
  }

  async computeThreadDetails(utid: number):
      Promise<ThreadDetails&ProcessDetails> {
    const threadInfo = (await this.args.engine.query(`
          SELECT tid, name, upid
          FROM thread
          WHERE utid = ${utid};
      `)).firstRow({tid: NUM, name: STR_NULL, upid: NUM_NULL});
    const threadDetails = {
      tid: threadInfo.tid,
      threadName: threadInfo.name || undefined,
    };
    if (threadInfo.upid) {
      return Object.assign(
          {}, threadDetails, await this.computeProcessDetails(threadInfo.upid));
    }
    return threadDetails;
  }

  async computeProcessDetails(upid: number): Promise<ProcessDetails> {
    const details: ProcessDetails = {};
    const processResult = (await this.args.engine.query(`
                SELECT pid, name, uid FROM process WHERE upid = ${upid};
              `)).firstRow({pid: NUM, name: STR_NULL, uid: NUM_NULL});
    details.pid = processResult.pid;
    details.processName = processResult.name || undefined;
    if (processResult.uid === null) {
      return details;
    }
    details.uid = processResult.uid;

    const packageResult = await this.args.engine.query(`
                  SELECT
                    package_name as packageName,
                    version_code as versionCode
                  FROM package_list WHERE uid = ${details.uid};
                `);
    // The package_list table is not populated in some traces so we need to
    // check if the result has returned any rows.
    if (packageResult.numRows() > 0) {
      const packageDetails = packageResult.firstRow({
        packageName: STR,
        versionCode: NUM,
      });
      details.packageName = packageDetails.packageName;
      details.versionCode = packageDetails.versionCode;
    }
    return details;
  }
}

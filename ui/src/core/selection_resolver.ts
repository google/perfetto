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
import {Time, time} from '../base/time';
import {Optional} from '../base/utils';
import {Args, ArgValue} from './selection_arg_types';
import {THREAD_SLICE_TRACK_KIND} from '../public/track_kinds';
import {Engine} from '../trace_processor/engine';
import {
  durationFromSql,
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
  timeFromSql,
} from '../trace_processor/query_result';
import {fromNumNull} from '../trace_processor/sql_utils';
import {LegacySelection, ThreadSliceSelection} from '../public/selection';
import {
  SelectedSliceDetails,
  SelectedThreadStateDetails,
} from './selection_manager';
import {TrackManagerImpl} from './track_manager';

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
export class SelectionResolver {
  constructor(
    private engine: Engine,
    private trackManager: TrackManagerImpl,
  ) {}

  async resolveSelection(
    selection: LegacySelection,
  ): Promise<Optional<SelectedSliceDetails>> {
    if (selection.kind === 'SCHED_SLICE') {
      return this.schedSliceDetails(selection.id);
    } else if (selection.kind === 'THREAD_STATE') {
      return this.threadStateDetails(selection.id);
    } else if (selection.kind === 'SLICE') {
      return this.sliceDetails(selection);
    }

    return undefined;
  }

  private async sliceDetails(
    selection: ThreadSliceSelection,
  ): Promise<Optional<SelectedSliceDetails>> {
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
      const result = await this.engine.query(`
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

    const promisedDetails = this.engine.query(`
      SELECT *, ABS_TIME_STR(ts) as absTime FROM ${leafTable} WHERE id = ${selectedId};
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
          ts = timeFromSql(v);
          break;
        case 'thread_ts':
          threadTs = timeFromSql(v);
          break;
        case 'absTime':
          /* eslint-disable @typescript-eslint/strict-boolean-expressions */
          if (v) absTime = `${v}`;
          /* eslint-enable */
          break;
        case 'name':
          name = `${v}`;
          break;
        case 'dur':
          dur = durationFromSql(v);
          break;
        case 'thread_dur':
          threadDur = durationFromSql(v);
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

    const selected: SelectedSliceDetails = {
      id: selectedId,
      ts,
      threadTs,
      absTime,
      dur,
      threadDur,
      name,
      category,
    };

    if (trackId !== undefined) {
      const columnInfo = (
        await this.engine.query(`
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
      `)
      ).firstRow({hasUpid: NUM, hasUtid: NUM, leafTrackTable: STR});
      const hasUpid = columnInfo.hasUpid !== 0;
      const hasUtid = columnInfo.hasUtid !== 0;

      if (hasUtid) {
        const utid = (
          await this.engine.query(`
            SELECT utid
            FROM ${columnInfo.leafTrackTable}
            WHERE id = ${trackId};
        `)
        ).firstRow({
          utid: NUM,
        }).utid;
        Object.assign(selected, await this.computeThreadDetails(utid));
      } else if (hasUpid) {
        const upid = (
          await this.engine.query(`
            SELECT upid
            FROM ${columnInfo.leafTrackTable}
            WHERE id = ${trackId};
        `)
        ).firstRow({
          upid: NUM,
        }).upid;
        Object.assign(selected, await this.computeProcessDetails(upid));
      }
    }

    // Check selection is still the same on completion of query.
    return selected;
  }

  private async getArgs(argId: number): Promise<Args> {
    const args = new Map<string, ArgValue>();
    const query = `
      select
        key AS name,
        display_value AS value
      FROM args
      WHERE arg_set_id = ${argId}
    `;
    const result = await this.engine.query(query);
    const it = result.iter({
      name: STR,
      value: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const name = it.name;
      const value = it.value ?? 'NULL';
      if (name === 'destination slice id' && !isNaN(Number(value))) {
        const destTrackUri = await this.getDestTrackUri(value);
        args.set('Destination Slice', {
          kind: 'SCHED_SLICE',
          trackUri: destTrackUri,
          sliceId: Number(value),
          rawValue: value,
        });
      } else {
        args.set(name, value);
      }
    }
    return args;
  }

  private async getDestTrackUri(sliceId: string): Promise<string> {
    const trackIdQuery = `select track_id as trackId from slice
    where slice_id = ${sliceId}`;
    const result = await this.engine.query(trackIdQuery);
    const trackId = result.firstRow({trackId: NUM}).trackId;
    // TODO(hjd): If we had a consistent mapping from TP track_id
    // UI track id for slice tracks this would be unnecessary.
    for (const trackInfo of this.trackManager.getAllTracks()) {
      if (trackInfo?.tags?.kind === THREAD_SLICE_TRACK_KIND) {
        const trackIds = trackInfo?.tags?.trackIds;
        if (trackIds && trackIds.length > 0 && trackIds[0] === trackId) {
          return trackInfo.uri;
        }
      }
    }
    return '';
  }

  // TODO(altimin): We currently rely on the ThreadStateDetails for supporting
  // marking the area (the rest goes is handled by ThreadStateTab
  // directly. Refactor it to be plugin-friendly and remove this.
  private async threadStateDetails(
    id: number,
  ): Promise<Optional<SelectedThreadStateDetails>> {
    const query = `
      SELECT
        ts,
        thread_state.dur as dur
      from thread_state
      where thread_state.id = ${id}
    `;
    const result = await this.engine.query(query);

    if (result.numRows() > 0) {
      const row = result.firstRow({
        ts: LONG,
        dur: LONG,
      });
      return {
        ts: Time.fromRaw(row.ts),
        dur: row.dur,
      };
    }
    return undefined;
  }

  private async schedSliceDetails(
    id: number,
  ): Promise<Optional<SelectedSliceDetails>> {
    const sqlQuery = `
      SELECT
        ts,
        dur,
        priority,
        end_state as endState,
        utid,
        cpu
      FROM sched
      WHERE sched.id = ${id}
    `;
    const result = await this.engine.query(sqlQuery);
    if (result.numRows() <= 0) return undefined;
    const row = result.firstRow({
      ts: LONG,
      dur: LONG,
      priority: NUM,
      endState: STR_NULL,
      utid: NUM,
      cpu: NUM,
    });
    const ts = Time.fromRaw(row.ts);
    const dur = row.dur;
    const priority = row.priority;
    const endState = row.endState;
    const utid = row.utid;
    const cpu = row.cpu;
    const selected: SelectedSliceDetails = {
      ts,
      dur,
      priority,
      endState,
      cpu,
      id,
      utid,
    };

    selected.threadStateId = await getThreadStateForSchedSlice(this.engine, id);

    Object.assign(selected, await this.computeThreadDetails(utid));
    Object.assign(selected, await this.schedulingDetails(ts, utid));
    return selected;
  }

  private async schedulingDetails(ts: time, utid: number) {
    // Find the ts of the first wakeup before the current slice.
    const wakeResult = await this.engine.query(`
      select ts, waker_utid as wakerUtid
      from thread_state
      where utid = ${utid} and ts < ${ts} and state = 'R'
      order by ts desc
      limit 1
    `);
    if (wakeResult.numRows() === 0) {
      return undefined;
    }

    const wakeFirstRow = wakeResult.firstRow({ts: LONG, wakerUtid: NUM_NULL});
    const wakeupTs = wakeFirstRow.ts;
    const wakerUtid = wakeFirstRow.wakerUtid;
    if (wakerUtid === null) {
      return undefined;
    }

    // Find the previous sched slice for the current utid.
    const prevSchedResult = await this.engine.query(`
      select ts
      from sched
      where utid = ${utid} and ts < ${ts}
      order by ts desc
      limit 1
    `);

    // If this is the first sched slice for this utid or if the wakeup found
    // was after the previous slice then we know the wakeup was for this slice.
    if (
      prevSchedResult.numRows() !== 0 &&
      wakeupTs < prevSchedResult.firstRow({ts: LONG}).ts
    ) {
      return undefined;
    }

    // Find the sched slice with the utid of the waker running when the
    // sched wakeup occurred. This is the waker.
    const wakerResult = await this.engine.query(`
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
    return {wakeupTs, wakerUtid, wakerCpu: wakerRow.cpu};
  }

  private async computeThreadDetails(
    utid: number,
  ): Promise<ThreadDetails & ProcessDetails> {
    const res = await this.engine.query(`
      SELECT tid, name, upid
      FROM thread
      WHERE utid = ${utid};
    `);
    const threadInfo = res.firstRow({tid: NUM, name: STR_NULL, upid: NUM_NULL});
    const threadDetails: ThreadDetails = {
      tid: threadInfo.tid,
      threadName: threadInfo.name ?? undefined,
    };
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (threadInfo.upid) {
      return Object.assign(
        {},
        threadDetails,
        await this.computeProcessDetails(threadInfo.upid),
      );
    }
    return threadDetails;
  }

  private async computeProcessDetails(upid: number) {
    const res = await this.engine.query(`
      include perfetto module android.process_metadata;
      select
        p.pid,
        p.name,
        p.uid,
        m.package_name as packageName,
        m.version_code as versionCode
      from process p
      left join android_process_metadata m using (upid)
      where p.upid = ${upid};
    `);
    const row = res.firstRow({
      pid: NUM,
      uid: NUM_NULL,
      packageName: STR_NULL,
      versionCode: NUM_NULL,
    });
    return {
      pid: row.pid,
      uid: fromNumNull(row.uid),
      packageName: row.packageName ?? undefined,
      versionCode: fromNumNull(row.versionCode),
    };
  }
}

// Get the corresponding thread state slice id for a given sched slice
async function getThreadStateForSchedSlice(
  engine: Engine,
  id: number,
): Promise<Optional<number>> {
  const sqlQuery = `
    SELECT
      thread_state.id as threadStateId
    FROM sched
    JOIN thread_state USING(ts, utid, cpu)
    WHERE sched.id = ${id}
  `;
  const threadStateResult = await engine.query(sqlQuery);
  if (threadStateResult.numRows() === 1) {
    const row = threadStateResult.firstRow({
      threadStateId: NUM,
    });
    return row.threadStateId;
  } else {
    return undefined;
  }
}

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

import m from 'mithril';

import {Actions} from '../../common/actions';
import {EngineProxy} from '../../common/engine';
import {LONG, NUM, STR} from '../../common/query_result';
import {TPDuration} from '../../common/time';
import {Anchor} from '../anchor';
import {globals} from '../globals';
import {focusHorizontalRange, verticalScrollToTrack} from '../scroll_helper';
import {
  asSliceSqlId,
  asUpid,
  asUtid,
  SliceSqlId,
  TPTimestamp,
  Upid,
  Utid,
} from '../sql_types';
import {asTPTimestamp} from '../sql_types';
import {constraintsToQueryFragment, SQLConstraints} from '../sql_utils';
import {
  getProcessInfo,
  getThreadInfo,
  ProcessInfo,
  ThreadInfo,
} from '../thread_and_process_info';

export interface SliceDetails {
  id: SliceSqlId;
  name: string;
  ts: TPTimestamp;
  dur: TPDuration;
  sqlTrackId: number;
  thread?: ThreadInfo;
  process?: ProcessInfo;
}

async function getUtidAndUpid(engine: EngineProxy, sqlTrackId: number):
    Promise<{utid?: Utid, upid?: Upid}> {
  const columnInfo = (await engine.query(`
    WITH
       leafTrackTable AS (SELECT type FROM track WHERE id = ${sqlTrackId}),
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

  const result: {utid?: Utid, upid?: Upid} = {};

  if (hasUtid) {
    const utid = (await engine.query(`
        SELECT utid
        FROM ${columnInfo.leafTrackTable}
        WHERE id = ${sqlTrackId};
    `)).firstRow({
         utid: NUM,
       }).utid;
    result.utid = asUtid(utid);
  } else if (hasUpid) {
    const upid = (await engine.query(`
        SELECT upid
        FROM ${columnInfo.leafTrackTable}
        WHERE id = ${sqlTrackId};
    `)).firstRow({
         upid: NUM,
       }).upid;
    result.upid = asUpid(upid);
  }
  return result;
}

async function getSliceFromConstraints(
    engine: EngineProxy, constraints: SQLConstraints): Promise<SliceDetails[]> {
  const query = await engine.query(`
    SELECT
      id,
      name,
      ts,
      dur,
      track_id as trackId
    FROM slice
    ${constraintsToQueryFragment(constraints)}`);
  const it = query.iter({
    id: NUM,
    name: STR,
    ts: LONG,
    dur: LONG,
    trackId: NUM,
  });

  const result: SliceDetails[] = [];
  for (; it.valid(); it.next()) {
    const {utid, upid} = await getUtidAndUpid(engine, it.trackId);

    const thread: ThreadInfo|undefined =
        utid === undefined ? undefined : await getThreadInfo(engine, utid);
    const process: ProcessInfo|undefined = thread !== undefined ?
        thread.process :
        (upid === undefined ? undefined : await getProcessInfo(engine, upid));

    result.push({
      id: asSliceSqlId(it.id),
      name: it.name,
      ts: asTPTimestamp(it.ts),
      dur: it.dur,
      sqlTrackId: it.trackId,
      thread,
      process,
    });
  }
  return result;
}

export async function getSlice(
    engine: EngineProxy, id: SliceSqlId): Promise<SliceDetails|undefined> {
  const result = await getSliceFromConstraints(engine, {
    filters: [`id=${id}`],
  });
  if (result.length > 1) {
    throw new Error(`slice table has more than one row with id ${id}`);
  }
  if (result.length === 0) {
    return undefined;
  }
  return result[0];
}

interface SliceRefAttrs {
  readonly id: SliceSqlId;
  readonly name: string;
  readonly ts: TPTimestamp;
  readonly dur: TPDuration;
  readonly sqlTrackId: number;
}

export class SliceRef implements m.ClassComponent<SliceRefAttrs> {
  view(vnode: m.Vnode<SliceRefAttrs>) {
    return m(
        Anchor,
        {
          icon: 'open_in_new',
          onclick: () => {
            const uiTrackId =
                globals.state.uiTrackIdByTraceTrackId[vnode.attrs.sqlTrackId];
            if (uiTrackId === undefined) return;
            verticalScrollToTrack(uiTrackId, true);
            focusHorizontalRange(
                vnode.attrs.ts, vnode.attrs.ts + vnode.attrs.dur);
            globals.makeSelection(Actions.selectChromeSlice(
                {id: vnode.attrs.id, trackId: uiTrackId, table: 'slice'}));
          },
        },
        vnode.attrs.name);
  }
}

export function sliceRef(slice: SliceDetails, name?: string): m.Child {
  return m(SliceRef, {
    id: slice.id,
    name: name ?? slice.name,
    ts: slice.ts,
    dur: slice.dur,
    sqlTrackId: slice.sqlTrackId,
  });
}

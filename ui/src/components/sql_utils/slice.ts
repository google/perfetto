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

import {duration, Time, time} from '../../base/time';
import {exists} from '../../base/utils';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {
  constraintsToQuerySuffix,
  SQLConstraints,
} from '../../trace_processor/sql_utils';
import {
  asArgSetId,
  asSliceSqlId,
  asUpid,
  asUtid,
  SliceSqlId,
  Upid,
  Utid,
} from './core_types';
import {ArgsDict, getArgs} from './args';
import {getThreadInfo, ThreadInfo} from './thread';
import {getProcessInfo, ProcessInfo} from './process';

// Basic information about a slice.
export interface SliceDetails {
  id: SliceSqlId;
  name?: string;
  ts: time;
  absTime?: string;
  dur: duration;
  parentId?: SliceSqlId;
  trackId: number;
  depth: number;
  thread?: ThreadInfo;
  process?: ProcessInfo;
  threadTs?: time;
  threadDur?: duration;
  category?: string;
  args?: ArgsDict;
}

async function getUtidAndUpid(
  engine: Engine,
  sqlTrackId: number,
): Promise<{utid?: Utid; upid?: Upid}> {
  const {upid, utid} = (
    await engine.query(`
      SELECT
        extract_arg(dimension_arg_set_id, 'upid') as upid,
        extract_arg(dimension_arg_set_id, 'utid') as utid
      FROM track
      WHERE id = ${sqlTrackId}
    `)
  ).firstRow({upid: NUM_NULL, utid: NUM_NULL});
  return {upid: asUpid(upid ?? undefined), utid: asUtid(utid ?? undefined)};
}

export async function getSliceFromConstraints(
  engine: Engine,
  constraints: SQLConstraints,
): Promise<SliceDetails[]> {
  const query = await engine.query(`
    SELECT
      id,
      name,
      ts,
      dur,
      track_id as trackId,
      depth,
      parent_id as parentId,
      thread_dur as threadDur,
      thread_ts as threadTs,
      category,
      arg_set_id as argSetId,
      ABS_TIME_STR(ts) as absTime
    FROM slice
    ${constraintsToQuerySuffix(constraints)}`);
  const it = query.iter({
    id: NUM,
    name: STR_NULL,
    ts: LONG,
    dur: LONG,
    trackId: NUM,
    depth: NUM,
    parentId: NUM_NULL,
    threadDur: LONG_NULL,
    threadTs: LONG_NULL,
    category: STR_NULL,
    argSetId: NUM_NULL,
    absTime: STR_NULL,
  });

  const result: SliceDetails[] = [];
  for (; it.valid(); it.next()) {
    const {utid, upid} = await getUtidAndUpid(engine, it.trackId);

    const thread: ThreadInfo | undefined =
      utid === undefined ? undefined : await getThreadInfo(engine, utid);
    const process: ProcessInfo | undefined =
      thread !== undefined
        ? thread.process
        : upid === undefined
          ? undefined
          : await getProcessInfo(engine, upid);

    result.push({
      id: asSliceSqlId(it.id),
      name: it.name ?? undefined,
      ts: Time.fromRaw(it.ts),
      dur: it.dur,
      trackId: it.trackId,
      depth: it.depth,
      parentId: asSliceSqlId(it.parentId ?? undefined),
      thread,
      process,
      threadDur: it.threadDur ?? undefined,
      threadTs: exists(it.threadTs) ? Time.fromRaw(it.threadTs) : undefined,
      category: it.category ?? undefined,
      args: exists(it.argSetId)
        ? await getArgs(engine, asArgSetId(it.argSetId))
        : undefined,
      absTime: it.absTime ?? undefined,
    });
  }
  return result;
}

export async function getSlice(
  engine: Engine,
  id: SliceSqlId,
): Promise<SliceDetails | undefined> {
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

// A slice tree node, combining the information about the given slice with
// information about its descendants.
export interface SliceTreeNode extends SliceDetails {
  children: SliceTreeNode[];
  parent?: SliceTreeNode;
}

// Get all descendants for a given slice in a tree form.
export async function getDescendantSliceTree(
  engine: Engine,
  id: SliceSqlId,
): Promise<SliceTreeNode | undefined> {
  const slice = await getSlice(engine, id);
  if (slice === undefined) {
    return undefined;
  }
  const descendants = await getSliceFromConstraints(engine, {
    filters: [
      `track_id=${slice.trackId}`,
      `depth >= ${slice.depth}`,
      `ts >= ${slice.ts}`,
      // TODO(altimin): consider making `dur` undefined here instead of -1.
      slice.dur >= 0 ? `ts <= (${slice.ts} + ${slice.dur})` : undefined,
    ],
    orderBy: ['ts', 'depth'],
  });
  const slices: {[key: SliceSqlId]: SliceTreeNode} = Object.fromEntries(
    descendants.map((slice) => [
      slice.id,
      {
        children: [],
        ...slice,
      },
    ]),
  );
  for (const [_, slice] of Object.entries(slices)) {
    if (slice.parentId !== undefined) {
      const parent = slices[slice.parentId];
      slice.parent = parent;
      parent.children.push(slice);
    }
  }
  return slices[id];
}

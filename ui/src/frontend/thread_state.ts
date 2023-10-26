// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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

import {Icons} from '../base/semantic_icons';
import {
  duration,
  Time,
  time,
} from '../base/time';
import {exists} from '../base/utils';
import {Actions} from '../common/actions';
import {EngineProxy} from '../common/engine';
import {pluginManager} from '../common/plugins';
import {LONG, NUM, NUM_NULL, STR_NULL} from '../common/query_result';
import {translateState} from '../common/thread_state';
import {CPU_SLICE_TRACK_KIND} from '../tracks/cpu_slices';
import {THREAD_STATE_TRACK_KIND} from '../tracks/thread_state';
import {Anchor} from '../widgets/anchor';

import {globals} from './globals';
import {scrollToTrackAndTs} from './scroll_helper';
import {
  asUtid,
  SchedSqlId,
  ThreadStateSqlId,
  Utid,
} from './sql_types';
import {
  constraintsToQuerySuffix,
  fromNumNull,
  SQLConstraints,
} from './sql_utils';
import {
  getThreadInfo,
  ThreadInfo,
} from './thread_and_process_info';

// Representation of a single thread state object, corresponding to
// a row for the |thread_slice| table.
export interface ThreadState {
  // Id into |thread_state| table.
  threadStateSqlId: ThreadStateSqlId;
  // Id of the corresponding entry in the |sched| table.
  schedSqlId?: SchedSqlId;
  // Timestamp of the beginning of this thread state in nanoseconds.
  ts: time;
  // Duration of this thread state in nanoseconds.
  dur: duration;
  // CPU id if this thread state corresponds to a thread running on the CPU.
  cpu?: number;
  // Human-readable name of this thread state.
  state: string;
  blockedFunction?: string;

  thread?: ThreadInfo;
  wakerThread?: ThreadInfo;
}

// Gets a list of thread state objects from Trace Processor with given
// constraints.
export async function getThreadStateFromConstraints(
    engine: EngineProxy, constraints: SQLConstraints): Promise<ThreadState[]> {
  const query = await engine.query(`
    SELECT
      thread_state.id as threadStateSqlId,
      (select sched.id
        from sched
        where sched.ts=thread_state.ts and sched.utid=thread_state.utid
        limit 1
       ) as schedSqlId,
      ts,
      thread_state.dur as dur,
      thread_state.cpu as cpu,
      state,
      thread_state.blocked_function as blockedFunction,
      io_wait as ioWait,
      thread_state.utid as utid,
      waker_utid as wakerUtid
    FROM thread_state
    ${constraintsToQuerySuffix(constraints)}`);
  const it = query.iter({
    threadStateSqlId: NUM,
    schedSqlId: NUM_NULL,
    ts: LONG,
    dur: LONG,
    cpu: NUM_NULL,
    state: STR_NULL,
    blockedFunction: STR_NULL,
    ioWait: NUM_NULL,
    utid: NUM,
    wakerUtid: NUM_NULL,
  });

  const result: ThreadState[] = [];

  for (; it.valid(); it.next()) {
    const ioWait = it.ioWait === null ? undefined : it.ioWait > 0;
    const wakerUtid = asUtid(it.wakerUtid || undefined);

    // TODO(altimin): Consider fetcing thread / process info using a single
    // query instead of one per row.
    result.push({
      threadStateSqlId: it.threadStateSqlId as ThreadStateSqlId,
      schedSqlId: fromNumNull(it.schedSqlId) as (SchedSqlId | undefined),
      ts: Time.fromRaw(it.ts),
      dur: it.dur,
      cpu: fromNumNull(it.cpu),
      state: translateState(it.state || undefined, ioWait),
      blockedFunction: it.blockedFunction || undefined,
      thread: await getThreadInfo(engine, asUtid(it.utid)),
      wakerThread: wakerUtid ? await getThreadInfo(engine, wakerUtid) :
                               undefined,
    });
  }
  return result;
}

export async function getThreadState(
    engine: EngineProxy, id: number): Promise<ThreadState|undefined> {
  const result = await getThreadStateFromConstraints(engine, {
    filters: [`id=${id}`],
  });
  if (result.length > 1) {
    throw new Error(`thread_state table has more than one row with id ${id}`);
  }
  if (result.length === 0) {
    return undefined;
  }
  return result[0];
}

export function goToSchedSlice(cpu: number, id: SchedSqlId, ts: time) {
  let trackId: string|undefined;
  for (const track of Object.values(globals.state.tracks)) {
    if (exists(track?.uri)) {
      const trackInfo = pluginManager.resolveTrackInfo(track.uri);
      if (trackInfo?.kind === CPU_SLICE_TRACK_KIND) {
        if (trackInfo?.cpu === cpu) {
          trackId = track.key;
          break;
        }
      }
    }
  }
  if (trackId === undefined) {
    return;
  }
  globals.makeSelection(Actions.selectSlice({id, trackKey: trackId}));
  scrollToTrackAndTs(trackId, ts);
}

interface ThreadStateRefAttrs {
  id: ThreadStateSqlId;
  ts: time;
  dur: duration;
  utid: Utid;
  // If not present, a placeholder name will be used.
  name?: string;
}

export class ThreadStateRef implements m.ClassComponent<ThreadStateRefAttrs> {
  view(vnode: m.Vnode<ThreadStateRefAttrs>) {
    return m(
        Anchor,
        {
          icon: Icons.UpdateSelection,
          onclick: () => {
            let trackKey: string|number|undefined;
            for (const track of Object.values(globals.state.tracks)) {
              const trackDesc = pluginManager.resolveTrackInfo(track.uri);
              // TODO(stevegolton): Handle v2.
              if (trackDesc && trackDesc.kind === THREAD_STATE_TRACK_KIND &&
                  trackDesc.utid === vnode.attrs.utid) {
                trackKey = track.key;
              }
            }

            if (trackKey) {
              globals.makeSelection(Actions.selectThreadState({
                id: vnode.attrs.id,
                trackKey: trackKey.toString(),
              }));

              scrollToTrackAndTs(trackKey, vnode.attrs.ts, true);
            }
          },
        },
        vnode.attrs.name ?? `Thread State ${vnode.attrs.id}`,
    );
  }
}

export function threadStateRef(state: ThreadState): m.Child {
  if (state.thread === undefined) return null;

  return m(ThreadStateRef, {
    id: state.threadStateSqlId,
    ts: state.ts,
    dur: state.dur,
    utid: state.thread?.utid,
  });
}

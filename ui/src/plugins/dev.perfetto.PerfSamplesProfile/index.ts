// Copyright (C) 2021 The Android Open Source Project
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

import {TrackData} from '../../common/track_data';
import {PERF_SAMPLES_PROFILE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {assertExists} from '../../base/logging';
import {
  ProcessPerfSamplesProfileTrack,
  ThreadPerfSamplesProfileTrack,
} from './perf_samples_profile_track';
import {getThreadUriPrefix} from '../../public/utils';
import {
  getOrCreateGroupForProcess,
  getOrCreateGroupForThread,
} from '../../public/standard_groups';
import {TrackNode} from '../../public/workspace';

export interface Data extends TrackData {
  tsStarts: BigInt64Array;
}

function makeUriForProc(upid: number) {
  return `/process_${upid}/perf_samples_profile`;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.PerfSamplesProfile';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const pResult = await ctx.engine.query(`
      select distinct upid
      from perf_sample
      join thread using (utid)
      where callsite_id is not null and upid is not null
    `);
    for (const it = pResult.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uri = makeUriForProc(upid);
      const title = `Process Callstacks`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
          upid,
        },
        track: new ProcessPerfSamplesProfileTrack(
          {
            trace: ctx,
            uri,
          },
          upid,
        ),
      });
      const group = getOrCreateGroupForProcess(ctx.workspace, upid);
      const track = new TrackNode({uri, title, sortOrder: -40});
      group.addChildInOrder(track);
    }
    const tResult = await ctx.engine.query(`
      select distinct
        utid,
        tid,
        thread.name as threadName,
        upid
      from perf_sample
      join thread using (utid)
      where callsite_id is not null
    `);
    for (
      const it = tResult.iter({
        utid: NUM,
        tid: NUM,
        threadName: STR_NULL,
        upid: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const {threadName, utid, tid, upid} = it;
      const title =
        threadName === null
          ? `Thread Callstacks ${tid}`
          : `${threadName} Callstacks ${tid}`;
      const uri = `${getThreadUriPrefix(upid, utid)}_perf_samples_profile`;
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
          utid,
          upid: upid ?? undefined,
        },
        track: new ThreadPerfSamplesProfileTrack(
          {
            trace: ctx,
            uri,
          },
          utid,
        ),
      });
      const group = getOrCreateGroupForThread(ctx.workspace, utid);
      const track = new TrackNode({uri, title, sortOrder: -50});
      group.addChildInOrder(track);
    }

    ctx.addEventListener('traceready', async () => {
      await selectPerfSample(ctx);
    });
  }
}

async function selectPerfSample(ctx: Trace) {
  const profile = await assertExists(ctx.engine).query(`
    select upid
    from perf_sample
    join thread using (utid)
    where callsite_id is not null
    order by ts desc
    limit 1
  `);
  if (profile.numRows() !== 1) return;
  const row = profile.firstRow({upid: NUM});
  const upid = row.upid;

  // Create an area selection over the first process with a perf samples track
  ctx.selection.selectArea({
    start: ctx.traceInfo.start,
    end: ctx.traceInfo.end,
    trackUris: [makeUriForProc(upid)],
  });
}

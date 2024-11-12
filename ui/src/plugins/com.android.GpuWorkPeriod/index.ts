// Copyright (C) 2024 The Android Open Source Project
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

import {NUM, STR} from '../../trace_processor/query_result';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {createQuerySliceTrack} from '../../public/lib/tracks/query_slice_track';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.GpuWorkPeriod';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      include perfetto module android.gpu.work_period;

      with grouped_packages as materialized (
        select
          uid,
          group_concat(package_name, ',') as package_name,
          count() as cnt
        from package_list
        group by uid
      )
      select
        t.id trackId,
        t.uid as uid,
        t.gpu_id as gpuId,
        iif(g.cnt = 1, g.package_name, 'UID ' || t.uid) as packageName
      from android_gpu_work_period_track t
      left join grouped_packages g using (uid)
      order by uid
    `);

    const it = result.iter({
      trackId: NUM,
      uid: NUM,
      gpuId: NUM,
      packageName: STR,
    });

    const workPeriodByGpu = new Map<number, TrackNode>();
    for (; it.valid(); it.next()) {
      const {trackId, gpuId, uid, packageName} = it;
      const uri = `/gpu_work_period_${gpuId}_${uid}`;
      const track = await createQuerySliceTrack({
        trace: ctx,
        uri,
        data: {
          sqlSource: `
            select ts, dur, name
            from slice
            where track_id = ${trackId}
          `,
        },
      });
      ctx.tracks.registerTrack({
        uri,
        title: packageName,
        tags: {
          trackIds: [trackId],
          kind: SLICE_TRACK_KIND,
        },
        track,
      });
      let workPeriod = workPeriodByGpu.get(gpuId);
      if (workPeriod === undefined) {
        workPeriod = new TrackNode({
          title: `GPU Work Period (GPU ${gpuId})`,
          isSummary: true,
        });
        workPeriodByGpu.set(gpuId, workPeriod);
        ctx.workspace.addChildInOrder(workPeriod);
      }
      workPeriod.addChildInOrder(new TrackNode({title: packageName, uri}));
    }
  }
}

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

import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';

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
      const track = await SliceTrack.createMaterialized({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          src: `
            select ts, dur, name
            from slice
            where track_id = ${trackId}
          `,
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
          },
        }),
      });
      ctx.tracks.registerTrack({
        uri,
        tags: {
          trackIds: [trackId],
          kinds: [SLICE_TRACK_KIND],
        },
        renderer: track,
      });
      let workPeriod = workPeriodByGpu.get(gpuId);
      if (workPeriod === undefined) {
        workPeriod = new TrackNode({
          name: `GPU Work Period (GPU ${gpuId})`,
          isSummary: true,
        });
        workPeriodByGpu.set(gpuId, workPeriod);
        ctx.defaultWorkspace.addChildInOrder(workPeriod);
      }
      workPeriod.addChildInOrder(new TrackNode({name: packageName, uri: uri}));
    }
  }
}

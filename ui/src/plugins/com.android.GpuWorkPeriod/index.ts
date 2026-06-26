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

import {Gpu} from '../../components/gpu';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import GpuPlugin, {SUMMARY_GROUP_SORT_BASE} from '../dev.perfetto.Gpu';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.GpuWorkPeriod';
  static readonly dependencies = [GpuPlugin, StandardGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const {engine} = ctx;

    const gpuCount = (
      await engine.query(`select count(*) as cnt from gpu`)
    ).firstRow({cnt: NUM}).cnt;

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
        t.id as trackId,
        t.uid as uid,
        t.gpu_id as gpuId,
        t.ugpu as ugpu,
        t.machine_id as machineId,
        g.name as gpuName,
        m.name as machineName,
        iif(p.cnt = 1, p.package_name, 'UID ' || t.uid) as packageName
      from android_gpu_work_period_track t
      left join grouped_packages p using (uid)
      left join gpu g on g.id = t.ugpu
      left join machine m on m.id = t.machine_id
      order by t.gpu_id, lower(packageName)
    `);

    const it = result.iter({
      trackId: NUM,
      uid: NUM,
      gpuId: NUM,
      ugpu: NUM_NULL,
      machineId: NUM_NULL,
      gpuName: STR_NULL,
      machineName: STR_NULL,
      packageName: STR,
    });

    const gpuGroup = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'GPU');

    // Cache the work-period group(s) by name so each is created only once.
    const groupsByName = new Map<string, TrackNode>();
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

      // The per-GPU split (when there is more than one GPU) is flattened into
      // the group name rather than adding a second nesting level, keeping the
      // tree one level deep under GPU.
      let groupName = 'Work Period';
      if (gpuCount > 1) {
        const gpu = new Gpu(
          it.ugpu ?? gpuId,
          gpuId,
          it.machineId ?? 0,
          it.gpuName ?? undefined,
          it.machineName ?? undefined,
        );
        groupName = `Work Period (${gpu.displayName})${gpu.maybeMachineLabel()}`;
      }

      let group = groupsByName.get(groupName);
      if (group === undefined) {
        group = new TrackNode({
          name: groupName,
          isSummary: true,
          collapsed: true,
          sortOrder: SUMMARY_GROUP_SORT_BASE,
        });
        groupsByName.set(groupName, group);
        gpuGroup.addChildInOrder(group);
      }
      group.addChildInOrder(new TrackNode({name: packageName, uri}));
    }
  }
}

// Copyright (C) 2025 The Android Open Source Project
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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {NUM, STR} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.CpuPerUid';
  static readonly dependencies = [StandardGroupsPlugin];

  private _topLevelGroup: TrackNode | undefined;

  private async addCpuPerUidTrack(
    ctx: Trace,
    sql: string,
    name: string,
    uri: string,
    group: TrackNode,
    sharing?: string,
  ) {
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: sql,
        columns: ['ts', 'value'],
      },
      columns: {ts: 'ts', value: 'value'},
      options: {
        unit: '%',
        yOverrideMaximum: 100,
        yOverrideMinimum: 0,
        yRangeSharingKey: sharing,
      },
      materialize: false,
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const node = new TrackNode({uri, name});
    group.addChildInOrder(node);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(`INCLUDE PERFETTO MODULE android.cpu.cpu_per_uid;`);
    await this.addSummaryCpuCounters(ctx);
    await this.addCpuCounters(ctx, 50000, 'Major users', 'cpu_per_uid');
    await this.addCpuCounters(ctx, 0, 'All', 'cpu_per_uid_all');
  }

  async addSummaryCpuCounters(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(
      `CREATE PERFETTO TABLE _android_cpu_per_uid_summary AS
      select
        case when t.uid % 100000 < 10000 then 'System' else 'Apps' end as type,
        cluster,
        ts,
        sum(100 * max(0, cpu_ratio)) as value
      from android_cpu_per_uid_track t join android_cpu_per_uid_counter c on t.id = c.track_id
      group by type, cluster, ts
      order by type, cluster, ts;`,
    );

    const tracks = await e.query(
      `select distinct type, cluster
        from _android_cpu_per_uid_summary
        order by type, cluster`,
    );

    const it = tracks.iter({type: STR, cluster: NUM});
    if (it.valid()) {
      const group = new TrackNode({
        name: 'Summary',
        isSummary: true,
      });
      this.topLevelGroup(ctx).addChildInOrder(group);

      for (; it.valid(); it.next()) {
        const name = `${it.type} (${clusterName(it.cluster)})`;
        await this.addCpuPerUidTrack(
          ctx,
          `select ts, value
          from _android_cpu_per_uid_summary
          where type = '${it.type}' and cluster = ${it.cluster}`,
          name,
          `/cpu_per_uid_summary_${it.type}_${it.cluster}`,
          group,
          'cpu-per-uid-summary',
        );
      }
    }
  }

  async addCpuCounters(
    ctx: Trace,
    thresholdMs: number,
    title: string,
    uriPrefix: string,
  ): Promise<void> {
    const e = ctx.engine;
    const tracks = await e.query(
      `select 
          t.id,
          t.cluster,
          ifnull(package_name, 'UID ' || uid) as name,
          sum(diff_ms) as total_cpu_ms
        from android_cpu_per_uid_track t join android_cpu_per_uid_counter c on t.id = c.track_id
        group by t.id, cluster, name
        having total_cpu_ms > ${thresholdMs}
        order by name, cluster`,
    );
    const it = tracks.iter({id: NUM, cluster: NUM, name: STR});
    if (it.valid()) {
      const group = new TrackNode({
        name: title,
        isSummary: true,
      });
      this.topLevelGroup(ctx).addChildInOrder(group);

      for (; it.valid(); it.next()) {
        const name = `${it.name} (${clusterName(it.cluster)})`;
        await this.addCpuPerUidTrack(
          ctx,
          `select
           ts,
           min(100, 100 * cpu_ratio) as value
         from android_cpu_per_uid_counter
         where track_id = ${it.id}`,
          name,
          `/${uriPrefix}_${it.id}`,
          group,
        );
      }
    }
  }

  topLevelGroup(ctx: Trace) {
    if (this._topLevelGroup === undefined) {
      this._topLevelGroup = new TrackNode({
        name: 'CPU per UID',
        isSummary: true,
      });
      ctx.defaultWorkspace.addChildInOrder(this._topLevelGroup);
    }

    return this._topLevelGroup;
  }
}

function clusterName(num: number): string {
  if (num === 0) {
    return 'little';
  } else if (num === 1) {
    return 'mid';
  } else if (num === 2) {
    return 'big';
  }

  return `cl-${num}`;
}

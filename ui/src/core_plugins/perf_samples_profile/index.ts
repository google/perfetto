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

import m from 'mithril';

import {TrackData} from '../../common/track_data';
import {
  Engine,
  LegacyDetailsPanel,
  PERF_SAMPLES_PROFILE_TRACK_KIND,
} from '../../public';
import {LegacyFlamegraphCache} from '../../core/legacy_flamegraph_cache';
import {
  LegacyFlamegraphDetailsPanel,
  profileType,
} from '../../frontend/legacy_flamegraph_panel';
import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {NUM} from '../../trace_processor/query_result';
import {PerfSamplesProfileTrack} from './perf_samples_profile_track';
import {
  LegacySelection,
  PerfSamplesSelection,
} from '../../core/selection_manager';
import {
  QueryFlamegraph,
  QueryFlamegraphAttrs,
  USE_NEW_FLAMEGRAPH_IMPL,
  metricsFromTableOrSubquery,
} from '../../core/query_flamegraph';
import {Monitor} from '../../base/monitor';
import {DetailsShell} from '../../widgets/details_shell';
import {assertExists} from '../../base/logging';
import {Timestamp} from '../../frontend/widgets/timestamp';

export interface Data extends TrackData {
  tsStarts: BigInt64Array;
}

class PerfSamplesProfilePlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const result = await ctx.engine.query(`
      select distinct upid, pid
      from perf_sample join thread using (utid) join process using (upid)
      where callsite_id is not null
    `);
    for (const it = result.iter({upid: NUM, pid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const pid = it.pid;
      ctx.registerTrack({
        uri: `perfetto.PerfSamplesProfile#${upid}`,
        displayName: `Callstacks ${pid}`,
        kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
        upid,
        trackFactory: () => new PerfSamplesProfileTrack(ctx.engine, upid),
      });
    }
    ctx.registerDetailsPanel(new PerfSamplesFlamegraphDetailsPanel(ctx.engine));
  }
}

class PerfSamplesFlamegraphDetailsPanel implements LegacyDetailsPanel {
  private sel?: PerfSamplesSelection;
  private selMonitor = new Monitor([
    () => this.sel?.leftTs,
    () => this.sel?.rightTs,
    () => this.sel?.upid,
    () => this.sel?.type,
  ]);
  private flamegraphAttrs?: QueryFlamegraphAttrs;
  private cache = new LegacyFlamegraphCache('perf_samples');

  constructor(private engine: Engine) {}

  render(sel: LegacySelection) {
    if (sel.kind !== 'PERF_SAMPLES') {
      this.sel = undefined;
      return undefined;
    }
    if (!USE_NEW_FLAMEGRAPH_IMPL.get()) {
      this.sel = undefined;
      return m(LegacyFlamegraphDetailsPanel, {
        cache: this.cache,
        selection: {
          profileType: profileType(sel.type),
          start: sel.leftTs,
          end: sel.rightTs,
          upids: [sel.upid],
        },
      });
    }

    const {leftTs, rightTs, upid} = sel;
    this.sel = sel;
    if (this.selMonitor.ifStateChanged()) {
      this.flamegraphAttrs = {
        engine: this.engine,
        metrics: [
          ...metricsFromTableOrSubquery(
            `
              (
                with agg_callsites as (
                  select p.callsite_id, count() as cnt
                  from perf_sample p
                  join thread t using (utid)
                  where p.ts >= ${leftTs}
                    and p.ts <= ${rightTs}
                    and t.upid = ${upid}
                  group by p.callsite_id
                )
                select
                  c.id,
                  c.parent_id as parentId,
                  ifnull(f.deobfuscated_name, f.name) as name,
                  ifnull(cnt, 0) as self_count
                from stack_profile_callsite c
                join stack_profile_frame f on c.frame_id = f.id
                left join agg_callsites a on c.id = a.callsite_id
              )
            `,
            [
              {
                name: 'Perf Samples',
                unit: '',
                columnName: 'self_count',
              },
            ],
          ),
        ],
      };
    }
    return m(
      '.flamegraph-profile',
      m(
        DetailsShell,
        {
          fillParent: true,
          title: m('.title', 'Perf Samples'),
          description: [],
          buttons: [
            m(
              'div.time',
              `First timestamp: `,
              m(Timestamp, {
                ts: this.sel.leftTs,
              }),
            ),
            m(
              'div.time',
              `Last timestamp: `,
              m(Timestamp, {
                ts: this.sel.rightTs,
              }),
            ),
          ],
        },
        m(QueryFlamegraph, assertExists(this.flamegraphAttrs)),
      ),
    );
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.PerfSamplesProfile',
  plugin: PerfSamplesProfilePlugin,
};

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
import {CPU_PROFILE_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
import {LegacyDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {CpuProfileTrack} from './cpu_profile_track';
import {getThreadUriPrefix} from '../../public/utils';
import {exists} from '../../base/utils';
import {Monitor} from '../../base/monitor';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
  QueryFlamegraphAttrs,
} from '../../core/query_flamegraph';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {assertExists} from '../../base/logging';
import {DetailsShell} from '../../widgets/details_shell';
import {
  CpuProfileSampleSelection,
  LegacySelection,
} from '../../public/selection';
import {getOrCreateGroupForThread} from '../../public/standard_groups';
import {TrackNode} from '../../public/workspace';

class CpuProfile implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      with thread_cpu_sample as (
        select distinct utid
        from cpu_profile_stack_sample
        where utid != 0
      )
      select
        utid,
        tid,
        upid,
        thread.name as threadName
      from thread_cpu_sample
      join thread using(utid)
    `);

    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      tid: NUM_NULL,
      threadName: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      const utid = it.utid;
      const upid = it.upid;
      const threadName = it.threadName;
      const uri = `${getThreadUriPrefix(upid, utid)}_cpu_samples`;
      const displayName = `${threadName} (CPU Stack Samples)`;
      ctx.tracks.registerTrack({
        uri,
        title: displayName,
        tags: {
          kind: CPU_PROFILE_TRACK_KIND,
          utid,
          ...(exists(upid) && {upid}),
        },
        track: new CpuProfileTrack(
          {
            engine: ctx.engine,
            uri,
          },
          utid,
        ),
      });
      const group = getOrCreateGroupForThread(ctx.workspace, utid);
      const track = new TrackNode(uri, displayName);
      track.sortOrder = -40;
      group.insertChildInOrder(track);
    }
    ctx.registerDetailsPanel(
      new CpuProfileSampleFlamegraphDetailsPanel(ctx.engine),
    );
  }
}

class CpuProfileSampleFlamegraphDetailsPanel implements LegacyDetailsPanel {
  private sel?: CpuProfileSampleSelection;
  private selMonitor = new Monitor([() => this.sel?.ts, () => this.sel?.utid]);
  private flamegraphAttrs?: QueryFlamegraphAttrs;

  constructor(private engine: Engine) {}

  render(sel: LegacySelection) {
    if (sel.kind !== 'CPU_PROFILE_SAMPLE') {
      this.sel = undefined;
      return undefined;
    }
    const {ts, utid} = sel;
    this.sel = sel;
    if (this.selMonitor.ifStateChanged()) {
      this.flamegraphAttrs = {
        engine: this.engine,
        metrics: [
          ...metricsFromTableOrSubquery(
            `
              (
                select
                  id,
                  parent_id as parentId,
                  name,
                  mapping_name,
                  source_file,
                  cast(line_number AS text) as line_number,
                  self_count
                from _callstacks_for_cpu_profile_stack_samples!((
                  select p.callsite_id
                  from cpu_profile_stack_sample p
                  where p.ts = ${ts} and p.utid = ${utid}
                ))
              )
            `,
            [
              {
                name: 'CPU Profile Samples',
                unit: '',
                columnName: 'self_count',
              },
            ],
            'include perfetto module callstacks.stack_profile',
            [{name: 'mapping_name', displayName: 'Mapping'}],
            [
              {name: 'source_file', displayName: 'Source File'},
              {name: 'line_number', displayName: 'Line Number'},
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
          title: m('.title', 'CPU Profile Samples'),
          description: [],
          buttons: [
            m('div.time', `Timestamp: `, m(Timestamp, {ts: this.sel.ts})),
          ],
        },
        m(QueryFlamegraph, assertExists(this.flamegraphAttrs)),
      ),
    );
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CpuProfile',
  plugin: CpuProfile,
};

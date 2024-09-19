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
import {Engine} from '../../trace_processor/engine';
import {LegacyDetailsPanel} from '../../public/details_panel';
import {PERF_SAMPLES_PROFILE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {LegacySelection, PerfSamplesSelection} from '../../public/selection';
import {
  QueryFlamegraph,
  QueryFlamegraphAttrs,
  metricsFromTableOrSubquery,
} from '../../core/query_flamegraph';
import {Monitor} from '../../base/monitor';
import {DetailsShell} from '../../widgets/details_shell';
import {assertExists} from '../../base/logging';
import {Timestamp} from '../../frontend/widgets/timestamp';
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

class PerfSamplesProfilePlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    const pResult = await ctx.engine.query(`
      select distinct upid
      from perf_sample
      join thread using (utid)
      where callsite_id is not null and upid is not null
    `);
    for (const it = pResult.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uri = `/process_${upid}/perf_samples_profile`;
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
            engine: ctx.engine,
            uri,
          },
          upid,
        ),
      });
      const group = getOrCreateGroupForProcess(ctx.workspace, upid);
      const track = new TrackNode(uri, title);
      track.sortOrder = -40;
      group.insertChildInOrder(track);
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
      const displayName =
        threadName === null
          ? `Thread Callstacks ${tid}`
          : `${threadName} Callstacks ${tid}`;
      const uri = `${getThreadUriPrefix(upid, utid)}_perf_samples_profile`;
      ctx.tracks.registerTrack({
        uri,
        title: displayName,
        tags: {
          kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
          utid,
          upid: upid ?? undefined,
        },
        track: new ThreadPerfSamplesProfileTrack(
          {
            engine: ctx.engine,
            uri,
          },
          utid,
        ),
      });
      const group = getOrCreateGroupForThread(ctx.workspace, utid);
      const track = new TrackNode(uri, displayName);
      track.sortOrder = -50;
      group.insertChildInOrder(track);
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
    () => this.sel?.utid,
    () => this.sel?.type,
  ]);
  private flamegraphAttrs?: QueryFlamegraphAttrs;

  constructor(private engine: Engine) {}

  render(sel: LegacySelection) {
    if (sel.kind !== 'PERF_SAMPLES') {
      this.sel = undefined;
      return undefined;
    }

    const {leftTs, rightTs, upid, utid} = sel;
    this.sel = sel;
    if (this.selMonitor.ifStateChanged()) {
      this.flamegraphAttrs = {
        engine: this.engine,
        metrics: [
          ...metricsFromTableOrSubquery(
            utid !== undefined
              ? `
                (
                  select
                    id,
                    parent_id as parentId,
                    name,
                    mapping_name,
                    source_file,
                    cast(line_number AS text) as line_number,
                    self_count
                  from _linux_perf_callstacks_for_samples!((
                    select p.callsite_id
                    from perf_sample p
                    where p.ts >= ${leftTs}
                      and p.ts <= ${rightTs}
                      and p.utid = ${utid}
                  ))
                )
              `
              : `
                  (
                    select
                      id,
                      parent_id as parentId,
                      name,
                      mapping_name,
                      source_file,
                      cast(line_number AS text) as line_number,
                      self_count
                    from _linux_perf_callstacks_for_samples!((
                      select p.callsite_id
                      from perf_sample p
                      join thread t using (utid)
                      where p.ts >= ${leftTs}
                        and p.ts <= ${rightTs}
                        and t.upid = ${assertExists(upid)}
                    ))
                  )
                `,
            [
              {
                name: 'Perf Samples',
                unit: '',
                columnName: 'self_count',
              },
            ],
            'include perfetto module linux.perf.samples',
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

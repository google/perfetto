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
import {PERF_SAMPLES_PROFILE_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {
  QueryFlamegraph,
  QueryFlamegraphAttrs,
  metricsFromTableOrSubquery,
} from '../../core/query_flamegraph';
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
import {time} from '../../base/time';

export interface Data extends TrackData {
  tsStarts: BigInt64Array;
}

function makeUriForProc(upid: number) {
  return `/process_${upid}/perf_samples_profile`;
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
        detailsPanel: (sel) => {
          const upid = assertExists(sel.upid);
          const ts = sel.ts;

          const flamegraphAttrs = {
            engine: ctx.engine,
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
                        from _linux_perf_callstacks_for_samples!((
                          select p.callsite_id
                          from perf_sample p
                          join thread t using (utid)
                          where p.ts >= ${ts}
                            and p.ts <= ${ts}
                            and t.upid = ${upid}
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
                  {
                    name: 'source_file',
                    displayName: 'Source File',
                    mergeAggregation: 'ONE_OR_NULL',
                  },
                  {
                    name: 'line_number',
                    displayName: 'Line Number',
                    mergeAggregation: 'ONE_OR_NULL',
                  },
                ],
              ),
            ],
          };

          return {
            render: () => renderDetailsPanel(flamegraphAttrs, ts),
          };
        },
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
        detailsPanel: (sel) => {
          const utid = assertExists(sel.utid);
          const ts = sel.ts;

          const flamegraphAttrs = {
            engine: ctx.engine,
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
                    from _linux_perf_callstacks_for_samples!((
                      select p.callsite_id
                      from perf_sample p
                      where p.ts >= ${ts}
                        and p.ts <= ${ts}
                        and p.utid = ${utid}
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
                  {
                    name: 'source_file',
                    displayName: 'Source File',
                    mergeAggregation: 'ONE_OR_NULL',
                  },
                  {
                    name: 'line_number',
                    displayName: 'Line Number',
                    mergeAggregation: 'ONE_OR_NULL',
                  },
                ],
              ),
            ],
          };

          return {
            render: () => renderDetailsPanel(flamegraphAttrs, ts),
          };
        },
      });
      const group = getOrCreateGroupForThread(ctx.workspace, utid);
      const track = new TrackNode({uri, title, sortOrder: -50});
      group.addChildInOrder(track);
    }
  }

  async onTraceReady(ctx: Trace): Promise<void> {
    await selectPerfSample(ctx);
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

function renderDetailsPanel(flamegraphAttrs: QueryFlamegraphAttrs, ts: time) {
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
              ts,
            }),
          ),
          m(
            'div.time',
            `Last timestamp: `,
            m(Timestamp, {
              ts,
            }),
          ),
        ],
      },
      m(QueryFlamegraph, assertExists(flamegraphAttrs)),
    ),
  );
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.PerfSamplesProfile',
  plugin: PerfSamplesProfilePlugin,
};

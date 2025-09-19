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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {createCpuProfileTrack} from './cpu_profile_track';
import {getThreadUriPrefix} from '../../public/utils';
import {exists} from '../../base/utils';
import {TrackNode} from '../../public/workspace';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
} from '../../components/query_flamegraph';
import {Flamegraph} from '../../widgets/flamegraph';
import {assertExists} from '../../base/logging';

const CPU_PROFILE_TRACK_KIND = 'CpuProfileTrack';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CpuProfile';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(`
      with thread_cpu_sample as (
        select distinct utid
        from cpu_profile_stack_sample
      )
      select
        utid,
        tid,
        upid,
        thread.name as threadName
      from thread_cpu_sample
      join thread using(utid)
      where not is_idle
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
      ctx.tracks.registerTrack({
        uri,
        tags: {
          kind: CPU_PROFILE_TRACK_KIND,
          utid,
          ...(exists(upid) && {upid}),
        },
        renderer: createCpuProfileTrack(ctx, uri, utid),
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({
        uri,
        name: `${threadName} (CPU Stack Samples)`,
        sortOrder: -40,
      });
      group?.addChildInOrder(track);
    }

    ctx.selection.registerAreaSelectionTab(createAreaSelectionTab(ctx));

    ctx.onTraceReady.addListener(async () => {
      await selectCpuProfileCallsite(ctx);
    });
  }
}

function createAreaSelectionTab(trace: Trace) {
  let previousSelection: undefined | AreaSelection;
  let flamegraph: undefined | QueryFlamegraph;

  return {
    id: 'cpu_profile_flamegraph',
    name: 'CPU Profile Sample Flamegraph',
    render(selection: AreaSelection) {
      const changed =
        previousSelection === undefined ||
        !areaSelectionsEqual(previousSelection, selection);

      if (changed) {
        flamegraph = computeCpuProfileFlamegraph(trace, selection);
        previousSelection = selection;
      }

      if (flamegraph === undefined) {
        return undefined;
      }

      return {isLoading: false, content: flamegraph.render()};
    },
  };
}

function computeCpuProfileFlamegraph(trace: Trace, selection: AreaSelection) {
  const utids = [];
  for (const trackInfo of selection.tracks) {
    if (trackInfo?.tags?.kind === CPU_PROFILE_TRACK_KIND) {
      utids.push(trackInfo.tags?.utid);
    }
  }
  if (utids.length === 0) {
    return undefined;
  }
  const metrics = metricsFromTableOrSubquery(
    `
      (
        select
          id,
          parent_id as parentId,
          name,
          mapping_name,
          source_file || ':' || line_number as source_location,
          self_count
        from _callstacks_for_callsites!((
          select p.callsite_id
          from cpu_profile_stack_sample p
          where p.ts >= ${selection.start}
            and p.ts <= ${selection.end}
            and p.utid in (${utids.join(',')})
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
      {
        name: 'source_location',
        displayName: 'Source Location',
        mergeAggregation: 'ONE_OR_SUMMARY',
      },
    ],
  );
  return new QueryFlamegraph(trace, metrics, {
    state: Flamegraph.createDefaultState(metrics),
  });
}

async function selectCpuProfileCallsite(trace: Trace) {
  const profile = await assertExists(trace.engine).query(`
    select utid, upid
    from cpu_profile_stack_sample
    join thread using(utid)
    where callsite_id is not null and not is_idle
    order by ts desc
    limit 1
  `);
  if (profile.numRows() !== 1) return;
  const {utid, upid} = profile.firstRow({utid: NUM, upid: NUM_NULL});

  // Create an area selection over the first process with a perf samples track
  trace.selection.selectArea({
    start: trace.traceInfo.start,
    end: trace.traceInfo.end,
    trackUris: [`${getThreadUriPrefix(upid, utid)}_cpu_samples`],
  });
}

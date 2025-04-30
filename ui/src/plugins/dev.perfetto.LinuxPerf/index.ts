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

import {assertExists} from '../../base/logging';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
} from '../../components/query_flamegraph';
import {PerfettoPlugin} from '../../public/plugin';
import {AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {Trace} from '../../public/trace';
import {
  COUNTER_TRACK_KIND,
  PERF_SAMPLES_PROFILE_TRACK_KIND,
} from '../../public/track_kinds';
import {getThreadUriPrefix} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {Flamegraph} from '../../widgets/flamegraph';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import {
  createProcessPerfSamplesProfileTrack,
  createThreadPerfSamplesProfileTrack,
} from './perf_samples_profile_track';

function makeUriForProc(upid: number) {
  return `/process_${upid}/perf_samples_profile`;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.LinuxPerf';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  async onTraceLoad(trace: Trace): Promise<void> {
    await this.addProcessPerfSamplesTracks(trace);
    await this.addThreadPerfSamplesTracks(trace);
    await this.addPerfCounterTracks(trace);

    trace.onTraceReady.addListener(async () => {
      await selectPerfSample(trace);
    });
  }

  private async addProcessPerfSamplesTracks(trace: Trace) {
    const pResult = await trace.engine.query(`
      SELECT DISTINCT upid
      FROM perf_sample
      JOIN thread USING (utid)
      WHERE
        callsite_id IS NOT NULL AND
        upid IS NOT NULL
    `);

    // Remember all the track URIs so we can use them in the command.
    const trackUris: string[] = [];

    for (const it = pResult.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uri = makeUriForProc(upid);
      trackUris.push(uri);
      const title = `Process Callstacks`;
      trace.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
          upid,
        },
        track: createProcessPerfSamplesProfileTrack(trace, uri, upid),
      });
      const group = trace.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const track = new TrackNode({uri, title, sortOrder: -40});
      group?.addChildInOrder(track);
    }

    // Add a command to select all the perf samples in the trace - it selects
    // the entirety of each process scoped perf sample track.
    trace.commands.registerCommand({
      id: 'dev.perfetto.LinuxPerf#SelectAllPerfSamples',
      name: 'Select all perf samples',
      callback: () => {
        trace.selection.selectArea({
          start: trace.traceInfo.start,
          end: trace.traceInfo.end,
          trackUris,
        });
      },
    });
  }

  private async addThreadPerfSamplesTracks(trace: Trace) {
    const tResult = await trace.engine.query(`
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
      trace.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: PERF_SAMPLES_PROFILE_TRACK_KIND,
          utid,
          upid: upid ?? undefined,
        },
        track: createThreadPerfSamplesProfileTrack(trace, uri, utid),
      });
      const group = trace.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForThread(utid);
      const track = new TrackNode({uri, title, sortOrder: -50});
      group?.addChildInOrder(track);
    }
  }

  private async addPerfCounterTracks(trace: Trace) {
    const perfCountersGroup = new TrackNode({
      title: 'Perf Counters',
      isSummary: true,
    });

    const result = await trace.engine.query(`
      select
        id,
        name,
        unit,
        extract_arg(dimension_arg_set_id, 'cpu') as cpu
      from counter_track
      where type = 'perf_counter'
      order by name, cpu
    `);

    const it = result.iter({
      id: NUM,
      name: STR_NULL,
      unit: STR_NULL,
      cpu: NUM, // Perf counters always have a cpu dimension
    });

    for (; it.valid(); it.next()) {
      const {id: trackId, name, unit, cpu} = it;
      const uri = `/counter_${trackId}`;
      const title = `Cpu ${cpu} ${name}`;

      trace.tracks.registerTrack({
        uri,
        title,
        tags: {
          kind: COUNTER_TRACK_KIND,
          trackIds: [trackId],
          cpu,
        },
        track: new TraceProcessorCounterTrack(
          trace,
          uri,
          {
            yMode: 'rate', // Default to rate mode
            unit: unit ?? undefined,
          },
          trackId,
          title,
        ),
      });
      const trackNode = new TrackNode({
        uri,
        title,
      });
      perfCountersGroup.addChildLast(trackNode);
    }

    if (perfCountersGroup.hasChildren) {
      const hardwareGroup = trace.plugins
        .getPlugin(StandardGroupsPlugin)
        .getOrCreateStandardGroup(trace.workspace, 'HARDWARE');
      hardwareGroup.addChildInOrder(perfCountersGroup);
    }

    trace.selection.registerAreaSelectionTab(createAreaSelectionTab(trace));
  }
}

async function selectPerfSample(trace: Trace) {
  const profile = await assertExists(trace.engine).query(`
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
  trace.selection.selectArea({
    start: trace.traceInfo.start,
    end: trace.traceInfo.end,
    trackUris: [makeUriForProc(upid)],
  });
}

function createAreaSelectionTab(trace: Trace) {
  let previousSelection: undefined | AreaSelection;
  let flamegraph: undefined | QueryFlamegraph;

  return {
    id: 'perf_sample_flamegraph',
    name: 'Perf Sample Flamegraph',
    render(selection: AreaSelection) {
      const changed =
        previousSelection === undefined ||
        !areaSelectionsEqual(previousSelection, selection);

      if (changed) {
        flamegraph = computePerfSampleFlamegraph(trace, selection);
        previousSelection = selection;
      }

      if (flamegraph === undefined) {
        return undefined;
      }

      return {isLoading: false, content: flamegraph.render()};
    },
  };
}

function getUpidsFromPerfSampleAreaSelection(currentSelection: AreaSelection) {
  const upids = [];
  for (const trackInfo of currentSelection.tracks) {
    if (
      trackInfo?.tags?.kind === PERF_SAMPLES_PROFILE_TRACK_KIND &&
      trackInfo.tags?.utid === undefined
    ) {
      upids.push(assertExists(trackInfo.tags?.upid));
    }
  }
  return upids;
}

function getUtidsFromPerfSampleAreaSelection(currentSelection: AreaSelection) {
  const utids = [];
  for (const trackInfo of currentSelection.tracks) {
    if (
      trackInfo?.tags?.kind === PERF_SAMPLES_PROFILE_TRACK_KIND &&
      trackInfo.tags?.utid !== undefined
    ) {
      utids.push(trackInfo.tags?.utid);
    }
  }
  return utids;
}

function computePerfSampleFlamegraph(
  trace: Trace,
  currentSelection: AreaSelection,
) {
  const upids = getUpidsFromPerfSampleAreaSelection(currentSelection);
  const utids = getUtidsFromPerfSampleAreaSelection(currentSelection);
  if (utids.length === 0 && upids.length === 0) {
    return undefined;
  }
  const metrics = metricsFromTableOrSubquery(
    `
      (
        select id, parent_id as parentId, name, self_count
        from _callstacks_for_callsites!((
          select p.callsite_id
          from perf_sample p
          join thread t using (utid)
          where p.ts >= ${currentSelection.start}
            and p.ts <= ${currentSelection.end}
            and (
              p.utid in (${utids.join(',')})
              or t.upid in (${upids.join(',')})
            )
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
  );
  return new QueryFlamegraph(trace, metrics, {
    state: Flamegraph.createDefaultState(metrics),
  });
}

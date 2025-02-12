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
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {
  COUNTER_TRACK_KIND,
  PERF_SAMPLES_PROFILE_TRACK_KIND,
} from '../../public/track_kinds';
import {getThreadUriPrefix} from '../../public/utils';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
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
      select distinct upid
      from perf_sample
      join thread using (utid)
      where callsite_id is not null and upid is not null
    `);
    for (const it = pResult.iter({upid: NUM}); it.valid(); it.next()) {
      const upid = it.upid;
      const uri = makeUriForProc(upid);
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
        type,
        name,
        id,
        unit,
        extract_arg(dimension_arg_set_id, 'cpu') as cpu
      from counter_track
      where type = 'perf_counter'
      order by name, cpu
    `);

    const it = result.iter({
      id: NUM,
      type: STR,
      name: STR_NULL,
      unit: STR_NULL,
      cpu: NUM, // Perf counters always have a cpu dimension
    });

    for (; it.valid(); it.next()) {
      const {type, id: trackId, name, unit, cpu} = it;
      console.log(type, trackId, name, unit, cpu);
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

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

import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';

export default class LinuxPerfPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.LinuxPerf';
  static readonly dependencies = [TraceProcessorTrackPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    const perfCountersGroup = new TrackNode({
      name: 'Perf counters',
      isSummary: true,
    });
    const result = await trace.engine.query(`
      select id, name, unit, cpu
      from perf_counter_track
      order by name, cpu
    `);
    for (
      const it = result.iter({
        id: NUM,
        name: STR_NULL,
        unit: STR_NULL,
        cpu: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const uri = `/counter_${it.id}`;
      const title = it.cpu === null ? `${it.name}` : `Cpu ${it.cpu} ${it.name}`;
      trace.tracks.registerTrack({
        uri,
        tags: {
          kinds: [COUNTER_TRACK_KIND],
          trackIds: [it.id],
          cpu: it.cpu ?? undefined,
        },
        renderer: new TraceProcessorCounterTrack({
          trace,
          uri,
          yMode: 'rate',
          unit: it.unit ?? undefined,
          trackId: it.id,
          trackName: title,
        }),
      });
      perfCountersGroup.addChildLast(new TrackNode({uri, name: title}));
    }
    if (perfCountersGroup.hasChildren) {
      trace.defaultWorkspace.addChildInOrder(perfCountersGroup);
    }
  }
}

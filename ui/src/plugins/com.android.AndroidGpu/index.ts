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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {STR_NULL} from '../../trace_processor/query_result';
import GpuPlugin from '../dev.perfetto.Gpu';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidGpu';
  static readonly dependencies = [GpuPlugin, TraceProcessorTrackPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    // Only apply to Android traces.
    const meta = await ctx.engine.query(`
      select extract_metadata('android_build_fingerprint') as fingerprint
    `);
    const fingerprint = meta.firstRow({fingerprint: STR_NULL}).fingerprint;
    if (fingerprint === null) return;

    // Find gpu_counter tracks registered by the GPU plugin and check if
    // they need RateDelta treatment using metadata from their tags.
    const gpuCounterTracks = ctx.tracks
      .getAllTracks()
      .filter((t) => t.tags?.type === 'gpu_counter');
    console.log(gpuCounterTracks);

    for (const track of gpuCounterTracks) {
      const trackIds = track.tags?.trackIds;
      if (trackIds === undefined || trackIds.length !== 1) continue;
      const trackId = trackIds[0];

      const name = String(track.tags?.name ?? '');
      const unit = String(track.tags?.unit ?? '');
      const description = String(track.tags?.description ?? '');

      // If it's not a rate delta, just keep the track as-is.
      if (!isRateDelta(name, description, unit)) continue;

      // Register a new track with cumulative-sum SQL + rate mode.
      const newUri = `/android_gpu_counter_${trackId}`;
      ctx.tracks.registerTrack({
        uri: newUri,
        description: description || undefined,
        tags: track.tags,
        renderer: new CumulativeSumCounterTrack(
          ctx,
          newUri,
          unit,
          trackId,
          name,
        ),
      });

      // Re-point the existing TrackNode to our new track.
      const node = ctx.defaultWorkspace.getTrackByUri(track.uri);
      if (node !== undefined) {
        node.uri = newUri;
      }
    }
  }
}

// A counter track whose SQL source converts already-delta values into a
// running sum. When combined with yMode 'rate', the base counter track
// will compute (cumsum[t+1] - cumsum[t]) / dt which equals delta / dt,
// giving us the per-second rate we want.
class CumulativeSumCounterTrack extends TraceProcessorCounterTrack {
  constructor(
    trace: Trace,
    uri: string,
    unit: string,
    private readonly tid: number,
    trackName: string,
  ) {
    super(trace, uri, {yMode: 'rate', unit}, tid, trackName);
  }

  override getSqlSource(): string {
    return `
      select
        id,
        ts,
        sum(value) over (order by ts) - value as value,
        arg_set_id
      from counter
      where track_id = ${this.tid}
    `;
  }
}

// Extremely hacky function which determines whether a gpu_counter track should
// use RateDelta interpolation. RateDelta means the raw counter values are
// already deltas and we want to display them as per-second rates. Tracks that
// are NOT RateDelta (i.e. they represent instantaneous values) are left
// unchanged.
//
// This exists entirely because:
//  1) When the protos were designed, a field indicating whether or not a
//     was delta encoded was *not* added. This meant it was possible for some
//     counters to be delta and others to be monotontic without anyone at
//     analysis time knowing about it.
//  2) GPU counters were first added to Perfetto in 2019 timeframe. It's now
//     far too late for us to do something better in the trace processor level
//     about this as it would break backcompat for lots of people who are
//     relying on the existing visualization.
//  3) Because of how AGI used to visualize these counters, there's a general
//     expectation that this is the "correct" way to do it. For that reason,
//     it's also the case that if we inconsistent with AGI, people will think
//     we are doing it wrong even if we are more faithful. To reduce the drift
//     and keep things consistent, we have to bow to the "correct" way to do it
//     even if it is very hacky.
//
// Ideally at some point, we actually add some indication to the proto if this
// is a delta and that way, we can undo the deltafication in trace processor
// and allowing us to slowly get rid of this hack with time.
function isRateDelta(name: string, description: string, unit: string): boolean {
  // Percentage and "per" unit counters are instantaneous.
  if (unit === '%' || unit.includes('/')) {
    return false;
  }

  // Arm GPU counters with "per" in name are instantaneous.
  if (name.includes(' per ')) return false;

  // PowerVR-style counters with certain description patterns are instantaneous.
  if (
    description.includes('Current ') ||
    description.includes(' per ') ||
    description.includes(' over ') ||
    name.includes('Utilization') ||
    description.includes('Percentage')
  ) {
    return false;
  }

  // Qualcomm-style counters are instantaneous.
  if (
    description.includes('during a given sample period') ||
    name.includes('Average')
  ) {
    return false;
  }

  // Everything else: values are deltas, display as rate.
  return true;
}

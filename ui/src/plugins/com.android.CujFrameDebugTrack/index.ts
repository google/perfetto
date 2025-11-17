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
import {
  addDebugSliceTrack,
  DebugSliceTrackArgs,
} from '../../components/tracks/debug_tracks';
import AndroidCujsPlugin from '../com.android.AndroidCujs';

/**
 * Plugin that adds and pins the debug track for frame boundaries in jank CUJs.
 * Example use-case is while debugging per-frame metrics, it can be useful to
 * see the frame boundaries that are considered for calculating the metric for a
 * given blocking call in a CUJ.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'com.android.CujFrameDebugTrack';
  static readonly dependencies = [AndroidCujsPlugin];

  async onTraceLoad(ctx: Trace) {
    ctx.commands.registerCommand({
      id: 'com.android.CujFrameDebugTrack',
      name: 'Debug: Pin CUJ frame boundaries',
      callback: async () => {
        const plugin = ctx.plugins.getPlugin(AndroidCujsPlugin);
        await plugin.pinJankCujs(ctx);

        const INCLUDE_PREQUERY = `
        INCLUDE PERFETTO MODULE android.frame_blocking_calls.blocking_calls_aggregation;
        `;
        await ctx.engine.query(INCLUDE_PREQUERY);
        const frameBoundariesArgs = await this.frameBoundariesConfig();
        addDebugSliceTrack({trace: ctx, ...frameBoundariesArgs});
      },
    });
  }

  private async frameBoundariesConfig(): Promise<
    Pick<DebugSliceTrackArgs, 'data' | 'columns' | 'rawColumns' | 'title'>
  > {
    // Fetch the ts and dur for the extended frame boundaries for all jank CUJs.
    // This table is defined in the
    // android.frame_blocking_calls.blocking_calls_aggregation stdlib module.
    const frameBoundariesQuery = `
       SELECT
         frame_id,
         ts,
         (ts_end - ts) AS dur
       FROM _extended_frame_boundary`;

    return {
      data: {
        sqlSource: frameBoundariesQuery,
        columns: ['frame_id', 'ts', 'dur'],
      },
      columns: {ts: 'ts', dur: 'dur', name: 'frame_id'},
      rawColumns: ['frame_id', 'ts', 'dur'],
      title: 'Frame boundaries',
    };
  }
}

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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {FrameSlice, SliceThreadState} from '../../lynx_perf/types';
import {LYNX_FRAME_JANK_PLUGIN_ID} from '../../lynx_perf/constants';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';

/**
 * Plugin for analyzing and tracking frame jank in Android applications
 *
 * This plugin identifies performance issues in frame rendering by:
 * 1. Correlating Choreographer#doFrame events with RenderThread DrawFrame events
 * 2. Measuring frame durations and identifying jank
 * 3. Building a map of frame performance data for visualization
 */
export default class FrameJankPlugin implements PerfettoPlugin {
  static readonly id = LYNX_FRAME_JANK_PLUGIN_ID;

  /**
   * This hook is called as the trace is loading. At this point the trace is
   * loaded into trace processor and it's ready to process queries. This hook
   * should be used for adding tracks and commands that depend on the trace.
   *
   * It should not be used for finding tracks from other plugins as there is no
   * guarantee those tracks will have been added yet.
   */
  async onTraceLoad(ctx: Trace): Promise<void> {
    // Query thread and track information from trace processor
    const result = await ctx.engine.query(`
      with summary as (
        select
        utid,
        name,
        GROUP_CONCAT(id) AS track_ids
        from thread_track
        join _slice_track_summary USING (id)
        group by utid, parent_id, name
      )
      select
        s.utid,
        thread.upid,
        s.name as trackName,
        thread.name as threadName,
        thread.tid as tid,
        s.track_ids as trackIds,
        thread.is_main_thread as isMainThread,
        k.is_kernel_thread AS isKernelThread
      from summary s
      join _threads_with_kernel_flag k using(utid)
      join thread using (utid)
    `);

    // Build mapping between track URIs and thread states
    const it = result.iter({
      utid: NUM,
      upid: NUM_NULL,
      trackName: STR_NULL,
      trackIds: STR,
      isMainThread: NUM_NULL,
      isKernelThread: NUM,
      threadName: STR_NULL,
      tid: NUM_NULL,
    });
    const trackUriToThreadMap = new Map<string, SliceThreadState>();
    for (; it.valid(); it.next()) {
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const uri = `/slice_${trackIds[0]}`;
      trackUriToThreadMap.set(uri, {
        utid: it.utid,
        upid: it.upid ?? 0,
        tid: it.tid ?? 0,
        trackName: it.trackName ?? '',
        trackId: parseInt(it.trackIds),
        isMainThread: it.isMainThread === 1,
        isKernelThread: it.isKernelThread === 1,
        threadName: it.threadName ?? '',
      });
    }
    lynxPerfGlobals.updateSliceThreadMap(trackUriToThreadMap);

    // Analyze frame performance by correlating UI thread and RenderThread events
    const frameDurationMap = new Map<number, FrameSlice>();
    const sliceThreadStates = Array.from(trackUriToThreadMap.values());
    const mainThreads = sliceThreadStates.filter(
      (value) => !value.isKernelThread && value.isMainThread,
    );
    for (const mainThread of mainThreads) {
      const upid = mainThread.upid;
      const renderThreads = sliceThreadStates.filter(
        (value) =>
          value.upid === upid && value.threadName.startsWith('RenderThread'),
      );

      // Find the most active RenderThread for this process
      let renderThreadTrackId = 0;
      let maxCount = 0;
      for (let i = 0; i < renderThreads.length; i++) {
        const result = await ctx.engine
          .query(`select count(*) as count from slice 
          where track_id=${renderThreads[i].trackId} and name like 'DrawFrame%'`);
        if (result.numRows() > 0) {
          const firstRow = result.firstRow({count: NUM});
          if (firstRow.count > maxCount) {
            maxCount = firstRow.count;
            renderThreadTrackId = renderThreads[i].trackId;
          }
        }
      }
      if (renderThreadTrackId <= 0) {
        continue;
      }

      // Correlate doFrame events with their corresponding DrawFrame events
      const doFrameResult = await ctx.engine.query(`select 
          slice.ts as ts, 
          slice.id as id, 
          slice.dur as dur,
          slice.name as name,
          drawFrame.name as drawFrameName,
          drawFrame.ts as drawFrameTs,
          drawFrame.dur as drawFrameDur 
        from slice 
        inner join slice as drawFrame
        where slice.track_id=${mainThread.trackId} 
              and drawFrame.track_id=${renderThreadTrackId} 
              and slice.name like 'Choreographer#doFrame%' 
              and drawFrame.name like 'DrawFrames%' 
              and drawFrame.ts > slice.ts 
              and drawFrame.ts < slice.ts + slice.dur 
        order by slice.ts`);

      const it = doFrameResult.iter({
        ts: NUM,
        id: NUM,
        dur: NUM,
        name: STR,
        drawFrameName: STR,
        drawFrameTs: NUM,
        drawFrameDur: NUM,
      });
      let preDoFrameBeginTs = 0;
      let preDoFrameEndTs = 0;

      // Process each frame event pair
      for (; it.valid(); it.next()) {
        // Skip nested doFrame events
        if (it.ts > preDoFrameBeginTs && it.ts + it.dur < preDoFrameEndTs) {
          continue;
        }

        // Verify event tokens match
        if (!this.doFrameDrawFrameTokenCheck(it.name, it.drawFrameName)) {
          console.error(
            'doFrame and drawFrame token do not match, doFrame timestamp:' +
              it.ts,
          );
          continue;
        }

        // Calculate frame duration (accounting for DrawFrame extending beyond doFrame)
        const doFrameEnd = it.ts + it.dur;
        const drawFrameEnd = it.drawFrameTs + it.drawFrameDur;
        let oldDur = frameDurationMap.get(it.ts)?.dur ?? 0;
        if (drawFrameEnd > doFrameEnd) {
          oldDur = Math.max(oldDur, drawFrameEnd - it.ts);
        } else {
          oldDur = it.dur;
        }
        frameDurationMap.set(it.ts, {dur: oldDur, id: it.id});
        preDoFrameBeginTs = it.ts;
        preDoFrameEndTs = it.ts + it.dur;
      }
      lynxPerfGlobals.updateFrameDurationMap(frameDurationMap);
    }
  }

  /**
   * Verifies that doFrame and DrawFrame events belong to the same frame
   * by comparing their token values (last part of the event name)
   * @param doFrameName - Name of the doFrame event
   * @param drawFrameName - Name of the DrawFrame event
   * @returns True if tokens match or couldn't be extracted
   */
  private doFrameDrawFrameTokenCheck(
    doFrameName: string,
    drawFrameName: string,
  ) {
    const doFrameNameArray = doFrameName.split(' ');
    const drawFrameNameArray = drawFrameName.split(' ');
    if (doFrameNameArray.length >= 2 && drawFrameNameArray.length >= 2) {
      return (
        doFrameNameArray[doFrameNameArray.length - 1] ===
        drawFrameNameArray[drawFrameNameArray.length - 1]
      );
    }
    return true;
  }
}

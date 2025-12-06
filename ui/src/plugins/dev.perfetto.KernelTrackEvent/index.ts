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

import {assertExists} from '../../base/logging';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {COUNTER_TRACK_KIND, SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';
import {TraceProcessorCounterTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_counter_track';
import {createTraceProcessorSliceTrack} from '../dev.perfetto.TraceProcessorTrack/trace_processor_slice_track';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.KernelTrackEvent';
  static readonly dependencies = [
    ProcessThreadGroupsPlugin,
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];
  static readonly description =
    'Renders tracks derived from ftrace events that follow a specific convention.';

  private kernelTrackEventsNode?: TrackNode;

  async onTraceLoad(trace: Trace): Promise<void> {
    await this.addTracks(trace);
  }

  private async addTracks(ctx: Trace) {
    // Four types of scopes for both slice and counter tracks, exactly one scope
    // dimension should be set per row.
    const res = await ctx.engine.query(`
      WITH
        kernel_tracks AS (
          SELECT
            id,
            name,
            type,
            extract_arg(dimension_arg_set_id, 'utid') AS utid,
            extract_arg(dimension_arg_set_id, 'upid') AS upid,
            extract_arg(dimension_arg_set_id, 'cpu') AS cpu,
            extract_arg(dimension_arg_set_id, 'scope') AS scope
          FROM track
          WHERE
            type IN ('kernel_trackevent_thread_slice',
                     'kernel_trackevent_process_slice',
                     'kernel_trackevent_cpu_slice',
                     'kernel_trackevent_custom_slice',
                     'kernel_trackevent_thread_counter',
                     'kernel_trackevent_process_counter',
                     'kernel_trackevent_cpu_counter',
                     'kernel_trackevent_custom_counter')
        )
      SELECT
        kt.id as trackId,
        kt.name,
        kt.utid,
        kt.upid,
        kt.cpu,
        kt.scope,
        kt.type IN (
          'kernel_trackevent_thread_counter',
          'kernel_trackevent_process_counter',
          'kernel_trackevent_cpu_counter',
          'kernel_trackevent_custom_counter'
        ) as isCounter,
        t.tid as tid,
        p.pid as pid
      FROM kernel_tracks AS kt
      LEFT JOIN process AS p
        USING (upid)
      LEFT JOIN thread AS t
        USING (utid)
      ORDER BY COALESCE(t.tid, p.pid, kt.cpu, kt.scope)
    `);

    const it = res.iter({
      trackId: NUM,
      name: STR_NULL,
      utid: NUM_NULL,
      upid: NUM_NULL,
      cpu: NUM_NULL,
      scope: NUM_NULL,
      isCounter: NUM,
      tid: LONG_NULL,
      pid: LONG_NULL,
    });

    for (; it.valid(); it.next()) {
      const {trackId, name, utid, upid, cpu, isCounter, scope, tid, pid} = it;

      const displayTrackName = this.getTrackName(
        assertExists(name),
        pid ?? undefined,
        tid ?? undefined,
        cpu ?? undefined,
        scope ?? undefined,
      );

      // Register track.
      const uri = `/kernel_trackevent_${trackId}`;
      if (isCounter) {
        ctx.tracks.registerTrack({
          uri,
          tags: {
            kinds: [COUNTER_TRACK_KIND],
            trackIds: [trackId],
            upid: upid ?? undefined,
            utid: utid ?? undefined,
            cpu: cpu ?? undefined,
            trackScope: scope ?? undefined,
          },
          renderer: new TraceProcessorCounterTrack(
            ctx,
            uri,
            {},
            trackId,
            displayTrackName,
          ),
        });
      } else {
        // slice track
        ctx.tracks.registerTrack({
          uri,
          tags: {
            kinds: [SLICE_TRACK_KIND],
            trackIds: [trackId],
            upid: upid ?? undefined,
            utid: utid ?? undefined,
            cpu: cpu ?? undefined,
            trackScope: scope ?? undefined,
          },
          renderer: await createTraceProcessorSliceTrack({
            trace: ctx,
            uri,
            trackIds: [trackId],
          }),
        });
      }

      // Create TrackNode and add it to the relevant group.
      // Order calculation copied from dev.perfetto.TraceProcessorTrack.
      const order = utid !== undefined || upid !== undefined ? 20 : 0;
      const track = new TrackNode({
        uri,
        name: displayTrackName,
        sortOrder: order,
      });
      const parent = this.getOrCreateParentTrackNode(
        ctx,
        upid ?? undefined,
        utid ?? undefined,
        cpu ?? undefined,
      );
      parent.addChildInOrder(track);
    }
  }

  private getTrackName(
    name: string,
    pid: bigint | undefined,
    tid: bigint | undefined,
    cpu: number | undefined,
    scope: number | undefined,
  ): string {
    if (pid !== undefined) {
      return `${name} (${pid})`;
    }
    if (tid !== undefined) {
      return `${name} (${tid})`;
    }
    if (cpu !== undefined) {
      return `${name} (${cpu})`;
    }
    if (scope !== undefined) {
      return `${name} (${scope})`;
    }
    return name;
  }

  private getOrCreateParentTrackNode(
    ctx: Trace,
    upid: number | undefined,
    utid: number | undefined,
    cpu: number | undefined,
  ): TrackNode {
    if (upid !== undefined) {
      return assertExists(
        ctx.plugins
          .getPlugin(ProcessThreadGroupsPlugin)
          .getGroupForProcess(assertExists(upid)),
      );
    }
    if (utid !== undefined) {
      return assertExists(
        ctx.plugins
          .getPlugin(ProcessThreadGroupsPlugin)
          .getGroupForThread(assertExists(utid)),
      );
    }
    if (cpu !== undefined) {
      return assertExists(
        ctx.plugins
          .getPlugin(StandardGroupsPlugin)
          .getOrCreateStandardGroup(ctx.defaultWorkspace, 'CPU'),
      );
    }
    // custom-scoped event: "Kernel -> Kernel track events".
    if (this.kernelTrackEventsNode === undefined) {
      const kernelGroup = assertExists(
        ctx.plugins
          .getPlugin(StandardGroupsPlugin)
          .getOrCreateStandardGroup(ctx.defaultWorkspace, 'KERNEL'),
      );
      this.kernelTrackEventsNode = new TrackNode({
        name: 'Kernel track events',
        isSummary: true,
      });
      kernelGroup.addChildInOrder(this.kernelTrackEventsNode);
    }
    return this.kernelTrackEventsNode;
  }
}

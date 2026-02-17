// Copyright (C) 2024 The Android Open Source Project
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

import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {uuidv4} from '../../base/uuid';
import {LONG, LONG_NULL, STR} from '../../trace_processor/query_result';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {TimeSpan} from '../../base/time';

export default class AndroidInputEvents implements PerfettoPlugin {
  static readonly id = 'com.android.InputEvents';
  static readonly dependencies = [StandardGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.input;
      INCLUDE PERFETTO MODULE intervals.overlap;
    `);

    ctx.commands.registerCommand({
      id: 'com.android.InputEvents.visualizeOverlaps',
      name: 'Input Events: Visualize event overlaps (over selection)',
      callback: () => this.visualizeOverlaps(ctx),
    });

    const cnt = await ctx.engine.query(`
      SELECT
        COUNT(*) AS cnt
      FROM slice
      WHERE name GLOB 'UnwantedInteractionBlocker::notifyMotion*'
    `);
    if (cnt.firstRow({cnt: LONG}).cnt == 0n) {
      return;
    }

    const uri = 'com.android.InputEvents#InputEventsTrack';
    const track = await SliceTrack.createMaterialized({
      trace: ctx,
      uri,
      dataset: new SourceDataset({
        src: `
          SELECT
            read_time AS ts,
            end_to_end_latency_dur AS dur,
            CONCAT(event_type, ' ', event_action, ': ', process_name, ' (', input_event_id, ')') as name
          FROM android_input_events
          WHERE end_to_end_latency_dur IS NOT NULL
        `,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const node = new TrackNode({uri, name: 'Input Events'});
    const group = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'USER_INTERACTION');
    group.addChildInOrder(node);
  }

  async visualizeOverlaps(ctx: Trace): Promise<void> {
    const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);

    const parentUri = `com.android.InputEvents.event_overlaps_parent.${uuidv4()}`;
    const parentNode = await this.createTrack(
        ctx,
        parentUri,
        `
        SELECT *
        FROM intervals_overlap_count!(
          (${this.getOverlappingEventsSubquery(window)}),
          dispatch_ts,
          total_latency_dur
        )
      `,
        'Input Events',
        'Number of concurrent input events (from input dispatch to input ACK ' +
            'received).',
    );
    ctx.defaultWorkspace.pinnedTracksNode.addChildLast(parentNode);

    const processes = await ctx.engine.query(`
      WITH
        process_peaks AS (
          SELECT
            group_name AS upid,
            MAX(value) AS peak
          FROM intervals_overlap_count_by_group!(
            (${this.getOverlappingEventsSubquery(window)}),
            dispatch_ts,
            total_latency_dur,
            upid
          )
          GROUP BY upid
        )
      SELECT
        pp.upid,
        p.name AS process_name
      FROM process_peaks pp
      JOIN process p USING (upid)
      ORDER BY pp.peak DESC
    `);

    for (const it = processes.iter({upid: LONG, process_name: STR});
         it.valid(); it.next()) {
      const upid = Number(it.upid);
      const processName = it.process_name;

      const processNode =
          await this.createProcessTrack(ctx, window, upid, processName);
      parentNode.addChildLast(processNode);

      const channels = await ctx.engine.query(`
        SELECT
          group_name AS event_channel
        FROM intervals_overlap_count_by_group!(
          (${this.getOverlappingEventsSubquery(window, undefined, upid)}),
          dispatch_ts,
          total_latency_dur,
          event_channel
        )
        GROUP BY event_channel
        ORDER BY MAX(value) DESC
      `);

      const channelTrackPromises: Promise<TrackNode>[] = [];
      for (const channelIt = channels.iter({event_channel: STR});
           channelIt.valid(); channelIt.next()) {
        const channel = channelIt.event_channel;
        channelTrackPromises.push(
            this.createChannelTrack(ctx, window, channel, upid));
      }

      const channelTracks = await Promise.all(channelTrackPromises);

      for (const node of channelTracks) {
        processNode.addChildLast(node);
      }
    }
  }

  private async createChannelTrack(
    ctx: Trace,
    window: TimeSpan,
    channel: string,
    upid: number,
  ): Promise<TrackNode> {
    const uri = `com.android.InputEvents.event_overlaps.proc_${upid}.${channel}.${uuidv4()}`;
    return this.createTrack(
      ctx,
      uri,
      `
        SELECT *
        FROM intervals_overlap_count_by_group!(
          (${this.getOverlappingEventsSubquery(window, channel, upid)}),
          dispatch_ts,
          total_latency_dur,
          event_channel
        )
      `,
      `Channel: ${channel}`,
      `Number of concurrent input events on the ${channel} channel (from input dispatch to input ACK received).`,
    );
  }

  private async createProcessTrack(
    ctx: Trace,
    window: TimeSpan,
    upid: number,
    processName: string,
  ): Promise<TrackNode> {
    const uri = `com.android.InputEvents.event_overlaps.proc_${upid}.${uuidv4()}`;
    return this.createTrack(
      ctx,
      uri,
      `
        SELECT *
        FROM intervals_overlap_count!(
          (${this.getOverlappingEventsSubquery(window, undefined, upid)}),
          dispatch_ts,
          total_latency_dur
        )
      `,
      `Process: ${processName} (${upid})`,
      `Number of concurrent input events in ${processName} (from input dispatch to input ACK received).`,
    );
  }

  private async createTrack(
    ctx: Trace,
    uri: string,
    sqlSource: string,
    name: string,
    description: string,
    removable = true,
  ): Promise<TrackNode> {
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      materialize: false,
      data: {
        sqlSource,
      },
      columns: {
        ts: 'ts',
        value: 'value',
      },
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
      description,
    });

    return new TrackNode({
      uri,
      name,
      removable,
    });
  }

  private getOverlappingEventsSubquery(
    window: TimeSpan,
    channel?: string,
    upid?: number,
  ): string {
    const channelConstraint = channel ? `AND event_channel = '${channel}'` : '';
    const upidConstraint = upid !== undefined ? `AND upid = ${upid}` : '';
    return `
      SELECT
        upid,
        process_name,
        event_channel,
        MAX(dispatch_ts, ${window.start}) AS dispatch_ts,
        MIN(dispatch_ts + total_latency_dur, ${window.end}) - MAX(dispatch_ts, ${window.start}) AS total_latency_dur
      FROM android_input_events
      WHERE
        total_latency_dur IS NOT NULL AND
        dispatch_ts < ${window.end} AND dispatch_ts + total_latency_dur > ${window.start}
        ${channelConstraint}
        ${upidConstraint}
    `;
  }
}

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
    const rootNode = await this.createRootTrack(ctx, window);
    ctx.defaultWorkspace.pinnedTracksNode.addChildLast(rootNode);

    const processes = await this.getProcesses(ctx, window);
    const processTrackPromises: Promise<TrackNode>[] = [];

    for (
      const it = processes.iter({upid: LONG, process_name: STR});
      it.valid();
      it.next()
    ) {
      const upid = Number(it.upid);
      const processName = it.process_name;
      processTrackPromises.push(
        this.createProcessTrack(ctx, window, upid, processName),
      );
    }

    const processTracks = await Promise.all(processTrackPromises);
    for (const processTrack of processTracks) {
      rootNode.addChildLast(processTrack);
    }
  }

  private async createRootTrack(
    ctx: Trace,
    window: TimeSpan,
  ): Promise<TrackNode> {
    const uri = `com.android.InputEvents.event_overlaps_parent.${uuidv4()}`;
    const sqlSource = this.getOverlapSqlSource(window);
    return this.createTrack(
      ctx,
      uri,
      sqlSource,
      'Input Events',
      'Number of concurrent input events (from input dispatch to input ACK received).',
    );
  }

  private async getProcesses(ctx: Trace, window: TimeSpan) {
    return ctx.engine.query(`
      WITH
        process_peaks AS (
          SELECT
            group_name AS upid,
            MAX(value) AS peak
          FROM intervals_overlap_count_by_group!((${this.getEventsSubquery(window)}), dispatch_ts, total_latency_dur, upid)
          GROUP BY upid
          HAVING MAX(value) > 0
        )
      SELECT
        pp.upid,
        p.name AS process_name
      FROM process_peaks pp
      JOIN process p USING (upid)
      ORDER BY pp.peak DESC
    `);
  }

  private async createProcessTrack(
    ctx: Trace,
    window: TimeSpan,
    upid: number,
    processName: string,
  ): Promise<TrackNode> {
    const uri = `com.android.InputEvents.event_overlaps.proc_${upid}.${uuidv4()}`;

    const channels = await this.getChannels(ctx, window, upid);
    const numberOfChannels = channels.numRows();
    const plural = numberOfChannels === 1 ? '' : 's';
    const name = `${processName} ${upid} (${numberOfChannels} channel${plural})`;

    const sqlSource = this.getOverlapSqlSource(window, [`upid = ${upid}`]);
    const processNode = await this.createTrack(
      ctx,
      uri,
      sqlSource,
      name,
      `Number of concurrent input events received by process ${processName} ${upid} (from input dispatch to input ACK received).`,
    );

    const channelTrackPromises: Promise<TrackNode>[] = [];
    for (
      const it = channels.iter({event_channel: STR});
      it.valid();
      it.next()
    ) {
      const channel = it.event_channel;
      channelTrackPromises.push(
        this.createChannelTrack(ctx, window, channel, upid),
      );
    }

    const channelTracks = await Promise.all(channelTrackPromises);
    for (const channelTrack of channelTracks) {
      processNode.addChildLast(channelTrack);
    }
    return processNode;
  }

  private async getChannels(ctx: Trace, window: TimeSpan, upid: number) {
    return ctx.engine.query(`
      SELECT
        group_name AS event_channel
      FROM intervals_overlap_count_by_group!((${this.getEventsSubquery(window, [`upid = ${upid}`])}), dispatch_ts, total_latency_dur, event_channel)
      GROUP BY event_channel
      HAVING MAX(value) > 0
      ORDER BY MAX(value) DESC
    `);
  }

  private async createChannelTrack(
    ctx: Trace,
    window: TimeSpan,
    channel: string,
    upid: number,
  ): Promise<TrackNode> {
    const uri = `com.android.InputEvents.event_overlaps.proc_${upid}.${channel}.${uuidv4()}`;
    const sqlSource = this.getOverlapSqlSource(window, [
      `upid = ${upid}`,
      `event_channel = '${channel}'`,
    ]);
    return this.createTrack(
      ctx,
      uri,
      sqlSource,
      `Channel: ${channel}`,
      `Number of concurrent input events on the ${channel} channel (from input dispatch to input ACK received).`,
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

  private getOverlapSqlSource(
    window: TimeSpan,
    whereClauses: string[] = [],
  ): string {
    const subquery = this.getEventsSubquery(window, whereClauses);
    return `
      SELECT *
      FROM intervals_overlap_count!(
        (${subquery}),
        dispatch_ts,
        total_latency_dur
      )
    `;
  }

  private getEventsSubquery(
    window: TimeSpan,
    whereClauses: string[] = [],
  ): string {
    const whereClause =
      whereClauses.length > 0 ? `AND ${whereClauses.join(' AND ')}` : '';
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
        ${whereClause}
    `;
  }
}

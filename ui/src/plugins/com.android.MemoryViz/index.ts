// Copyright (C) 2026 The Android Open Source Project
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
import {TrackNode} from '../../public/workspace';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import {
  BreakdownTrackAggType,
  BreakdownTracks,
} from '../../components/tracks/breakdown_tracks';
import {uuidv4} from '../../base/uuid';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import type {TimeSpan} from '../../base/time';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {HSLColor} from '../../base/color';
import {makeColorScheme} from '../../components/colorizer';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';

const KSWAPD_COLOR = makeColorScheme(new HSLColor('#2196F3')); // Blue 500

export default class MemoryViz implements PerfettoPlugin {
  static readonly id = 'com.android.MemoryViz';
  static readonly dependencies = [StandardGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const memoryGroup = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'MEMORY');

    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE intervals.overlap;
      INCLUDE PERFETTO MODULE slices.with_context;
      INCLUDE PERFETTO MODULE android.memory.lmk;
    `);

    await this.addKswapdTrack(ctx, memoryGroup);
    await this.addDirectReclaimTracks(ctx, memoryGroup);
    await this.addLmkTracks(ctx, memoryGroup);

    ctx.commands.registerCommand({
      id: `com.android.visualizeMemory`,
      name: 'Memory: Visualize (over selection)',
      callback: async () => {
        await ctx.engine.query(`
          INCLUDE PERFETTO MODULE intervals.intersect;
          INCLUDE PERFETTO MODULE android.memory.memory_breakdown;
        `);
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);

        const tracks = [
          ['mem.rss.file', 'RSS File'],
          ['mem.rss.shmem', 'RSS Shmem'],
          ['mem.dmabuf_rss', 'DMA buffer RSS'],
          ['mem.heap', 'Heap Size'],
          ['mem.locked', 'Locked Memory'],
          ['GPU Memory', 'GPU Memory'],
        ];

        const trackPromises: Promise<TrackNode | undefined>[] = [
          this.createRssAnonSwapTrack(ctx, window),
        ];
        trackPromises.push(
          ...tracks.map(([trackName, displayName]) =>
            this.createBreakdownTrack(ctx, window, trackName, displayName),
          ),
        );
        const createdTracks = await Promise.all(trackPromises);

        for (const track of createdTracks) {
          if (track) {
            ctx.defaultWorkspace.pinnedTracksNode.addChildLast(track);
          }
        }
      },
    });
  }

  private async addKswapdTrack(ctx: Trace, parent: TrackNode): Promise<void> {
    const tableName = 'memory_viz_kswapd';
    await createPerfettoTable({
      engine: ctx.engine,
      name: tableName,
      as: `
        SELECT
          row_number() OVER (ORDER BY ts) AS id,
          ts,
          dur,
          thread.name AS name
        FROM sched
        JOIN thread USING (utid)
        WHERE thread.name GLOB 'kswapd0*' AND dur > 0
      `,
    });

    const rowCount = await ctx.engine.query(
      `SELECT COUNT(*) AS n FROM ${tableName}`,
    );
    if (rowCount.firstRow({n: NUM}).n === 0) {
      return;
    }

    const uri = `${MemoryViz.id}#kswapd`;
    ctx.tracks.registerTrack({
      uri,
      description:
        'Shows when the background page reclaim daemon (kswapd) is running on a CPU. ',
      renderer: SliceTrack.create({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          src: tableName,
          schema: {id: NUM, ts: LONG, dur: LONG, name: STR},
        }),
        colorizer: () => KSWAPD_COLOR,
      }),
    });
    parent.addChildInOrder(
      new TrackNode({uri, name: 'Kswapd', sortOrder: 101}),
    );
  }

  private async addDirectReclaimTracks(
    ctx: Trace,
    parent: TrackNode,
  ): Promise<void> {
    const tableName = 'memory_viz_direct_reclaim';
    await createPerfettoTable({
      engine: ctx.engine,
      name: tableName,
      as: `
        SELECT id, ts, dur, name, process_name, thread_name
        FROM thread_slice
        WHERE name GLOB 'mm_vmscan_direct_reclaim' AND dur > 0
      `,
    });

    const rowCount = await ctx.engine.query(
      `SELECT COUNT(*) AS n FROM ${tableName}`,
    );
    if (rowCount.firstRow({n: NUM}).n === 0) {
      return;
    }

    const breakdowns = new BreakdownTracks({
      trace: ctx,
      trackTitle: 'Direct Reclaim',
      description:
        'Shows synchronous page reclaim events.' +
        'This usually indicates severe memory pressure.',
      aggregationType: BreakdownTrackAggType.COUNT,
      aggregation: {
        columns: ['process_name', 'thread_name'],
        tsCol: 'ts',
        durCol: 'dur',
        tableName,
      },
      slice: {
        columns: ['name'],
        tsCol: 'ts',
        durCol: 'dur',
        tableName,
      },
      sliceIdColumn: 'id',
      sortTracks: true,
    });

    const directReclaimNode = await breakdowns.createTracks();
    directReclaimNode.sortOrder = 102;
    parent.addChildInOrder(directReclaimNode);
  }

  private async addLmkTracks(ctx: Trace, parent: TrackNode): Promise<void> {
    const tableName = 'memory_viz_lmk_slices';
    await createPerfettoTable({
      engine: ctx.engine,
      name: tableName,
      as: `
        SELECT
          row_number() OVER (ORDER BY ts) AS id,
          ts,
          0 AS dur,
          upid,
          pid,
          COALESCE(process_name, 'Unknown') AS process_name,
          oom_score_adj,
          android_oom_adj_score_to_bucket_name(oom_score_adj) AS oom_bucket,
          kill_reason
        FROM android_lmk_events
      `,
    });

    const buckets = await ctx.engine.query(`
      SELECT DISTINCT oom_bucket
      FROM ${tableName}
      WHERE oom_bucket IS NOT NULL
      ORDER BY oom_bucket
    `);
    if (buckets.numRows() === 0) {
      return;
    }

    const lmkUri = `${MemoryViz.id}#lmk`;
    ctx.tracks.registerTrack({
      uri: lmkUri,
      description:
        'Shows Android Low Memory Killer (LMK) events. ' +
        'Each event marks a process kill.',
      renderer: SliceTrack.create({
        trace: ctx,
        uri: lmkUri,
        dataset: new SourceDataset({
          src: tableName,
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG,
            upid: NUM_NULL,
            pid: NUM_NULL,
            process_name: STR,
            oom_score_adj: NUM,
            oom_bucket: STR,
            kill_reason: STR_NULL,
          },
        }),
        sliceName: (row) => row.process_name,
      }),
    });
    const lmkGroup = new TrackNode({
      uri: lmkUri,
      name: 'LMK',
      isSummary: true,
      sortOrder: 100,
    });
    parent.addChildInOrder(lmkGroup);

    for (const it = buckets.iter({oom_bucket: STR}); it.valid(); it.next()) {
      const bucket = it.oom_bucket;
      const uri = `${MemoryViz.id}#lmk.${bucket}`;
      ctx.tracks.registerTrack({
        uri,
        description: `Low Memory Killer events for processes in the '${bucket}' OOM adjustment bucket.`,
        renderer: SliceTrack.create({
          trace: ctx,
          uri,
          dataset: new SourceDataset({
            src: tableName,
            schema: {
              id: NUM,
              ts: LONG,
              dur: LONG,
              upid: NUM_NULL,
              pid: NUM_NULL,
              process_name: STR,
              oom_score_adj: NUM,
              oom_bucket: STR,
              kill_reason: STR_NULL,
            },
            filter: {col: 'oom_bucket', eq: bucket},
          }),
          sliceName: (row) => row.process_name,
        }),
      });
      lmkGroup.addChildInOrder(new TrackNode({uri, name: bucket}));
    }
  }

  private async createTrack(
    ctx: Trace,
    uri: string,
    sqlSource: string,
    name: string,
    description: string,
    removable = true,
    sortOrder?: number,
  ): Promise<TrackNode> {
    const track = CounterTrack.create({
      trace: ctx,
      uri,
      sqlSource,
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
      sortOrder,
    });
  }

  private async createRssAnonSwapTrack(
    ctx: Trace,
    window: TimeSpan,
  ): Promise<TrackNode | undefined> {
    const uri = `${MemoryViz.id}.rss_anon_swap.${uuidv4()}`;
    const sqlSource = this.getSqlSource(window, [
      `memory_track_name IN ('mem.rss.anon', 'mem.swap')`,
    ]);
    const rootNode = await this.createTrack(
      ctx,
      uri,
      sqlSource,
      'RSS Anon + Swap',
      'Sum of mem.rss.anon and mem.swap memory across all processes.',
      true,
    );

    const rssAnonNode = await this.createBreakdownTrack(
      ctx,
      window,
      'mem.rss.anon',
      'RSS Anon',
    );
    const swapNode = await this.createBreakdownTrack(
      ctx,
      window,
      'mem.swap',
      'Swap',
    );

    if (rssAnonNode) {
      rootNode.addChildLast(rssAnonNode);
    }
    if (swapNode) {
      rootNode.addChildLast(swapNode);
    }

    return rootNode;
  }

  private async createBreakdownTrack(
    ctx: Trace,
    window: TimeSpan,
    trackName: string,
    name: string,
  ): Promise<TrackNode | undefined> {
    const uri = `${MemoryViz.id}.${trackName}.${uuidv4()}`;
    const sqlSource = this.getSqlSource(window, [
      `memory_track_name = '${trackName}'`,
    ]);
    const breakdownNode = await this.createTrack(
      ctx,
      uri,
      sqlSource,
      name,
      `Sum of ${trackName} memory across all processes.`,
      true,
    );

    const buckets = await ctx.engine.query(`
      SELECT DISTINCT bucket
      FROM android_process_memory_intervals_by_oom_bucket
      WHERE memory_track_name = '${trackName}'
      ORDER BY bucket ASC
    `);

    for (const iter = buckets.iter({}); iter.valid(); iter.next()) {
      const bucket = iter.get('bucket') as string;
      const bucketNode = await this.createOomBucketTrack(
        ctx,
        window,
        trackName,
        bucket,
      );
      if (bucketNode) {
        breakdownNode.addChildInOrder(bucketNode);
      }
    }

    return breakdownNode;
  }

  private async createOomBucketTrack(
    ctx: Trace,
    window: TimeSpan,
    trackName: string,
    bucket: string,
  ): Promise<TrackNode | undefined> {
    const processes = await ctx.engine.query(`
      SELECT
        upid,
        pid,
        process_name,
        MAX(zygote_adjusted_value) AS max_value
      FROM android_process_memory_intervals_by_oom_bucket
      WHERE
        bucket = '${bucket}' AND
        memory_track_name = '${trackName}' AND
        ts < ${window.end} AND
        ts + dur > ${window.start}
      GROUP BY upid, pid, process_name
      HAVING max_value > 0
      ORDER BY max_value DESC
    `);

    const numProcesses = processes.numRows();
    if (numProcesses === 0) {
      return undefined;
    }

    const plural = numProcesses === 1 ? '' : 'es';
    const name = `${bucket} (${numProcesses} process${plural})`;

    const uri = `${MemoryViz.id}.${trackName}.${bucket}.${uuidv4()}`;
    const sqlSource = this.getSqlSource(window, [
      `bucket = '${bucket}'`,
      `memory_track_name = '${trackName}'`,
    ]);
    const peak = await ctx.engine.query(
      `SELECT MAX(value) AS peak FROM (${sqlSource})`,
    );
    const peakValue = peak.iter({}).get('peak') as number;

    const bucketNode = await this.createTrack(
      ctx,
      uri,
      sqlSource,
      name,
      `Sum of ${trackName} memory across all processes while in the '${
        bucket
      }' OOM bucket (0 otherwise).`,
      true,
      -peakValue, // Sort buckets in descending order of peak memory usage
    );

    for (
      const procIter = processes.iter({});
      procIter.valid();
      procIter.next()
    ) {
      const upid = procIter.get('upid') as number;
      const pid = procIter.get('pid') as number;
      const processName = procIter.get('process_name') as string;
      const processNode = await this.createSingleProcessTrack(
        ctx,
        window,
        upid,
        pid,
        processName,
        trackName,
        bucket,
      );
      bucketNode.addChildLast(processNode);
    }
    return bucketNode;
  }

  private async createSingleProcessTrack(
    ctx: Trace,
    window: TimeSpan,
    upid: number,
    pid: number,
    processName: string,
    trackName: string,
    bucket: string,
  ): Promise<TrackNode> {
    const name = `${processName} ${pid}`;
    const uri = `${MemoryViz.id}.process.${upid}.${trackName}.${uuidv4()}`;
    const sqlSource = this.getSqlSource(window, [
      `bucket = '${bucket}'`,
      `memory_track_name = '${trackName}'`,
      `upid = ${upid}`,
    ]);
    return this.createTrack(
      ctx,
      uri,
      sqlSource,
      name,
      `Process ${processName} (${pid}) ${trackName} memory while in the '${
        bucket
      }' OOM bucket (0 otherwise).`,
      true,
    );
  }

  private getSqlSource(window: TimeSpan, whereClauses: string[] = []): string {
    const whereClause =
      whereClauses.length > 0 ? `WHERE ${whereClauses.join(' AND ')}` : '';
    return `
      SELECT iss.ts, SUM(IIF(iss.interval_ends_at_ts = FALSE, m.zygote_adjusted_value, 0)) as value FROM interval_self_intersect!((
        SELECT
          id,
          MAX(ts, ${window.start}) as ts,
          MIN(ts + dur, ${window.end}) - MAX(ts, ${window.start}) as dur
        FROM android_process_memory_intervals_by_oom_bucket
        ${whereClause} AND ts < ${window.end} and ts + dur > ${window.start}
      )) iss JOIN android_process_memory_intervals_by_oom_bucket m USING(id)
      GROUP BY group_id
    `;
  }
}

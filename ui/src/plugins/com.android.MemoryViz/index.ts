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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {uuidv4} from '../../base/uuid';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {TimeSpan} from '../../base/time';

export default class MemoryViz implements PerfettoPlugin {
  static readonly id = 'com.android.MemoryViz';

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE intervals.intersect;
      INCLUDE PERFETTO MODULE android.memory.memory_breakdown;
    `);

    ctx.commands.registerCommand({
      id: `com.android.visualizeMemory`,
      name: 'Memory: Visualize (over selection)',
      callback: async () => {
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

  private async createTrack(
    ctx: Trace,
    uri: string,
    sqlSource: string,
    name: string,
    description: string,
    removable = true,
    sortOrder?: number,
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
      sortOrder,
    });
  }

  private async createRssAnonSwapTrack(
    ctx: Trace,
    window: TimeSpan,
  ): Promise<TrackNode | undefined> {
    const uri = `${MemoryViz.id}.rss_anon_swap.${uuidv4()}`;
    const sqlSource = this.getSqlSource(window, [
      `track_name IN ('mem.rss.anon', 'mem.swap')`,
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
      `track_name = '${trackName}'`,
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
      FROM _memory_breakdown_mem_with_buckets
      WHERE track_name = '${trackName}'
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
      FROM _memory_breakdown_mem_with_buckets
      WHERE
        bucket = '${bucket}' AND
        track_name = '${trackName}' AND
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
      `track_name = '${trackName}'`,
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
      `track_name = '${trackName}'`,
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
        FROM _memory_breakdown_mem_with_buckets
        ${whereClause} AND ts < ${window.end} and ts + dur > ${window.start}
      )) iss JOIN _memory_breakdown_mem_with_buckets m USING(id)
      GROUP BY group_id
    `;
  }
}

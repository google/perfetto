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
import {
  createQueryCounterTrack,
  SqlTableCounterTrack,
} from '../../components/tracks/query_counter_track';
import {uuidv4} from '../../base/uuid';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {time, TimeSpan} from '../../base/time';
import {z} from 'zod';

export default class OomAdjScoreViz implements PerfettoPlugin {
  static readonly id = 'com.android.OomAdjScoreViz';

  private static readonly TBL_INTERVALS = `_${OomAdjScoreViz.id.replace(/\./g, '_')}_oom_intervals`;
  private static readonly TBL_COUNT = `_${OomAdjScoreViz.id.replace(/\./g, '_')}_oom_count`;

  private static readonly TimeSpanSchema = z
    .object({
      startTime: z.string().optional(),
      endTime: z.string().optional(),
    })
    .default({});

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.oom_adjuster;
      INCLUDE PERFETTO MODULE intervals.overlap;

      CREATE OR REPLACE PERFETTO TABLE ${OomAdjScoreViz.TBL_INTERVALS} AS
      SELECT
        row_number() OVER () AS id,
        *
      FROM (
        SELECT
          max(oom.ts, ifnull(p.start_ts, 0)) as ts,
          min(oom.ts + oom.dur, ifnull(p.end_ts, oom.ts + oom.dur)) - max(oom.ts, ifnull(p.start_ts, 0)) AS dur,
          oom.upid,
          oom.score,
          oom.bucket
        FROM android_oom_adj_intervals oom
        JOIN process p ON oom.upid = p.upid
        WHERE oom.dur > 0
      )
      WHERE dur > 0;

      CREATE OR REPLACE PERFETTO TABLE ${OomAdjScoreViz.TBL_COUNT} AS
      SELECT
        ts,
        lead(ts) OVER(PARTITION BY group_name ORDER BY ts) - ts AS dur,
        group_name AS bucket,
        value AS concurrency
      FROM intervals_overlap_count_by_group!(${OomAdjScoreViz.TBL_INTERVALS}, ts, dur, bucket);
    `);

    ctx.commands.registerCommand({
      id: `com.android.visualizeOomAdjScore`,
      name: 'OOM Adjuster Score: Visualize (over selection)',
      callback: async (...args: unknown[]) => {
        const params = OomAdjScoreViz.TimeSpanSchema.parse(args[0]);

        let window: TimeSpan;
        if (params.startTime !== undefined && params.endTime !== undefined) {
          const start = BigInt(params.startTime) as time;
          const end = BigInt(params.endTime) as time;
          window = new TimeSpan(start, end);
        } else {
          window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        }

        const buckets = await ctx.engine.query(
          `SELECT DISTINCT bucket FROM android_oom_adj_intervals WHERE ts < ${window.end} AND ts + dur > ${window.start}`,
        );
        for (const iter = buckets.iter({}); iter.valid(); iter.next()) {
          const bucket = iter.get('bucket') as string;
          const track = await this.createTracksForBucket(ctx, window, bucket);
          ctx.defaultWorkspace.pinnedTracksNode.addChildLast(track);
        }
      },
    });
  }

  private async createTracksForBucket(
    ctx: Trace,
    window: TimeSpan,
    bucket: string,
  ): Promise<TrackNode> {
    const concurrencyUri = `${OomAdjScoreViz.id}.${bucket}.concurrency.${uuidv4()}`;
    ctx.tracks.registerTrack({
      uri: concurrencyUri,
      renderer: new SqlTableCounterTrack(
        ctx,
        concurrencyUri,
        OomAdjScoreViz.getConcurrencyTrackQuery(window, bucket),
      ),
      description: `This track shows the number of processes that are concurrently in the same OOM score bucket '${bucket}' over time.`,
    });

    const concurrencyNode = new TrackNode({
      uri: concurrencyUri,
      name: `${bucket} processes count`,
      removable: true,
    });

    const processes = await ctx.engine.query(
      OomAdjScoreViz.getProcessesQuery(window, bucket),
    );
    for (
      const procIter = processes.iter({});
      procIter.valid();
      procIter.next()
    ) {
      const processName = procIter.get('process_name') as string;
      const upid = procIter.get('upid') as number;
      const pid = procIter.get('pid') as number;
      const processNode = await this.createSingleProcessTrack(
        ctx,
        window,
        upid,
        pid,
        processName,
      );
      concurrencyNode.addChildLast(processNode);
    }
    return concurrencyNode;
  }

  private async createSingleProcessTrack(
    ctx: Trace,
    window: TimeSpan,
    upid: number,
    pid: number,
    processName: string,
  ): Promise<TrackNode> {
    const name = `${processName} ${pid}`;
    const uri = `${OomAdjScoreViz.id}.process.${upid}.${uuidv4()}`;
    const renderer = await createQueryCounterTrack({
      trace: ctx,
      uri,
      materialize: false,
      data: {
        sqlSource: OomAdjScoreViz.getOomScoreTrackQuery(window, upid),
      },
      columns: {
        ts: 'ts',
        value: 'value',
      },
    });
    ctx.tracks.registerTrack({
      uri,
      renderer,
    });
    return new TrackNode({
      name,
      uri,
    });
  }

  private static getConcurrencyTrackQuery(
    window: TimeSpan,
    bucket: string,
  ): string {
    return `
      SELECT
        iif(ts < ${window.start}, ${window.start}, ts) AS ts,
        concurrency AS value
      FROM
        ${this.TBL_COUNT}
      WHERE
        bucket = '${bucket}' AND
        ts < ${window.end} AND
        ts + dur > ${window.start}
      UNION SELECT ${window.end} AS ts, 0 AS value
    `;
  }

  private static getProcessesQuery(window: TimeSpan, bucket: string): string {
    return `
      SELECT
        coalesce(process.name, 'Unknown process') as process_name,
        upid,
        pid
      FROM 
        ${OomAdjScoreViz.TBL_INTERVALS}
      JOIN process USING(upid)
      WHERE
        bucket = '${bucket}' AND
        ts < ${window.end} AND
        ts + dur > ${window.start}
      GROUP BY
        upid, pid, process_name
      ORDER BY
        SUM(MIN(ts + dur, ${window.end}) - MAX(ts, ${window.start})) DESC,
        MAX(score) DESC,
        process_name ASC
    `;
  }

  private static getOomScoreTrackQuery(window: TimeSpan, upid: number): string {
    return `
      SELECT
        iif(ts < ${window.start}, ${window.start}, ts) AS ts,
        score AS value
      FROM
        ${OomAdjScoreViz.TBL_INTERVALS}
      WHERE
        upid = ${upid} AND
        ts < ${window.end} AND
        ts + dur > ${window.start}
      UNION SELECT ${window.end} AS ts, 0 AS value
    `;
  }
}

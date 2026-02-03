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
import {TrackNode} from '../../public/workspace';
import {
  createQueryCounterTrack,
  SqlTableCounterTrack,
} from '../../components/tracks/query_counter_track';
import {TrackRenderer} from '../../public/track';
import {uuidv4} from '../../base/uuid';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {time, TimeSpan} from '../../base/time';

export default class OomAdjScore implements PerfettoPlugin {
  static readonly id = 'com.android.OomAdjScore';

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.oom_adjuster;
      INCLUDE PERFETTO MODULE intervals.overlap;

      CREATE OR REPLACE PERFETTO TABLE _com_android_oom_adj_score_oom_intervals AS
      SELECT row_number() OVER () AS id, * FROM android_oom_adj_intervals
      WHERE dur > 0;

      CREATE OR REPLACE PERFETTO TABLE _com_android_oom_adj_score_oom_count AS
      SELECT
        ts,
        lead(ts) OVER(PARTITION BY group_name ORDER BY ts) - ts AS dur,
        group_name AS bucket,
        value AS concurrency
      FROM intervals_overlap_count_by_group!(_com_android_oom_adj_score_oom_intervals, ts, dur, bucket);
    `);

    ctx.commands.registerCommand({
      id: 'com.android.OomAdjScore.visualize',
      name: 'OOM Adjuster Score: Visualize',
      callback: async (...args: unknown[]) => {
        const params = args[0] as {[key: string]: unknown} | undefined;
        const window = await (async (): Promise<TimeSpan> => {
          if (params?.ts_start !== undefined && params?.ts_end !== undefined) {
            const start = BigInt(params.ts_start as number) as time;
            const end = BigInt(params.ts_end as number) as time;
            return new TimeSpan(start, end);
          }
          return getTimeSpanOfSelectionOrVisibleWindow(ctx);
        })();

        const buckets = await ctx.engine.query(
          `SELECT DISTINCT bucket FROM android_oom_adj_intervals WHERE ts < ${window.end} AND ts + dur > ${window.start}`,
        );
        for (const iter = buckets.iter({}); iter.valid(); iter.next()) {
          const bucket = iter.get('bucket') as string;
          await this.addTracksForBucket(ctx, window, bucket);
        }
      },
    });
  }

  private async addTracksForBucket(
    ctx: Trace,
    window: Awaited<TimeSpan>,
    bucket: string,
  ) {
    const concurrencyUri = `com.android.OomAdjScore.${bucket}.concurrency.${uuidv4()}`;
    ctx.tracks.registerTrack({
      uri: concurrencyUri,
      renderer: new SqlTableCounterTrack(
        ctx,
        concurrencyUri,
        this.getConcurrencyTrackQuery(window, bucket),
      ),
    });

    const concurrencyNode = new TrackNode({
      uri: concurrencyUri,
      name: `OOM Score: ${bucket} concurrency`,
      removable: true,
    });
    ctx.defaultWorkspace.pinnedTracksNode.addChildLast(concurrencyNode);

    const processes = await ctx.engine.query(
      this.getProcessesQuery(window, bucket),
    );
    for (
      const procIter = processes.iter({});
      procIter.valid();
      procIter.next()
    ) {
      const processName = procIter.get('process_name') as string;
      const upid = procIter.get('upid') as number;
      const processNode = await this.createCounterTrackNode(
        ctx,
        `Process: ${processName}`,
        upid,
        window,
      );
      concurrencyNode.addChildLast(processNode);
    }
  }

  private async createCounterTrackNode(
    trace: Trace,
    name: string,
    upid: number,
    window: Awaited<TimeSpan>,
  ): Promise<TrackNode> {
    return await this.createTrackNode(trace, name, (uri: string) => {
      return createQueryCounterTrack({
        trace: trace,
        uri,
        materialize: false,
        data: {
          sqlSource: this.getOomScoreTrackQuery(window, upid),
        },
        columns: {
          ts: 'ts',
          value: 'value',
        },
      });
    });
  }

  private async createTrackNode(
    trace: Trace,
    name: string,
    createTrack: (uri: string, filtersClause: string) => Promise<TrackRenderer>,
  ) {
    const uri = `name_${uuidv4()}`;
    const renderer = await createTrack(uri, '');
    trace.tracks.registerTrack({
      uri,
      renderer,
    });

    return new TrackNode({
      name,
      uri,
    });
  }

  private getConcurrencyTrackQuery(
    window: Awaited<ReturnType<typeof getTimeSpanOfSelectionOrVisibleWindow>>,
    bucket: string,
  ): string {
    return `
      SELECT
        iif(ts < ${window.start}, ${window.start}, ts) AS ts,
        concurrency AS value
      FROM
        _com_android_oom_adj_score_oom_count
      WHERE
        bucket = '${bucket}' AND
        ts < ${window.end} AND
        ts + dur > ${window.start}
      UNION SELECT ${window.end} AS ts, 0 AS value
    `;
  }

  private getProcessesQuery(
    window: Awaited<ReturnType<typeof getTimeSpanOfSelectionOrVisibleWindow>>,
    bucket: string,
  ): string {
    return `
      SELECT
        coalesce(process.name, 'Unknown process') as process_name,
        upid
      FROM _com_android_oom_adj_score_oom_intervals
      JOIN process USING(upid)
      WHERE
        bucket = '${bucket}' AND
        ts < ${window.end} AND
        ts + dur > ${window.start}
      GROUP BY
        upid, process_name
      ORDER BY
        SUM(MIN(ts + dur, ${window.end}) - MAX(ts, ${window.start})) DESC,
        MAX(score) DESC,
        process_name ASC
    `;
  }

  private getOomScoreTrackQuery(
    window: Awaited<ReturnType<typeof getTimeSpanOfSelectionOrVisibleWindow>>,
    upid: number,
  ): string {
    return `
      SELECT
        iif(ts < ${window.start}, ${window.start}, ts) AS ts,
        score AS value
      FROM
        android_oom_adj_intervals
      WHERE
        upid = ${upid} AND
        ts < ${window.end} AND
        ts + dur > ${window.start}
      UNION SELECT ${window.end} AS ts, 0 AS value
    `;
  }
}

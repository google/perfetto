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
  static readonly table_prefix = '_rss_anon_swap_memory_';

  static readonly DENY_THRESHOLD_BYTES = 100 * 1024 * 1024; // 100 MiB

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.oom_adjuster;
      INCLUDE PERFETTO MODULE intervals.intersect;
      INCLUDE PERFETTO MODULE counters.intervals;

      -- Create a table containing intervals of memory counters values, adjusted to process lifetime.
      CREATE OR REPLACE PERFETTO TABLE ${MemoryViz.table_prefix}mem_intervals_raw AS
      WITH
        -- We deny tracks that have large swings in value
        -- This can happen because of rss_stat accounting issue: see b/418231246 for details.
        denied_tracks AS (
          WITH diffs AS (
            SELECT
              track_id,
              value - LAG(value) OVER (PARTITION BY track_id ORDER BY ts) AS d
            FROM counter
          )
          SELECT DISTINCT track_id
          FROM diffs
          WHERE ABS(d) > ${MemoryViz.DENY_THRESHOLD_BYTES}
        ),
        target_counters AS (
          SELECT c.id, c.ts, c.track_id, c.value
          FROM counter c
          JOIN process_counter_track t ON c.track_id = t.id
          WHERE
            t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file')
            AND t.id NOT IN (SELECT track_id FROM denied_tracks)
        ),
        -- Get all memory counter values for all processes, and clip them to process lifetime.
        marked_intervals AS (
          SELECT
            i.ts,
            i.ts + i.dur AS raw_end_ts,
            p.upid,
            p.start_ts,
            p.end_ts,
            t.name AS track_name,
            i.value
          FROM counter_leading_intervals!(target_counters) i
          JOIN process_counter_track t ON i.track_id = t.id
          JOIN process p USING (upid)
        )
      SELECT
        MAX(ts, IFNULL(start_ts, ts)) as ts,
        MIN(raw_end_ts, IFNULL(end_ts, raw_end_ts)) - MAX(ts, IFNULL(start_ts, ts)) as dur,
        upid,
        track_name,
        value
      FROM marked_intervals
      -- Only keep rows where the clipping resulted in a positive duration.
      WHERE (
        MIN(raw_end_ts, IFNULL(end_ts, raw_end_ts)) - MAX(ts, IFNULL(start_ts, ts))) > 0;

      -- Create a table containing intervals of OOM adjustment scores.
      -- This table will be used as the right side of a span join.
      CREATE OR REPLACE PERFETTO TABLE ${MemoryViz.table_prefix}oom_intervals_prepared AS
      SELECT ts, dur, upid, bucket
      FROM android_oom_adj_intervals
      WHERE dur > 0;

      -- Create a virtual table that joins memory counter intervals with OOM
      -- adjustment score intervals.
      DROP TABLE IF EXISTS ${MemoryViz.table_prefix}mem_oom_span_join;
      CREATE VIRTUAL TABLE ${MemoryViz.table_prefix}mem_oom_span_join
      USING SPAN_LEFT_JOIN(
        ${MemoryViz.table_prefix}mem_intervals_raw PARTITIONED upid,
        ${MemoryViz.table_prefix}oom_intervals_prepared PARTITIONED upid
      );

      -- Create a table containing memory counter intervals with OOM buckets.
      CREATE OR REPLACE PERFETTO TABLE ${MemoryViz.table_prefix}mem_with_buckets_indexed AS
      WITH
        -- Get the baseline values for RSS anon and swap from the zygote process.
        zygote_baseline AS (
          SELECT
            MAX(CASE WHEN track_name = 'mem.rss.anon' THEN avg_val END) AS rss_anon_base,
            MAX(CASE WHEN track_name = 'mem.swap' THEN avg_val END) AS swap_base,
            MAX(CASE WHEN track_name = 'mem.rss.file' THEN avg_val END) AS rss_file_base
          FROM (
            SELECT t.name AS track_name, AVG(c.value) AS avg_val
            FROM counter c
            JOIN process_counter_track t ON c.track_id = t.id
            JOIN process p USING (upid)
            WHERE
              -- TODO: improve zygote process detection
              p.name IN ('zygote', 'zygote64', 'webview_zygote') AND
              t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file')
            GROUP BY t.name
          )
        )
      SELECT
        row_number() OVER () AS id,
        ts,
        dur,
        track_name,
        app.name as process_name,
        upid,
        pid,
        IFNULL(bucket, 'unknown') AS bucket,
        CASE
          WHEN app.upid IS NOT NULL AND track_name = 'mem.rss.anon'
            THEN MAX(0, cast_int!(value) - cast_int!(IFNULL((SELECT rss_anon_base FROM zygote_baseline), 0)))
          WHEN app.upid IS NOT NULL AND track_name = 'mem.swap'
            THEN MAX(0, cast_int!(value) - cast_int!(IFNULL((SELECT swap_base FROM zygote_baseline), 0)))
          WHEN app.upid IS NOT NULL AND track_name = 'mem.rss.file'
            THEN MAX(0, cast_int!(value) - cast_int!(IFNULL((SELECT rss_file_base FROM zygote_baseline), 0)))
          ELSE cast_int!(value)
        END AS zygote_adjusted_value
      FROM ${MemoryViz.table_prefix}mem_oom_span_join
      LEFT JOIN process app USING (upid)
      WHERE dur > 0;
`);

    ctx.commands.registerCommand({
      id: `com.android.visualizeMemory`,
      name: 'Memory: Visualize (over selection)',
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        const rssAnonSwapTrack = await this.createRssAnonSwapTrack(ctx, window);
        ctx.defaultWorkspace.pinnedTracksNode.addChildLast(rssAnonSwapTrack);

        const rssFileTrack = await this.createBreakdownTrack(
          ctx,
          window,
          'mem.rss.file',
          'RSS File',
        );
        ctx.defaultWorkspace.pinnedTracksNode.addChildLast(rssFileTrack);
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

  private async createRssAnonSwapTrack(
    ctx: Trace,
    window: TimeSpan,
  ): Promise<TrackNode> {
    const uri = `${MemoryViz.id}.rss_anon_swap.${uuidv4()}`;
    const sqlSource = this.getSqlSource(window, [
      `track_name IN ('mem.rss.anon', 'mem.swap')`,
    ]);
    const rootNode = await this.createTrack(
      ctx,
      uri,
      sqlSource,
      'RSS Anon + Swap',
      'Sum of anonymous RSS and Swap memory across all processes.',
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

    rootNode.addChildLast(rssAnonNode);
    rootNode.addChildLast(swapNode);

    return rootNode;
  }

  private async createBreakdownTrack(
    ctx: Trace,
    window: TimeSpan,
    trackName: string,
    name: string,
  ): Promise<TrackNode> {
    const uri = `${MemoryViz.id}.${trackName}.${uuidv4()}`;
    const sqlSource = this.getSqlSource(window, [
      `track_name = '${trackName}'`,
    ]);
    const breakdownNode = await this.createTrack(
      ctx,
      uri,
      sqlSource,
      name,
      `Total ${name} memory usage across all processes.`,
      true,
    );

    const buckets = await ctx.engine.query(`
      SELECT DISTINCT bucket
      FROM ${MemoryViz.table_prefix}mem_with_buckets_indexed
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
      breakdownNode.addChildLast(bucketNode);
    }

    return breakdownNode;
  }

  private async createOomBucketTrack(
    ctx: Trace,
    window: TimeSpan,
    trackName: string,
    bucket: string,
  ): Promise<TrackNode> {
    const uri = `${MemoryViz.id}.${trackName}.${bucket}.${uuidv4()}`;
    const sqlSource = this.getSqlSource(window, [
      `bucket = '${bucket}'`,
      `track_name = '${trackName}'`,
    ]);
    const bucketNode = await this.createTrack(
      ctx,
      uri,
      sqlSource,
      bucket,
      `Total ${trackName} memory usage across all processes while in the '${
        bucket
      }' OOM bucket.`,
      true,
    );

    const processes = await ctx.engine.query(`
      SELECT upid, pid, process_name, MAX(IIF(iss.interval_ends_at_ts = FALSE, m.zygote_adjusted_value, 0)) as max_value FROM interval_self_intersect!((
        SELECT
          id,
          MAX(ts, ${window.start}) as ts,
          MIN(ts + dur, ${window.end}) - MAX(ts, ${window.start}) as dur
        FROM ${MemoryViz.table_prefix}mem_with_buckets_indexed
        WHERE bucket = '${bucket}' AND track_name = '${trackName}' AND ts < ${
          window.end
        } AND ts + dur > ${window.start}
      )) iss
      JOIN ${MemoryViz.table_prefix}mem_with_buckets_indexed m USING(id)
      GROUP BY upid, pid, process_name
      HAVING max_value > 0
      ORDER BY max_value DESC
      `);
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
      `Process ${processName} (${pid}) ${trackName} memory usage while in the '${
        bucket
      }' OOM bucket`,
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
        FROM ${MemoryViz.table_prefix}mem_with_buckets_indexed
        ${whereClause} AND ts < ${window.end} and ts + dur > ${window.start}
      )) iss JOIN ${MemoryViz.table_prefix}mem_with_buckets_indexed m USING(id)
      GROUP BY group_id
    `;
  }
}

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

export default class RssAnonSwapMemory implements PerfettoPlugin {
  static readonly id = 'com.google.RssAnonSwapMemory';
  static readonly table_prefix = '_rss_anon_swap_memory_';

  // TODO: Make this dynamic
  static readonly RSS_ANON_SWAP_TABLE = '_rss_anon_swap_memory';

  async onTraceLoad(ctx: Trace): Promise<void> {
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.oom_adjuster;
      INCLUDE PERFETTO MODULE intervals.intersect;

      -- ============================================================================
      -- 1. DATA PREPARATION (Blacklist & Baselines)
      -- ============================================================================
      CREATE OR REPLACE PERFETTO TABLE blacklisted_tracks AS
      WITH diffs AS (
      SELECT track_id, value - LAG(value) OVER (PARTITION BY track_id ORDER BY ts) AS d
      FROM counter
      )
      SELECT DISTINCT track_id FROM diffs WHERE ABS(d) > 104857600;

      CREATE OR REPLACE PERFETTO TABLE android_app_processes AS
      SELECT upid FROM process
      WHERE parent_upid IN (
      SELECT upid FROM process
      WHERE name IN ('zygote', 'zygote64', 'webview_zygote')
      );

      CREATE OR REPLACE PERFETTO TABLE zygote_baseline AS
      SELECT
      MAX(CASE WHEN track_name = 'mem.rss.anon' THEN avg_val END) AS rss_anon_base,
      MAX(CASE WHEN track_name = 'mem.swap' THEN avg_val END) AS swap_base
      FROM (
      SELECT t.name AS track_name, AVG(c.value) AS avg_val
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      JOIN process p USING (upid)
      WHERE (p.name = 'zygote' OR p.name = 'zygote64')
        AND (t.name = 'mem.rss.anon' OR t.name = 'mem.swap')
      GROUP BY t.name
      );

      -- ============================================================================
      -- 2. SPAN LEFT JOIN FOR OOM BUCKET ATTRIBUTION
      -- ============================================================================
      -- Prepare Left Side: Memory Intervals (Dropping first AND last samples per track)
      CREATE OR REPLACE PERFETTO TABLE mem_intervals_raw AS
      WITH marked_intervals AS (
      SELECT
        c.ts,
        -- Calculate duration based on the next sample
        IFNULL(LEAD(c.ts) OVER (PARTITION BY track_id ORDER BY c.ts), (SELECT end_ts FROM trace_bounds)) - c.ts AS dur,
        p.upid,
        t.name AS track_name,
        c.value,
        -- Rank from start to find the first
        ROW_NUMBER() OVER (PARTITION BY track_id ORDER BY c.ts ASC) as row_asc,
        -- Rank from end to find the last
        ROW_NUMBER() OVER (PARTITION BY track_id ORDER BY c.ts DESC) as row_desc
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      JOIN process p USING (upid)
      WHERE (t.name = 'mem.rss.anon' OR t.name = 'mem.swap')
        AND t.id NOT IN (SELECT track_id FROM blacklisted_tracks)
      )
      SELECT ts, dur, upid, track_name, value
      FROM marked_intervals
      WHERE row_asc > 1 -- Drops the first sample
      AND row_desc > 1; -- Drops the last sample

      -- Prepare Right Side: OOM Intervals (Must be a table for virtual table usage)
      CREATE OR REPLACE PERFETTO TABLE oom_intervals_prepared AS
      SELECT ts, dur, upid, bucket
      FROM android_oom_adj_intervals
      WHERE dur > 0;

      -- Create the Virtual Table
      DROP TABLE IF EXISTS mem_oom_span_join;
      CREATE VIRTUAL TABLE mem_oom_span_join
      USING SPAN_LEFT_JOIN(
      mem_intervals_raw PARTITIONED upid,
      oom_intervals_prepared PARTITIONED upid
      );

      -- Materialize and clean up IDs for the Macro
      CREATE OR REPLACE PERFETTO TABLE mem_with_buckets_indexed AS
      SELECT
      row_number() OVER () AS id,
      ts,
      dur,
      track_name,
      app.name as process_name,
      upid,
      IFNULL(bucket, 'unknown') AS bucket,
      CASE
        WHEN app.upid IS NOT NULL AND track_name = 'mem.rss.anon'
        THEN MAX(0, CAST(value AS INT) - CAST(IFNULL((SELECT rss_anon_base FROM zygote_baseline), 0) AS INT))
        WHEN app.upid IS NOT NULL AND track_name = 'mem.swap'
        THEN MAX(0, CAST(value AS INT) - CAST(IFNULL((SELECT swap_base FROM zygote_baseline), 0) AS INT))
        ELSE CAST(value AS INT)
      END AS adjusted_value
      FROM mem_oom_span_join
      LEFT JOIN process app USING (upid)
      WHERE dur > 0;
`);

    ctx.commands.registerCommand({
      id: `${RssAnonSwapMemory.id}.visualize`,
      name: 'RSS Anon/Swap: Visualize',
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        const rootTrack = await this.createRootTrack(ctx, window);
        ctx.defaultWorkspace.pinnedTracksNode.addChildLast(rootTrack);
      },
    });
  }

  private async createRootTrack(
    ctx: Trace,
    window: TimeSpan,
  ): Promise<TrackNode> {
    const uri = `${RssAnonSwapMemory.id}.rss_anon_swap.${uuidv4()}`;
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      materialize: false,
      data: {
        sqlSource: `
          SELECT ts, dur, SUM(v) as value FROM (
            SELECT iss.ts, iss.dur, upid, m.adjusted_value as v, iss.group_id FROM interval_self_intersect!((SELECT id, ts, dur FROM mem_with_buckets_indexed)) iss JOIN mem_with_buckets_indexed m USING(id) where iss.interval_ends_at_ts = FALSE
          ) GROUP BY group_id
        `,
      },
      columns: {
        ts: 'ts',
        value: 'value',
      },
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });

    const rootNode = new TrackNode({
      uri,
      name: 'RSS Anon + Swap',
      removable: true,
    });

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
    const uri = `${RssAnonSwapMemory.id}.${trackName}.${uuidv4()}`;
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      materialize: false,
      data: {
        sqlSource: `
          SELECT ts, dur, SUM(v) as value FROM (
            SELECT iss.ts, iss.dur, upid, m.adjusted_value as v, iss.group_id FROM interval_self_intersect!((SELECT id, ts, dur FROM mem_with_buckets_indexed WHERE track_name = '${trackName}')) iss JOIN mem_with_buckets_indexed m USING(id) where iss.interval_ends_at_ts = FALSE
          ) GROUP BY group_id
        `,
      },
      columns: {
        ts: 'ts',
        value: 'value',
      },
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });

    const breakdownNode = new TrackNode({
      uri,
      name,
      removable: true,
    });

    const buckets = await ctx.engine.query(`
      SELECT DISTINCT bucket
      FROM mem_with_buckets_indexed
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
    const uri = `${RssAnonSwapMemory.id}.${trackName}.${bucket}.${uuidv4()}`;
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      materialize: false,
      data: {
        sqlSource: `
          SELECT ts, dur, '${bucket}' as bucket,SUM(v) as value FROM (
            SELECT iss.ts, iss.dur, IIF(iss.interval_ends_at_ts = FALSE, m.adjusted_value, 0) as v, iss.group_id FROM interval_self_intersect!((SELECT id, ts, dur FROM mem_with_buckets_indexed WHERE bucket = '${bucket}' AND track_name = '${trackName}')) iss JOIN mem_with_buckets_indexed m USING(id) 
          ) GROUP BY group_id
        `,
      },
      columns: {
        ts: 'ts',
        value: 'value',
      },
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });

    const bucketNode = new TrackNode({
      uri,
      name: bucket,
      removable: true,
    });

    const processes = await ctx.engine.query(`
      SELECT upid, process_name, MAX(IIF(iss.interval_ends_at_ts = FALSE, m.adjusted_value, 0)) as max_value FROM interval_self_intersect!((SELECT id, ts, dur FROM mem_with_buckets_indexed WHERE bucket = '${bucket}' AND track_name = '${trackName}')) iss 
      JOIN mem_with_buckets_indexed m USING(id) 
      GROUP BY upid, process_name
      ORDER BY max_value DESC
      `);
    for (
      const procIter = processes.iter({});
      procIter.valid();
      procIter.next()
    ) {
      const upid = procIter.get('upid') as number;
      const processName = procIter.get('process_name') as string;
      const processNode = await this.createSingleProcessTrack(
        ctx,
        window,
        upid,
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
    _window: TimeSpan,
    upid: number,
    processName: string,
    trackName: string,
    bucket: string,
  ): Promise<TrackNode> {
    const name = `${processName} : ${upid}`;
    const uri = `${RssAnonSwapMemory.id}.process.${upid}.${trackName}.${uuidv4()}`;
    const renderer = await createQueryCounterTrack({
      trace: ctx,
      uri,
      materialize: false,
      data: {
        sqlSource: `
          SELECT iss.ts as ts, iss.dur, IIF(iss.interval_ends_at_ts = FALSE, m.adjusted_value, 0) as value FROM interval_self_intersect!((SELECT id, ts, dur FROM mem_with_buckets_indexed WHERE bucket = '${bucket}' AND track_name = '${trackName}' AND upid = ${upid} AND process_name = '${processName}')) iss JOIN mem_with_buckets_indexed m USING(id)
        `,
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
}

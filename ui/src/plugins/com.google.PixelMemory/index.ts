// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {addDebugCounterTrack} from '../../components/tracks/debug_tracks';
import {Time} from '../../base/time';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {NUM} from '../../trace_processor/query_result';
import {randomColor} from '../../components/colorizer';

const CREATE_BREAKDOWN_TABLE_SQL = `
  DROP TABLE IF EXISTS process_memory_breakdown;
  CREATE VIRTUAL TABLE process_memory_breakdown
  USING
    SPAN_OUTER_JOIN(
      android_gpu_memory_per_process PARTITIONED upid,
      memory_rss_and_swap_per_process PARTITIONED upid
    );
`;

export default class implements PerfettoPlugin {
  static readonly id = 'com.google.PixelMemory';
  private tablesInitialized = false;

  // Helper to set up the breakdown tables idempotently.
  private async setupTables(ctx: Trace) {
    if (this.tablesInitialized) {
      return;
    }
    await ctx.engine.query('INCLUDE PERFETTO MODULE android.gpu.memory;');
    await ctx.engine.query('INCLUDE PERFETTO MODULE linux.memory.process;');
    await ctx.engine.query(CREATE_BREAKDOWN_TABLE_SQL);
    this.tablesInitialized = true;
  }

  // Helper to find the max value from a SQL query and add a note to the
  // timeline.
  private async addMaxMemoryAnnotation(
    ctx: Trace,
    findMaxSql: string,
    noteTarget: string,
  ) {
    try {
      const maxResult = await ctx.engine.query(findMaxSql);
      // Using .firstRow() as LIMIT 1 is in the SQL
      if (maxResult.numRows() > 0) {
        const maxRow = maxResult.firstRow({ts: NUM, value: NUM});
        const maxTs = BigInt(maxRow.ts);
        const maxValueInBytes = maxRow.value;
        const maxValueInKib = (maxValueInBytes / 1024.0).toFixed(2);
        const noteText = `Max (${noteTarget}): ${maxValueInKib} KiB`;
        const color = randomColor();

        ctx.notes.addNote({
          timestamp: Time.fromRaw(maxTs),
          text: noteText,
          color,
        });
      }
    } catch (e) {
      console.error('Failed to add max memory annotation:', e);
    }
  }

  // Helper function to handle the aggregation logic for multiple PIDs.
  private async createAggregatedTrackAndGetTableName(
    ctx: Trace,
    id: string,
    pidList: string[],
    sqlValueExpr: string,
    titleSuffix: string,
    pidsIdentifier: string,
  ): Promise<string> {
    const runId = id.replace(/[#\.]/g, '_');
    const viewNames = pidList.map((pid) => `__view_${runId}_${pid}`);
    const valueColNames = pidList.map((pid) => `value_${pid}`);
    const aggTableName = `__agg_${runId}`;

    // 1. Create a separate VIEW for each PID's memory data.
    for (let i = 0; i < pidList.length; i++) {
      const createViewSql = `
        DROP VIEW IF EXISTS ${viewNames[i]};
        CREATE VIEW ${viewNames[i]} AS
        SELECT
            ts,
            dur,
            (${sqlValueExpr}) AS ${valueColNames[i]}
        FROM process_memory_breakdown
        WHERE pid = ${pidList[i]};
      `;
      await ctx.engine.query(createViewSql);
    }

    // 2. Iteratively SPAN_OUTER_JOIN the views together.
    let previousTableName = viewNames[0];
    for (let i = 1; i < pidList.length; i++) {
      const newJoinedTableName = `__joined_${runId}_${i}`;
      await ctx.engine.query(`DROP TABLE IF EXISTS ${newJoinedTableName};`);
      const joinSql = `
        CREATE VIRTUAL TABLE ${newJoinedTableName}
        USING SPAN_OUTER_JOIN(${previousTableName}, ${viewNames[i]});
      `;
      await ctx.engine.query(joinSql);
      previousTableName = newJoinedTableName;
    }

    const finalSelectTable = previousTableName;
    const sumOfValues = valueColNames
      .map((col) => `IFNULL(${col}, 0)`)
      .join(' + ');

    // 3. Materialize the aggregated sum into a standard table
    await ctx.engine.query(`DROP TABLE IF EXISTS ${aggTableName}`);
    const createAggTableSql = `
      CREATE TABLE ${aggTableName} AS
      SELECT
        CAST(ts AS BIGINT) AS ts,
        (${sumOfValues}) AS value
      FROM ${finalSelectTable}
      WHERE ts IS NOT NULL;
    `;
    await ctx.engine.query(createAggTableSql);

    // 4. Add the debug track using the materialized aggregate table
    await addDebugCounterTrack({
      trace: ctx,
      data: {
        sqlSource: `SELECT ts, value FROM ${aggTableName} ORDER BY ts`,
        columns: ['ts', 'value'],
      },
      title: `${pidsIdentifier}${titleSuffix}`,
    });

    // 5. Return the aggregate table name
    return aggTableName;
  }

  // Prepares the SQL and target name for the max memory annotation.
  private async prepareAnnotationData(
    ctx: Trace,
    id: string,
    pidList: string[],
    sqlValueExpr: string,
    titleSuffix: string,
  ): Promise<{findMaxSql: string; noteTarget: string}> {
    if (pidList.length > 1) {
      const pidsIdentifierForTracks = pidList.join('_');
      const noteTarget = pidList.join('+');
      const aggTableName = await this.createAggregatedTrackAndGetTableName(
        ctx,
        id,
        pidList,
        sqlValueExpr,
        titleSuffix,
        pidsIdentifierForTracks,
      );

      const findMaxSql = `
        SELECT ts, value
        FROM ${aggTableName}
        WHERE value IS NOT NULL
        ORDER BY value DESC, ts ASC
        LIMIT 1
      `;
      return {findMaxSql, noteTarget};
    } else {
      const noteTarget = pidList[0];
      const findMaxSql = `
        SELECT
          ts,
          (${sqlValueExpr}) AS value
        FROM process_memory_breakdown
        WHERE pid = ${pidList[0]} AND value IS NOT NULL
        ORDER BY value DESC, ts ASC
        LIMIT 1
      `;
      return {findMaxSql, noteTarget};
    }
  }

  // Helper to register a command that adds a memory counter track.
  private registerMemoryCommand(
    ctx: Trace,
    id: string,
    name: string,
    sqlValueExpr: string,
    titleSuffix: string,
  ) {
    ctx.commands.registerCommand({
      id,
      name,
      callback: async (pids?: string) => {
        if (pids === undefined) {
          pids =
            prompt(
              'Enter one or more process pids, separated by commas',
              'e.g. 1234, 5678',
            ) || '';
          if (!pids) return;
        }

        await this.setupTables(ctx);

        const pidList = pids
          .split(',')
          .map((pid) => pid.trim())
          .filter((pid) => pid);

        if (pidList.length === 0) {
          return;
        }

        // Add individual tracks for each PID.
        for (const pid of pidList) {
          await addDebugCounterTrack({
            trace: ctx,
            data: {
              sqlSource: `
                SELECT
                  ts,
                  (${sqlValueExpr}) AS value
                FROM process_memory_breakdown
                WHERE pid = ${pid}
              `,
              columns: ['ts', 'value'],
            },
            title: `${pid}${titleSuffix}`,
          });
        }

        const {findMaxSql, noteTarget} = await this.prepareAnnotationData(
          ctx,
          id,
          pidList,
          sqlValueExpr,
          titleSuffix,
        );

        await this.addMaxMemoryAnnotation(ctx, findMaxSql, noteTarget);
      },
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.registerMemoryCommand(
      ctx,
      'dev.perfetto.PixelMemory#ShowTotalMemory',
      'Add tracks: show process total memory',
      'COALESCE(rss_and_swap, 0) + COALESCE(gpu_memory, 0)',
      '_rss_anon_file_swap_shmem_gpu',
    );

    this.registerMemoryCommand(
      ctx,
      'dev.perfetto.PixelMemory#ShowRssAnonShmemSwapGpuMemory',
      'Add tracks: show process total memory (excluding file RSS)',
      'COALESCE(anon_rss_and_swap, 0) + COALESCE(shmem_rss, 0) + ' +
        'COALESCE(gpu_memory, 0)',
      '_rss_anon_shmem_swap_gpu',
    );
  }
}

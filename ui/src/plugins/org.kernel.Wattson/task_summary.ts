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

import {exists} from '../../base/utils';
import {type AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import type {Engine} from '../../trace_processor/engine';
import {WATTSON_THREAD_TRACK_KIND} from './track_kinds';

// The tracks in an area selection that Wattson can attribute CPU power to.
export interface WattsonTrackSelection {
  readonly cpus: ReadonlyArray<number>;
  readonly utids: ReadonlyArray<number>;
}

export function getWattsonTrackSelection(
  area: AreaSelection,
): WattsonTrackSelection {
  const cpus: number[] = [];
  const utids: number[] = [];
  for (const trackInfo of area.tracks) {
    if (trackInfo?.tags?.kinds?.includes(CPU_SLICE_TRACK_KIND)) {
      exists(trackInfo.tags.cpu) && cpus.push(trackInfo.tags.cpu);
    }
    if (trackInfo?.tags?.kinds?.includes(WATTSON_THREAD_TRACK_KIND)) {
      exists(trackInfo.tags.utid) && utids.push(trackInfo.tags.utid);
    }
  }
  return {cpus, utids};
}

/**
 * Builds `wattson_plugin_thread_summary`, shared by the thread, process and
 * package tabs. All applicable tabs are prepared on each selection change, so
 * the build is memoised on the area selection and concurrent callers share
 * the same promise.
 */
export class WattsonTaskSummary {
  private cached?: {
    readonly area: AreaSelection;
    readonly done: Promise<void>;
  };

  constructor(private readonly engine: Engine) {}

  build(
    area: AreaSelection,
    {cpus, utids}: WattsonTrackSelection,
  ): Promise<void> {
    if (!this.cached || !areaSelectionsEqual(this.cached.area, area)) {
      this.cached = {area, done: this.createTaskSummary(area, cpus, utids)};
    }
    return this.cached.done;
  }

  private async createTaskSummary(
    area: AreaSelection,
    cpus: ReadonlyArray<number>,
    utids: ReadonlyArray<number>,
  ): Promise<void> {
    const duration = area.end - area.start;
    const filters = [];
    if (cpus.length > 0) {
      filters.push(`cpu IN (${cpus.join()})`);
    }
    if (utids.length > 0) {
      filters.push(`utid IN (${utids.join()})`);
    }
    const whereClause = `WHERE ${filters.join(' OR ')}`;

    await this.engine.query(`
      INCLUDE PERFETTO MODULE wattson.aggregation;
      CREATE OR REPLACE PERFETTO TABLE wattson_plugin_ui_selection_window AS
      SELECT
        ${area.start} as ts,
        ${duration} as dur,
        0 as period_id;

      -- Prefilter tasks table to be small
      CREATE OR REPLACE PERFETTO VIEW _wattson_ui_selected_tasks AS
      SELECT *
      FROM _estimates_w_tasks_attribution
      ${whereClause};

      -- Use a dedicated CPUs table to avoid incorrectly filtering idle costs
      CREATE OR REPLACE PERFETTO TABLE _wattson_ui_selected_cpus AS
      SELECT cpu FROM _wattson_cpus
      ${cpus.length > 0 ? `WHERE cpu IN (${cpus.join()})` : ''};

      -- Use SPAN_JOIN to clip tasks to the window
      DROP TABLE IF EXISTS _wattson_ui_windowed_tasks;
      CREATE VIRTUAL TABLE _wattson_ui_windowed_tasks
      USING SPAN_JOIN(
        wattson_plugin_ui_selection_window,
        _wattson_ui_selected_tasks PARTITIONED cpu
      );

      -- Materialize the thread-level summary once.
      CREATE OR REPLACE PERFETTO TABLE wattson_plugin_thread_summary AS
      SELECT *
      FROM _wattson_threads_aggregation!(
        _wattson_ui_windowed_tasks,
        wattson_plugin_ui_selection_window,
        _wattson_ui_selected_cpus
      );
    `);
  }
}

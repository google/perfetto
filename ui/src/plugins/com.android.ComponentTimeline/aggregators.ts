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

import m from 'mithril';
import {
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
} from '../../components/aggregation_adapter';
import {formatPercentValue} from '../../components/aggregation_panel';
import {titleWithHelp} from '../../components/distribution_panel';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {CellRenderer} from '../../components/widgets/datagrid/datagrid_schema';
import {Icons} from '../../base/semantic_icons';
import {Duration, Time} from '../../base/time';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {sqliteString} from '../../base/string_utils';
import {Anchor} from '../../widgets/anchor';

export const PLUGIN_ID = 'com.android.ComponentTimeline';

/** Track tag `type` for total intersecting overlap counter track. */
export const TAG_TOTAL_OVERLAP_TRACK = 'android_component_total_overlap';

/** Track tag `type` for self-intersecting overlap slice track. */
export const TAG_SELF_INTERSECT_TRACK = 'android_component_self_intersect';

/** Track tag `type` for per-category concurrency counter tracks. */
export const TAG_CONCURRENCY_TRACK = 'android_component_concurrency';

/** Track tag `type` for per-target concurrency counter tracks. */
export const TAG_TARGET_CONCURRENCY_TRACK =
  'android_component_target_concurrency';

/** Track tag `type` for per-category summary tracks. */
export const TAG_CATEGORY_TRACK = 'android_component_category';

/** Track tag `type` for per-component target sub-tracks. */
export const TAG_TARGET_TRACK = 'android_component_target';

/** Track tag `type` for per-process summary tracks. */
export const TAG_PROCESS_TRACK = 'android_component_process';

/** Track tag `type` for per-process intersecting overlap counter tracks. */
export const TAG_PROCESS_OVERLAP_TRACK = 'android_component_process_overlap';

/** Track tag `type` for per-process per-category sub-tracks. */
export const TAG_PROCESS_CATEGORY_TRACK = 'android_component_process_category';

/** Track tag `type` for per-process per-target sub-tracks. */
export const TAG_PROCESS_TARGET_TRACK = 'android_component_process_target';

export function totalOverlapTrackUri(): string {
  return `${PLUGIN_ID}#count.total_overlap`;
}

export function totalOverlapChildTrackUri(): string {
  return `${PLUGIN_ID}#count.total_overlap.child`;
}

export function selfIntersectTrackUri(): string {
  return `${PLUGIN_ID}#self_intersect`;
}

export function concurrencyTrackUri(category: string): string {
  return `${PLUGIN_ID}#count.${category}`;
}

export function targetConcurrencyTrackUri(
  category: string,
  target: string,
): string {
  return `${PLUGIN_ID}#count.${category}.${target}`;
}

export function categoryTrackUri(category: string): string {
  return `${PLUGIN_ID}#category.${category}`;
}

export function targetTrackUri(category: string, target: string): string {
  return `${PLUGIN_ID}#target.${category}.${target}`;
}

export function processTrackUri(upid: number): string {
  return `${PLUGIN_ID}#proc.${upid}`;
}

export function processOverlapTrackUri(upid: number): string {
  return `${PLUGIN_ID}#proc.${upid}.overlap`;
}

export function processCategoryTrackUri(
  upid: number,
  category: string,
): string {
  return `${PLUGIN_ID}#proc.${upid}.${category}`;
}

export function processTargetTrackUri(
  upid: number,
  category: string,
  target: string,
): string {
  return `${PLUGIN_ID}#proc.${upid}.${category}.${target}`;
}

interface Scope {
  readonly allActive: boolean;
  readonly upids: ReadonlyArray<number>;
  readonly categories: ReadonlyArray<string>;
  readonly catTargets: ReadonlyArray<string>;
  readonly procCats: ReadonlyArray<string>;
  readonly procCatTargets: ReadonlyArray<string>;
}

function probeScope(area: AreaSelection): Scope | undefined {
  let allActive = false;
  const upids = new Set<number>();
  const categories = new Set<string>();
  const catTargets = new Set<string>();
  const procCats = new Set<string>();
  const procCatTargets = new Set<string>();

  for (const track of area.tracks) {
    const tags = track.tags;
    if (!tags) continue;
    const type = tags.type;
    if (type === TAG_TOTAL_OVERLAP_TRACK || type === TAG_SELF_INTERSECT_TRACK) {
      allActive = true;
    } else if (
      (type === TAG_PROCESS_TRACK || type === TAG_PROCESS_OVERLAP_TRACK) &&
      typeof tags.upid === 'number'
    ) {
      upids.add(tags.upid);
    } else if (
      type === TAG_PROCESS_CATEGORY_TRACK &&
      typeof tags.upid === 'number' &&
      typeof tags.category === 'string'
    ) {
      procCats.add(`${tags.upid}:${tags.category}`);
    } else if (
      type === TAG_PROCESS_TARGET_TRACK &&
      typeof tags.upid === 'number' &&
      typeof tags.category === 'string' &&
      typeof tags.target === 'string'
    ) {
      procCatTargets.add(`${tags.upid}:${tags.category}:${tags.target}`);
    } else if (
      (type === TAG_CATEGORY_TRACK || type === TAG_CONCURRENCY_TRACK) &&
      typeof tags.category === 'string'
    ) {
      categories.add(tags.category);
    } else if (
      (type === TAG_TARGET_TRACK || type === TAG_TARGET_CONCURRENCY_TRACK) &&
      typeof tags.category === 'string' &&
      typeof tags.target === 'string'
    ) {
      catTargets.add(`${tags.category}:${tags.target}`);
    }
  }

  if (
    !allActive &&
    upids.size === 0 &&
    categories.size === 0 &&
    catTargets.size === 0 &&
    procCats.size === 0 &&
    procCatTargets.size === 0
  ) {
    return undefined;
  }

  return {
    allActive,
    upids: [...upids],
    categories: [...categories],
    catTargets: [...catTargets],
    procCats: [...procCats],
    procCatTargets: [...procCatTargets],
  };
}

function buildScopeWhereClause(scope: Scope): string {
  if (scope.allActive) {
    return `category != 'proc_state'`;
  }
  const terms: string[] = [];
  if (scope.upids.length > 0) {
    terms.push(
      `(upid IN (${scope.upids.join(',')}) AND category != 'proc_state')`,
    );
  }
  if (scope.categories.length > 0) {
    terms.push(`category IN (${scope.categories.map(sqliteString).join(',')})`);
  }
  if (scope.catTargets.length > 0) {
    terms.push(
      `cat_target_key IN (${scope.catTargets.map(sqliteString).join(',')})`,
    );
  }
  if (scope.procCats.length > 0) {
    terms.push(
      `proc_cat_key IN (${scope.procCats.map(sqliteString).join(',')})`,
    );
  }
  if (scope.procCatTargets.length > 0) {
    terms.push(
      `proc_cat_target_key IN (${scope.procCatTargets.map(sqliteString).join(',')})`,
    );
  }
  return terms.join(' OR ');
}

function durationCellRenderer(trace: Trace): CellRenderer {
  return (value) => {
    const dur = typeof value === 'number' ? BigInt(Math.round(value)) : value;
    if (typeof dur !== 'bigint') {
      return 'N/A';
    }
    return m(DurationWidget, {trace, dur: Duration.fromRaw(dur)});
  };
}

/**
 * Aggregates component execution residency over the selected tracks and time
 * range using Perfetto's C++ interval tree (`_interval_intersect_single!`),
 * `interval_merge_overlapping_partitioned!`, and `intervals_overlap_count_by_group!`.
 */
export class ComponentResidencyAggregator implements Aggregator {
  readonly id = 'android_component_residency';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area);
    if (scope === undefined) {
      return undefined;
    }

    const whereClause = buildScopeWhereClause(scope);

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const selectionDur = area.end > area.start ? area.end - area.start : 1n;
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH scoped_raw AS (
              SELECT
                id,
                ts,
                dur,
                upid,
                pid,
                process_name,
                category,
                target,
                proc_cat_target_key
              FROM _android_component_timeline_events
              WHERE (${whereClause})
                AND dur > 0
            ),
            ii_clipped AS (
              SELECT
                ii.id,
                ii.ts,
                ii.dur,
                s.upid,
                s.pid,
                s.process_name,
                s.category,
                s.target,
                s.proc_cat_target_key
              FROM _interval_intersect_single!(
                ${area.start},
                ${selectionDur},
                scoped_raw
              ) ii
              JOIN scoped_raw s USING (id)
              WHERE ii.dur > 0
            ),
            merged_wall AS (
              SELECT
                upid,
                category,
                target,
                sum(dur) AS wall_dur
              FROM interval_merge_overlapping_partitioned!(
                ii_clipped,
                (upid, category, target)
              )
              GROUP BY upid, category, target
            ),
            peak_ov AS (
              SELECT
                group_name AS proc_cat_target_key,
                max(value) AS max_overlap
              FROM intervals_overlap_count_by_group!(
                ii_clipped,
                ts,
                dur,
                proc_cat_target_key
              )
              GROUP BY group_name
            )
            SELECT
              c.process_name,
              c.pid,
              c.category,
              c.target,
              coalesce(mw.wall_dur, sum(c.dur)) AS total_dur,
              sum(c.dur) AS cumulative_dur,
              coalesce(po.max_overlap, 1) AS max_overlap,
              coalesce(mw.wall_dur, sum(c.dur)) / ${selectionDur}.0 AS occupancy,
              count() AS occurrences
            FROM ii_clipped c
            LEFT JOIN merged_wall mw USING (upid, category, target)
            LEFT JOIN peak_ov po USING (proc_cat_target_key)
            GROUP BY c.upid, c.category, c.target
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        category: {title: 'Category', columnType: 'text'},
        target: {title: 'Component / Target', columnType: 'text'},
        total_dur: {
          title: titleWithHelp(
            'Wall duration (merged)',
            'Non-overlapping wall-clock time where at least one instance of this component was active (via interval_merge_overlapping_partitioned!).',
          ),
          titleString: 'Wall duration (merged)',
          columnType: 'quantitative',
          cellRenderer: durationCellRenderer(this.trace),
        },
        cumulative_dur: {
          title: titleWithHelp(
            'Cumulative duration',
            'Sum of all clipped component slice durations in the selection (via _interval_intersect_single!).',
          ),
          titleString: 'Cumulative duration',
          columnType: 'quantitative',
          cellRenderer: durationCellRenderer(this.trace),
        },
        max_overlap: {
          title: titleWithHelp(
            'Peak overlap',
            'Maximum number of concurrently overlapping instances of this component in the selection.',
          ),
          titleString: 'Peak overlap',
          columnType: 'quantitative',
        },
        occupancy: {
          title: '% of selection',
          columnType: 'quantitative',
          cellRenderer: formatPercentValue,
        },
        occurrences: {title: 'Occurrences', columnType: 'quantitative'},
      },
      initialColumns: [
        {id: 'process_name', field: 'process_name'},
        {id: 'pid', field: 'pid'},
        {id: 'category', field: 'category'},
        {id: 'target', field: 'target'},
        {id: 'total_dur', field: 'total_dur', aggregate: 'SUM', sort: 'DESC'},
        {id: 'cumulative_dur', field: 'cumulative_dur', aggregate: 'SUM'},
        {id: 'max_overlap', field: 'max_overlap', aggregate: 'MAX'},
        {id: 'occupancy', field: 'occupancy'},
        {id: 'occurrences', field: 'occurrences', aggregate: 'SUM'},
      ],
    };
  }

  getTabName() {
    return 'Component Execution Summary';
  }
}

/**
 * Lists atomic time segments in the selection where >= 2 components intersect
 * simultaneously, powered by `_interval_intersect_single!` and `interval_self_intersect!`.
 */
export class ComponentOverlapAggregator implements Aggregator {
  readonly id = 'android_component_self_intersect_overlap';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area);
    if (scope === undefined) {
      return undefined;
    }

    const whereClause = buildScopeWhereClause(scope);

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const selectionDur = area.end > area.start ? area.end - area.start : 1n;
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH scoped_raw AS (
              SELECT
                id,
                ts,
                dur,
                upid,
                process_name,
                category,
                target
              FROM _android_component_timeline_events
              WHERE (${whereClause})
                AND category != 'proc_state'
                AND dur > 0
            ),
            ii_clipped AS (
              SELECT
                ii.id,
                ii.ts,
                ii.dur,
                s.upid,
                s.process_name,
                s.category,
                s.target
              FROM _interval_intersect_single!(
                ${area.start},
                ${selectionDur},
                scoped_raw
              ) ii
              JOIN scoped_raw s USING (id)
              WHERE ii.dur > 0
              ORDER BY ii.dur DESC, ii.ts ASC
              LIMIT 10000
            )
            SELECT
              si.ts,
              si.dur,
              count() AS overlap_count,
              count(DISTINCT c.upid) AS process_count,
              GROUP_CONCAT(DISTINCT c.category) AS categories,
              GROUP_CONCAT(c.process_name || ': ' || c.target, ' | ') AS overlapping_components
            FROM interval_self_intersect!(ii_clipped) si
            JOIN ii_clipped c ON c.id = si.id
            WHERE si.interval_ends_at_ts = FALSE
              AND si.dur > 0
            GROUP BY si.group_id, si.ts, si.dur
            HAVING overlap_count >= 2
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        ts: {
          title: 'Intersection start',
          columnType: 'quantitative',
          cellRenderer: (value: unknown) => {
            if (typeof value === 'bigint') {
              return m(Timestamp, {trace: this.trace, ts: Time.fromRaw(value)});
            }
            return String(value ?? '');
          },
        },
        dur: {
          title: 'Intersection duration',
          columnType: 'quantitative',
          cellRenderer: durationCellRenderer(this.trace),
        },
        overlap_count: {
          title: 'Concurrent components',
          columnType: 'quantitative',
        },
        process_count: {
          title: 'Distinct processes',
          columnType: 'quantitative',
        },
        categories: {
          title: 'Categories',
          columnType: 'text',
        },
        overlapping_components: {
          title: 'Intersecting components (process: target)',
          columnType: 'text',
        },
      },
      initialColumns: [
        {
          id: 'overlap_count',
          field: 'overlap_count',
          aggregate: 'MAX',
          sort: 'DESC',
        },
        {id: 'dur', field: 'dur', aggregate: 'SUM'},
        {id: 'ts', field: 'ts'},
        {id: 'process_count', field: 'process_count', aggregate: 'MAX'},
        {id: 'categories', field: 'categories'},
        {id: 'overlapping_components', field: 'overlapping_components'},
      ],
    };
  }

  getTabName() {
    return 'Intersecting Component Overlaps';
  }
}

/**
 * Lists every individual component execution interval in the selection, clipped
 * via Perfetto's C++ interval tree (`_interval_intersect_single!`).
 */
export class ComponentEventsAggregator implements Aggregator {
  readonly id = 'android_component_events';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area);
    if (scope === undefined) {
      return undefined;
    }

    const whereClause = buildScopeWhereClause(scope);

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const selectionDur = area.end > area.start ? area.end - area.start : 1n;
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH scoped_raw AS (
              SELECT
                id,
                ts,
                dur,
                upid,
                pid,
                process_name,
                category,
                target,
                raw_name,
                hosting_type
              FROM _android_component_timeline_events
              WHERE (${whereClause})
                AND dur > 0
            )
            SELECT
              s.ts,
              json_object('id', s.id, 'upid', s.upid, 'category', s.category) AS slice_ref,
              s.process_name,
              s.pid,
              s.category,
              s.target,
              s.raw_name,
              ii.dur AS clipped_dur,
              s.dur,
              coalesce(s.hosting_type, 'N/A') AS hosting_type
            FROM _interval_intersect_single!(
              ${area.start},
              ${selectionDur},
              scoped_raw
            ) ii
            JOIN scoped_raw s USING (id)
            WHERE ii.dur > 0
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        ts: {
          title: 'Start ts',
          columnType: 'quantitative',
          cellRenderer: (value: unknown) => {
            if (typeof value === 'bigint') {
              return m(Timestamp, {trace: this.trace, ts: Time.fromRaw(value)});
            }
            return String(value ?? '');
          },
        },
        slice_ref: {
          title: 'Slice ID',
          columnType: 'identifier',
          cellRenderer: (value: unknown) => {
            if (typeof value !== 'string') {
              return String(value ?? '');
            }
            const {id, upid, category} = JSON.parse(value) as {
              id: number;
              upid: number;
              category: string;
            };
            return m(
              Anchor,
              {
                title: 'Go to component slice on process sub-track',
                icon: Icons.UpdateSelection,
                onclick: () => {
                  this.trace.selection.selectTrackEvent(
                    processCategoryTrackUri(upid, category),
                    id,
                    {
                      scrollToSelection: true,
                      switchToCurrentSelectionTab: false,
                    },
                  );
                },
              },
              String(id),
            );
          },
          cellFormatter: (value: unknown) => {
            if (typeof value === 'string') {
              return String((JSON.parse(value) as {id: number}).id);
            }
            return String(value ?? '');
          },
        },
        process_name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        category: {title: 'Category', columnType: 'text'},
        target: {title: 'Component / Target', columnType: 'text'},
        raw_name: {title: 'Event / Trigger', columnType: 'text'},
        clipped_dur: {
          title: titleWithHelp(
            'Clipped duration',
            'Duration of this component interval within the selected time window (via _interval_intersect_single!).',
          ),
          titleString: 'Clipped duration',
          columnType: 'quantitative',
          cellRenderer: durationCellRenderer(this.trace),
        },
        dur: {
          title: titleWithHelp(
            'Total duration',
            'Total elapsed duration of the component interval, unclipped by the selection window.',
          ),
          titleString: 'Total duration',
          columnType: 'quantitative',
          cellRenderer: durationCellRenderer(this.trace),
        },
        hosting_type: {title: 'Cold-start hosting', columnType: 'text'},
      },
      initialColumns: [
        {id: 'ts', field: 'ts', sort: 'ASC'},
        {id: 'slice_ref', field: 'slice_ref'},
        {id: 'process_name', field: 'process_name'},
        {id: 'pid', field: 'pid'},
        {id: 'category', field: 'category'},
        {id: 'target', field: 'target'},
        {id: 'raw_name', field: 'raw_name'},
        {id: 'clipped_dur', field: 'clipped_dur', aggregate: 'SUM'},
        {id: 'dur', field: 'dur'},
        {id: 'hosting_type', field: 'hosting_type'},
      ],
    };
  }

  getTabName() {
    return 'Component Events';
  }
}

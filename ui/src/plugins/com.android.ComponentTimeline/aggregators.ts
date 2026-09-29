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

/** Track tag `type` for per-category concurrency counter tracks. */
export const TAG_CONCURRENCY_TRACK = 'android_component_concurrency';

/** Track tag `type` for per-category summary tracks. */
export const TAG_CATEGORY_TRACK = 'android_component_category';

/** Track tag `type` for per-component target sub-tracks. */
export const TAG_TARGET_TRACK = 'android_component_target';

/** Track tag `type` for per-process summary tracks. */
export const TAG_PROCESS_TRACK = 'android_component_process';

/** Track tag `type` for per-process per-category sub-tracks. */
export const TAG_PROCESS_CATEGORY_TRACK = 'android_component_process_category';

/** Track tag `type` for per-process per-target sub-tracks. */
export const TAG_PROCESS_TARGET_TRACK = 'android_component_process_target';

export function concurrencyTrackUri(category: string): string {
  return `${PLUGIN_ID}#count.${category}`;
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
  readonly upids: ReadonlyArray<number>;
  readonly categories: ReadonlyArray<string>;
  readonly catTargets: ReadonlyArray<string>;
  readonly procCats: ReadonlyArray<string>;
  readonly procCatTargets: ReadonlyArray<string>;
}

function probeScope(area: AreaSelection): Scope | undefined {
  const upids = new Set<number>();
  const categories = new Set<string>();
  const catTargets = new Set<string>();
  const procCats = new Set<string>();
  const procCatTargets = new Set<string>();

  for (const track of area.tracks) {
    const tags = track.tags;
    if (!tags) continue;
    const type = tags.type;
    if (type === TAG_PROCESS_TRACK && typeof tags.upid === 'number') {
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
      type === TAG_TARGET_TRACK &&
      typeof tags.category === 'string' &&
      typeof tags.target === 'string'
    ) {
      catTargets.add(`${tags.category}:${tags.target}`);
    }
  }

  if (
    upids.size === 0 &&
    categories.size === 0 &&
    catTargets.size === 0 &&
    procCats.size === 0 &&
    procCatTargets.size === 0
  ) {
    return undefined;
  }

  return {
    upids: [...upids],
    categories: [...categories],
    catTargets: [...catTargets],
    procCats: [...procCats],
    procCatTargets: [...procCatTargets],
  };
}

function buildScopeWhereClause(scope: Scope): string {
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

function scopedIntervals(scope: Scope, area: AreaSelection): string {
  const whereClause = buildScopeWhereClause(scope);
  return `
    SELECT
      *,
      min(ts + dur, ${area.end}) - max(ts, ${area.start}) AS clipped_dur
    FROM _android_component_timeline_events
    WHERE (${whereClause})
      AND ts < ${area.end}
      AND ts + dur > ${area.start}
  `;
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

/** Aggregates component execution residency over the selected tracks and time range. */
export class ComponentResidencyAggregator implements Aggregator {
  readonly id = 'android_component_residency';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection) {
    const scope = probeScope(area);
    if (scope === undefined) {
      return undefined;
    }

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const selectionDur = area.end - area.start;
        const table = await createPerfettoTable({
          engine,
          as: `
            WITH scoped AS (${scopedIntervals(scope, area)})
            SELECT
              process_name,
              pid,
              category,
              target,
              sum(clipped_dur) AS total_dur,
              sum(clipped_dur) / ${selectionDur}.0 AS occupancy,
              count() AS occurrences
            FROM scoped
            GROUP BY upid, category, target
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
          title: 'Total duration',
          columnType: 'quantitative',
          cellRenderer: durationCellRenderer(this.trace),
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
        {id: 'occupancy', field: 'occupancy'},
        {id: 'occurrences', field: 'occurrences', aggregate: 'SUM'},
      ],
    };
  }

  getTabName() {
    return 'Component Execution Summary';
  }
}

/** Lists every individual component execution interval in the selection. */
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
        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              ts,
              json_object('id', id, 'upid', upid, 'category', category) AS slice_ref,
              process_name,
              pid,
              category,
              target,
              raw_name,
              dur,
              coalesce(hosting_type, 'N/A') AS hosting_type
            FROM _android_component_timeline_events
            WHERE (${whereClause})
              AND ts < ${area.end}
              AND ts + dur > ${area.start}
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
        dur: {
          title: titleWithHelp(
            'Duration',
            'Total elapsed duration of the component interval, unclipped by the selection window.',
          ),
          titleString: 'Duration',
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
        {id: 'dur', field: 'dur'},
        {id: 'hosting_type', field: 'hosting_type'},
      ],
    };
  }

  getTabName() {
    return 'Component Events';
  }
}

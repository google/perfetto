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

import m from 'mithril';
import {Engine} from '../../../trace_processor/engine';
import {LONG_NULL, NUM, STR} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {Select} from '../../../widgets/select';
import {GridLayout} from '../../../widgets/grid_layout';
import {
  HeatmapChart,
  HeatmapData,
} from '../../../components/widgets/charts/heatmap';
import {
  StatsSectionRow,
  loadStatsWithFilter,
  groupByCategory,
  renderErrorCategoryCard,
  renderCategorySection,
} from '../utils';

const NUM_BUCKETS = 100;

interface TraceFileHeatmap {
  readonly traceId: number;
  readonly traceName: string;
  readonly data: HeatmapData;
}

export interface DataLossesData {
  losses: StatsSectionRow[];
  importLogStatNames: string[];
}

export async function loadDataLossesData(
  engine: Engine,
): Promise<DataLossesData> {
  const losses = await loadStatsWithFilter(
    engine,
    "severity = 'data_loss' AND value > 0",
  );

  // Get distinct stat names that have import log entries with byte offsets
  // and a packet_sequence_id arg.
  const namesResult = await engine.query(`
    SELECT DISTINCT l.name
    FROM _trace_import_logs l
    JOIN args a ON l.arg_set_id = a.arg_set_id
    WHERE l.byte_offset IS NOT NULL
      AND a.key = 'packet_sequence_id'
    ORDER BY l.name
  `);
  const importLogStatNames: string[] = [];
  for (const it = namesResult.iter({name: STR}); it.valid(); it.next()) {
    importLogStatNames.push(it.name);
  }

  return {losses, importLogStatNames};
}

async function loadHeatmapData(
  engine: Engine,
  statName: string,
): Promise<TraceFileHeatmap[]> {
  const result = await engine.query(`
    SELECT
      l.trace_id as trace_id,
      IFNULL(f.name, 'trace ' || l.trace_id) as trace_name,
      f.size as trace_size,
      l.byte_offset as byte_offset,
      CAST(extract_arg(l.arg_set_id, 'packet_sequence_id') AS INT) as seq_id
    FROM _trace_import_logs l
    JOIN __intrinsic_trace_file f ON l.trace_id = f.id
    WHERE l.name = '${statName}'
      AND l.byte_offset IS NOT NULL
    ORDER BY l.trace_id, seq_id, l.byte_offset
  `);

  // Group by trace_id
  const byTrace = new Map<
    number,
    {
      traceName: string;
      traceSize: number;
      events: Array<{byteOffset: number; seqId: number}>;
    }
  >();

  for (
    const it = result.iter({
      trace_id: NUM,
      trace_name: STR,
      trace_size: LONG_NULL,
      byte_offset: LONG_NULL,
      seq_id: LONG_NULL,
    });
    it.valid();
    it.next()
  ) {
    if (
      it.byte_offset === null ||
      it.seq_id === null ||
      it.trace_size === null
    ) {
      continue;
    }
    const traceId = it.trace_id;
    if (!byTrace.has(traceId)) {
      byTrace.set(traceId, {
        traceName: it.trace_name,
        traceSize: Number(it.trace_size),
        events: [],
      });
    }
    byTrace.get(traceId)!.events.push({
      byteOffset: Number(it.byte_offset),
      seqId: Number(it.seq_id),
    });
  }

  // Build heatmap data per trace file
  const heatmaps: TraceFileHeatmap[] = [];
  for (const [traceId, info] of byTrace) {
    // X-axis: 100 buckets (0% to 99%)
    const xLabels = Array.from({length: NUM_BUCKETS}, (_, i) => `${i}%`);

    // Y-axis: unique sequence IDs
    const seqIds = [...new Set(info.events.map((e) => e.seqId))].sort(
      (a, b) => a - b,
    );
    const yLabels = seqIds.map((id) => `seq ${id}`);
    const seqIdToIndex = new Map(seqIds.map((id, i) => [id, i]));

    // Bucketize: count events per (bucket, seqId)
    const counts = new Map<string, number>();
    let maxCount = 0;
    for (const event of info.events) {
      const bucket = Math.min(
        Math.floor((event.byteOffset / info.traceSize) * NUM_BUCKETS),
        NUM_BUCKETS - 1,
      );
      const seqIdx = seqIdToIndex.get(event.seqId)!;
      const key = `${bucket},${seqIdx}`;
      const count = (counts.get(key) ?? 0) + 1;
      counts.set(key, count);
      if (count > maxCount) maxCount = count;
    }

    // Build values array
    const values: Array<[number, number, number]> = [];
    for (const [key, count] of counts) {
      const [bucket, seqIdx] = key.split(',').map(Number);
      values.push([bucket, seqIdx, count]);
    }

    heatmaps.push({
      traceId,
      traceName: info.traceName,
      data: {
        xLabels,
        yLabels,
        values,
        min: 0,
        max: maxCount,
      },
    });
  }

  return heatmaps;
}

export interface DataLossesTabAttrs {
  data: DataLossesData;
  engine: Engine;
}

export class DataLossesTab implements m.ClassComponent<DataLossesTabAttrs> {
  private selectedStat: string | undefined;
  private heatmaps: TraceFileHeatmap[] | undefined;
  private loading = false;

  view({attrs}: m.CVnode<DataLossesTabAttrs>) {
    const categories = groupByCategory(attrs.data.losses);
    const statNames = attrs.data.importLogStatNames;

    // Auto-select first stat if none selected
    if (this.selectedStat === undefined && statNames.length > 0) {
      this.selectedStat = statNames[0];
      this.loadHeatmap(attrs.engine, this.selectedStat);
    }

    return m(
      '.pf-trace-info-page__tab-content',
      // Data loss heatmap section
      statNames.length > 0 &&
        m(
          Section,
          {
            title: 'Data Loss Timeline',
            subtitle:
              'Heatmap of dropped packets by byte position in the trace file. ' +
              'X-axis shows position (% through file), Y-axis shows packet sequence ID',
          },
          this.renderStatSelector(attrs.engine, statNames),
          this.renderHeatmaps(),
        ),
      // Category cards
      m(
        Section,
        {
          title: 'Data Loss Categories',
          subtitle:
            'Summary of data loss events grouped by category. These counters are collected at trace recording time',
        },
        categories.length === 0
          ? m('')
          : m(
              GridLayout,
              {},
              categories.map((cat) =>
                renderErrorCategoryCard(cat, 'warning', 'warning'),
              ),
            ),
      ),
      // Detailed breakdown by category
      categories.length > 0 &&
        m(
          Section,
          {
            title: 'Detailed Breakdown',
            subtitle: 'Individual data loss entries grouped by category',
          },
          categories.map((cat) =>
            renderCategorySection(cat, {
              className: 'pf-trace-info-page__logs-grid',
            }),
          ),
        ),
    );
  }

  private renderStatSelector(engine: Engine, statNames: string[]): m.Children {
    return m(
      '.pf-trace-info-page__heatmap-controls',
      {style: {marginBottom: '16px'}},
      m('label', {style: {marginRight: '8px', fontWeight: '500'}}, 'Stat: '),
      m(
        Select,
        {
          value: this.selectedStat,
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            this.selectedStat = target.value;
            this.loadHeatmap(engine, this.selectedStat);
          },
        },
        statNames.map((name) => m('option', {value: name}, name)),
      ),
    );
  }

  private renderHeatmaps(): m.Children {
    if (this.loading) {
      return m('', 'Loading heatmap data...');
    }
    if (this.heatmaps === undefined || this.heatmaps.length === 0) {
      return m(
        '',
        'No data loss events with byte offsets found for this stat.',
      );
    }
    return this.heatmaps.map((hm) =>
      m(
        '',
        {style: {marginBottom: '24px'}},
        this.heatmaps!.length > 1 &&
          m('h4', {style: {marginBottom: '8px'}}, hm.traceName),
        m(HeatmapChart, {
          data: hm.data,
          height: Math.max(150, hm.data.yLabels.length * 30 + 80),
          xAxisLabel: 'Position in trace file (%)',
          yAxisLabel: '',
          formatValue: (v: number) => `${v} dropped`,
        }),
      ),
    );
  }

  private loadHeatmap(engine: Engine, statName: string): void {
    this.loading = true;
    this.heatmaps = undefined;
    loadHeatmapData(engine, statName).then((heatmaps) => {
      this.heatmaps = heatmaps;
      this.loading = false;
      m.redraw();
    });
  }
}

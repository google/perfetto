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
import {Icons} from '../../base/semantic_icons';
import {duration} from '../../base/time';
import {formatDuration} from '../../components/time_utils';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {
  CellRenderResult,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {SQLSchemaRegistry} from '../../components/widgets/datagrid/sql_schema';
import {
  AreaSelection,
  areaSelectionsEqual,
  Selection,
  TrackEventSelection,
  TrackSelection,
} from '../../public/selection';
import {Tab} from '../../public/tab';
import {Trace} from '../../public/trace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {
  NUM,
  Row,
  SqlValue,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {DownloadToFileButton} from '../../widgets/download_to_file_button';
import {MultiSelectDiff, PopupMultiSelect} from '../../widgets/multiselect';
import {PopupPosition} from '../../widgets/popup';
import {Spinner} from '../../widgets/spinner';

const V8_RCS_SQL_SCHEMA: SQLSchemaRegistry = {
  v8_rcs: {
    table: 'v8_rcs_view',
    columns: {
      v8_rcs_group: {},
      v8_rcs_name: {},
      v8_rcs_count: {},
      v8_rcs_dur: {},
      v8_rcs_count_percent: {},
      v8_rcs_dur_percent: {},
    },
  },
};

const GROUP_COLORS: {[key: string]: string} = {
  api: '#990099',
  blink: '#006600',
  callback: '#109618',
  compile_bg: '#b08000',
  compile: '#FFAA00',
  gc_bg: '#00597c',
  gc_custom: '#0099C6',
  gc: '#00799c',
  ic: '#3366CC',
  javascript: '#DD4477',
  network_data: '#103366',
  optimize_bg: '#702000',
  optimize_maglev_bg: '#962c02',
  optimize_maglev: '#fc4f26',
  optimize: '#DC3912',
  parse_bg: '#c05000',
  parse: '#FF6600',
  runtime: '#88BB00',
  total: '#BBB',
  unclassified: '#000',
};
const GROUP_COLORS_LENGTH = Object.keys(GROUP_COLORS).length;

export class V8RuntimeCallStatsTab implements Tab {
  private previousSelection?: Selection;
  private loading = false;
  private dataSource?: SQLDataSource;
  private selectedGroups = new Set<string>(Object.keys(GROUP_COLORS));

  constructor(private readonly trace: Trace) {}

  getTitle(): string {
    return 'V8 Runtime Call Stats';
  }

  render(): m.Children {
    const selection = this.trace.selection.selection;
    if (
      selection.kind !== 'area' &&
      selection.kind !== 'track_event' &&
      selection.kind !== 'track' &&
      selection.kind !== 'empty'
    ) {
      return this.renderEmptyState();
    }

    const selectionChanged = this.hasSelectionChanged(selection);

    if (selectionChanged) {
      this.previousSelection = selection;
      this.dataSource = undefined;
      this.loading = true;
      this.loadData(selection);
    }

    if (this.loading) {
      return m('div.pf-v8-loading-container', m(Spinner));
    }

    if (!this.dataSource) {
      return this.renderEmptyState();
    }

    return m(DataGrid, {
      schema: this.getUiSchema(),
      rootSchema: 'v8_rcs',
      toolbarItemsLeft: this.renderGroupFilter(),
      toolbarItemsRight: this.renderExportButton(),
      data: this.dataSource,
      fillHeight: true,
      initialPivot: {
        groupBy: [{field: 'v8_rcs_group', id: 'v8_rcs_group'}],
        aggregates: [
          {
            function: 'SUM',
            field: 'v8_rcs_dur',
            sort: 'DESC',
            id: 'v8_rcs_dur',
          },
          {
            function: 'SUM',
            field: 'v8_rcs_dur_percent',
            id: 'v8_rcs_dur_percent',
          },
          {
            function: 'SUM',
            field: 'v8_rcs_count',
            id: 'v8_rcs_count',
          },
          {
            function: 'SUM',
            field: 'v8_rcs_count_percent',
            id: 'v8_rcs_count_percent',
          },
        ],
      },
    });
  }

  private getUiSchema(): SchemaRegistry {
    return {
      v8_rcs: {
        v8_rcs_group: {
          title: 'Group',
          columnType: 'text',
        },
        v8_rcs_name: {
          title: 'Name',
          columnType: 'text',
        },
        v8_rcs_dur: {
          title: 'RCS Duration',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            return formatDuration(this.trace, value as duration);
          },
        },
        v8_rcs_dur_percent: {
          title: 'Duration %',
          columnType: 'quantitative',
          cellRenderer: (value, row) => this.renderPercentCell(value, row),
        },
        v8_rcs_count: {
          title: 'RCS Count',
          columnType: 'quantitative',
        },
        v8_rcs_count_percent: {
          title: 'Count %',
          columnType: 'quantitative',
          cellRenderer: (value, row) => this.renderPercentCell(value, row),
        },
      },
    };
  }

  private renderPercentCell(
    value: SqlValue,
    row: Row,
  ): CellRenderResult | string {
    const val = (value as number) ?? 0;
    const group = row['v8_rcs_group'] as string;
    const isHeaderRow = !group;
    if (isHeaderRow) {
      return `${val.toFixed(2)}%`;
    }
    const color = GROUP_COLORS[group] ?? GROUP_COLORS['unclassified'];
    return {
      content: m(
        'div.pf-v8-percent-cell',
        m('div.bar', {
          style: {
            width: `${val}%`,
            backgroundColor: color,
          },
        }),
        m('div.value', `${val.toFixed(2)}%`),
      ),
    };
  }

  private renderEmptyState(): m.Children {
    return m(
      'div',
      {style: {padding: '10px'}},
      'Select an area, a slice, or a track to view specific V8 Runtime Call Stats, or clear selection to view all.',
    );
  }

  private renderGroupFilter() {
    const groups = Object.keys(GROUP_COLORS);
    return m(PopupMultiSelect, {
      icon: Icons.Filter,
      label: 'Filter Groups',
      position: PopupPosition.Top,
      showNumSelected: true,
      options: groups.map((group) => ({
        id: group,
        name: group,
        checked: this.selectedGroups.has(group),
      })),
      onChange: (diffs: MultiSelectDiff[]) => {
        for (const {id, checked} of diffs) {
          if (checked) {
            this.selectedGroups.add(id);
          } else {
            this.selectedGroups.delete(id);
          }
        }
        if (diffs.length && this.previousSelection) {
          this.loadData(this.previousSelection);
        }
      },
    });
  }

  private renderExportButton() {
    let selection = 'All';
    switch (this.previousSelection?.kind) {
      case 'area':
        selection = 'Area';
        break;
      case 'track_event':
        selection = 'Slice';
        break;
      case 'track':
        selection = 'Trace';
        break;
    }

    return m(DownloadToFileButton, {
      fileName: 'rcs.json',
      label: `Export ${selection} RCS`,
      content: () => new RcsJsonExporter().export(this.trace),
    });
  }

  private hasSelectionChanged(selection: Selection): boolean {
    if (this.previousSelection === undefined) return true;
    if (this.previousSelection.kind !== selection.kind) return true;

    if (selection.kind === 'area') {
      return !areaSelectionsEqual(
        this.previousSelection as AreaSelection,
        selection,
      );
    }

    if (selection.kind === 'track_event') {
      const prev = this.previousSelection as TrackEventSelection;
      return (
        prev.eventId !== selection.eventId ||
        prev.trackUri !== selection.trackUri
      );
    }

    if (selection.kind === 'track') {
      const prev = this.previousSelection as TrackSelection;
      return prev.trackUri !== selection.trackUri;
    }

    return false;
  }

  private async loadData(selection: Selection) {
    let shouldLoad = false;
    let trackIds: number[] = [];

    if (selection.kind === 'area') {
      trackIds = selection.tracks
        .filter((track) => track.tags?.kinds?.includes(SLICE_TRACK_KIND))
        .flatMap((track) => track.tags?.trackIds ?? []);
      shouldLoad = trackIds.length > 0;
    } else if (selection.kind === 'track') {
      const track = this.trace.tracks.getTrack(selection.trackUri);
      trackIds = (track?.tags?.trackIds ?? []) as number[];
      shouldLoad = trackIds.length > 0;
    } else if (selection.kind === 'track_event') {
      const result = await this.trace.engine.query(`
          SELECT 1 FROM args
          JOIN slice ON slice.arg_set_id = args.arg_set_id
          WHERE slice.id = ${selection.eventId}
          AND args.key GLOB 'debug.runtime-call-stats.*'
          LIMIT 1
        `);
      shouldLoad = result.numRows() > 0;
    } else if (selection.kind === 'empty') {
      shouldLoad = true;
    }

    if (shouldLoad && this.previousSelection === selection) {
      await this.updateSqlView(selection, trackIds);
      if (this.previousSelection === selection) {
        this.dataSource = new SQLDataSource({
          engine: this.trace.engine,
          sqlSchema: V8_RCS_SQL_SCHEMA,
          rootSchemaName: 'v8_rcs',
        });
      }
    }

    if (this.previousSelection === selection) {
      this.loading = false;
    }
  }

  private async updateSqlView(selection: Selection, trackIds: number[]) {
    let whereClause: string;
    let ratioExpression: string = '1.0';

    if (selection.kind === 'area') {
      const start = selection.start;
      const end = selection.end;
      whereClause = `
          s.track_id IN (${trackIds.join(',')}) AND
          s.ts < ${end} AND s.ts + s.dur > ${start}
      `;
      ratioExpression = `
          CASE
            WHEN s.dur = 0 THEN 1.0
            ELSE
              MAX(0.0, (
                  MIN(s.ts + s.dur, ${end}) -
                  MAX(s.ts, ${start}))
              ) / CAST(s.dur AS DOUBLE)
          END
      `;
    } else if (selection.kind === 'track') {
      whereClause = `s.track_id IN (${trackIds.join(',')})`;
    } else if (selection.kind === 'track_event') {
      const prev = selection as TrackEventSelection;
      whereClause = `s.id = ${prev.eventId}`;
    } else if (selection.kind === 'empty') {
      // Select all.
      whereClause = '1 = 1';
    } else {
      throw new Error(`Unknown selection kind: ${selection.kind}`);
    }

    let groupWhereClause = '1 = 1';
    if (this.selectedGroups.size !== GROUP_COLORS_LENGTH) {
      const selectedGroups = Array.from(this.selectedGroups)
        .map((name) => `'${name}'`)
        .join(',');
      groupWhereClause = `v8_rcs_group IN (${selectedGroups})`;
    }

    await this.trace.engine.query(`
      CREATE OR REPLACE PERFETTO VIEW v8_rcs_view AS
      WITH rcs_entries AS (
        SELECT
          s.ts,
          s.dur,
          s.track_id,
          SUBSTR(a.key, 26, LENGTH(a.key) - 28) AS name,
          SUBSTR(a.key, -3) AS suffix,
          a.int_value,
          ${ratioExpression} AS ratio
        FROM slice s
        JOIN args a ON s.arg_set_id = a.arg_set_id
        WHERE
          a.key GLOB 'debug.runtime-call-stats.*' AND
          ${whereClause}
      ),
      rcs_aggregated AS (
        SELECT
          CASE
            WHEN name LIKE '%Total%' THEN 'total'
            WHEN name LIKE '%RegExp%' THEN 'regexp'
            WHEN name LIKE '%IC^_%' ESCAPE '^' THEN 'ic'
            WHEN name LIKE '%IC%Miss' THEN 'ic'
            WHEN name LIKE 'IC' THEN 'ic'
            WHEN name LIKE 'Json%' THEN 'json'
            WHEN name LIKE '%Optimize%Background%' THEN 'optimize_bg'
            WHEN name LIKE '%Optimize%Concurrent%' THEN 'optimize_bg'
            WHEN name LIKE 'StackGuard%' THEN 'optimize'
            WHEN name LIKE 'Optimize%' THEN 'optimize'
            WHEN name LIKE 'Deoptimize%' THEN 'optimize'
            WHEN name LIKE 'Recompile%' THEN 'optimize'
            WHEN name LIKE '%TierUp%' THEN 'optimize'
            WHEN name LIKE '%BudgetInterrupt%' THEN 'optimize'
            WHEN name LIKE 'Compile%Optimized%' THEN 'optimize'
            WHEN name LIKE '%Compile%Background%' THEN 'compile_bg'
            WHEN name LIKE 'Compile%' THEN 'compile'
            WHEN name LIKE '%^_Compile%' ESCAPE '^' THEN 'compile'
            WHEN name LIKE '%CompileLazy%' THEN 'compile'
            WHEN name LIKE '%Parse%Background%' THEN 'parse_bg'
            WHEN name LIKE 'Parse%' THEN 'parse'
            WHEN name LIKE 'PreParse%' THEN 'parse'
            WHEN name LIKE '%GetMoreDataCallback%' THEN 'network_data'
            WHEN name LIKE '%Callback%' THEN 'callback'
            WHEN name LIKE '%Blink C\+\+%' THEN 'callback'
            WHEN name LIKE '%API%' THEN 'api'
            WHEN name LIKE 'GC^_Custom^_%'  ESCAPE '^' THEN 'gc_custom'
            WHEN name LIKE 'GC^_%BACKGROUND%' ESCAPE '^' THEN 'gc_bg'
            WHEN name LIKE 'GC^_%Background%' ESCAPE '^' THEN 'gc_bg'
            WHEN name LIKE 'GC^_%AllocateInTargetSpace' ESCAPE '^' THEN 'gc'
            WHEN name LIKE 'GC_%' ESCAPE '^' THEN 'gc'
            WHEN name LIKE 'JS^_Execution' ESCAPE '^' THEN 'javascript'
            WHEN name LIKE 'JavaScript' THEN 'javascript'
            WHEN name LIKE '%Blink^_%' ESCAPE '^' THEN 'blink'
            ELSE 'runtime'
          END AS v8_rcs_group,
          name AS v8_rcs_name,
          track_id,
          SUM(CASE WHEN suffix = '[1]'
            THEN CAST(int_value * 1000 * ratio AS INT)
            ELSE 0
            END) AS v8_rcs_dur,
          SUM(CASE WHEN suffix = '[0]'
            THEN CAST(int_value * ratio AS INT)
            ELSE 0
            END) AS v8_rcs_count
        FROM rcs_entries
        GROUP BY name, track_id
      )
      SELECT
        *,
        v8_rcs_dur * 100.0 / SUM(v8_rcs_dur) OVER () AS v8_rcs_dur_percent,
        v8_rcs_count * 100.0 / SUM(v8_rcs_count) OVER () AS v8_rcs_count_percent
      FROM rcs_aggregated
      WHERE ${groupWhereClause}
    `);
  }
}

const RCS_PROCESS_URL_RE = /https?:\/\/[^\/\s]+/;

class RcsJsonExporter {
  private pageNames = new Map<number, string>();
  private tldCounts = new Map<string, number>();
  private pageStats: {
    [pageName: string]: {
      [key: string]: {
        count: {
          average: number;
          stddev: number;
        };
        duration: {
          average: number;
          stddev: number;
        };
      };
    };
  } = Object.create(null);

  public async export(trace: Trace): Promise<string> {
    const result = await trace.engine.query(`
      SELECT
        p.upid,
        args.string_value AS process_label,
        v.v8_rcs_name,
        SUM(v.v8_rcs_dur) AS v8_rcs_dur,
        SUM(v.v8_rcs_count) AS v8_rcs_count
      FROM v8_rcs_view v
      JOIN thread_track tt ON v.track_id = tt.id
      JOIN thread t ON tt.utid = t.utid
      JOIN process p ON t.upid = p.upid
      LEFT JOIN args ON p.arg_set_id = args.arg_set_id AND args.key = 'chrome.process_label[0]'
      GROUP BY p.upid, v.v8_rcs_name
    `);

    const it = result.iter({
      upid: NUM,
      process_label: STR_NULL,
      v8_rcs_name: STR,
      v8_rcs_count: NUM,
      v8_rcs_dur: NUM,
    });

    for (; it.valid(); it.next()) {
      const pageName = this.getPageName(it.upid, it.process_label ?? '');
      const pageStats = this.getPageStats(pageName);
      pageStats[it.v8_rcs_name] = this.newEntry(
        it.v8_rcs_count,
        it.v8_rcs_dur / 1_000_000,
      );
    }

    for (const pageStats of Object.values(this.pageStats)) {
      let totalCount = 0;
      let totalDurationMs = 0;
      for (const rcsEntry of Object.values(pageStats)) {
        totalCount += rcsEntry.count.average;
        totalDurationMs += rcsEntry.duration.average;
      }
      pageStats['Total'] = this.newEntry(totalCount, totalDurationMs);
    }

    return JSON.stringify(
      {
        'default version': this.pageStats,
      },
      null,
      2,
    );
  }

  getPageName(upid: number, processLabel: string): string {
    const cachedPageName = this.pageNames.get(upid);
    if (cachedPageName) return cachedPageName;

    const match = processLabel.match(RCS_PROCESS_URL_RE);
    let tld;
    if (match) {
      const rawURL = match[0];
      tld = new URL(rawURL).hostname;
    } else {
      tld = `PID=${upid}`;
    }
    const tldCount = (this.tldCounts.get(tld) ?? 0) + 1;
    this.tldCounts.set(tld, tldCount);
    const pageName = tldCount == 1 ? tld : `${tld}-${tldCount}`;
    this.pageNames.set(upid, pageName);
    return pageName;
  }

  getPageStats(pageName: string) {
    if (pageName in this.pageStats) return this.pageStats[pageName];
    const newStats = Object.create(null);
    this.pageStats[pageName] = newStats;
    return newStats;
  }

  newEntry(count: number, duration: number) {
    return {
      count: {
        average: count,
        stddev: 0,
      },
      duration: {
        average: duration,
        stddev: 0,
      },
    };
  }
}

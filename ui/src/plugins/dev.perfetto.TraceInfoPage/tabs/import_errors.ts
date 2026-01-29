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
import {
  LONG_NULL,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {Grid, GridCell, GridHeaderCell} from '../../../widgets/grid';
import {GridLayout} from '../../../widgets/grid_layout';
import {time, Time} from '../../../base/time';
import {renderTimecode} from '../../../components/time_utils';
import {
  StatsSectionRow,
  loadStatsWithFilter,
  groupByCategory,
  renderErrorCategoryCard,
  getTraceInfos,
} from '../utils';
import {parseJsonWithBigints} from '../../../base/json_utils';

// Import log row spec
const importLogSpec = {
  ts: LONG_NULL,
  severity: STR,
  name: STR,
  byte_offset: LONG_NULL,
  args: STR_NULL,
  trace_id: NUM_NULL,
};

interface ImportLogRow {
  ts: time | null;
  severity: string;
  name: string;
  byte_offset: bigint | null;
  args: string | null;
  traceId: number | null;
  traceIndex: number | null;
}

interface ErrorCategory {
  name: string;
  description: string;
  totalCount: number;
  entries: StatsSectionRow[];
  logs: ImportLogRow[];
}

export interface ImportErrorsData {
  errors: StatsSectionRow[];
  categories: ErrorCategory[];
  isMultiTrace: boolean;
}

export async function loadImportErrorsData(
  engine: Engine,
): Promise<ImportErrorsData> {
  const errors = await loadStatsWithFilter(
    engine,
    "severity = 'error' AND source = 'analysis' AND value > 0",
  );

  const traceInfos = await getTraceInfos(engine);
  const isMultiTrace = traceInfos.size > 1;

  // Group errors by category and add logs array
  const categories = groupByCategory(errors).map((cat) => ({
    ...cat,
    logs: [] as ImportLogRow[],
  }));

  const categoryMap = new Map<string, ErrorCategory>();
  for (const category of categories) {
    categoryMap.set(category.name, category);
  }

  // Load import logs for each category
  const logsResult = await engine.query(`
    select
      ts,
      severity,
      name,
      byte_offset,
      __intrinsic_arg_set_to_json(arg_set_id) as args,
      trace_id
    from _trace_import_logs
    where severity = 'error'
    order by ts
  `);

  for (const iter = logsResult.iter(importLogSpec); iter.valid(); iter.next()) {
    const category = categoryMap.get(iter.name);
    if (!category) continue;

    const traceId = iter.trace_id;
    const info = traceId !== null ? traceInfos.get(traceId) : undefined;
    category.logs.push({
      ts: iter.ts !== null ? Time.fromRaw(iter.ts) : null,
      severity: iter.severity,
      name: iter.name,
      byte_offset: iter.byte_offset,
      args: iter.args,
      traceId: iter.trace_id ?? null,
      traceIndex: info?.traceIndex ?? null,
    });
  }

  return {errors, categories, isMultiTrace};
}

export interface ImportErrorsTabAttrs {
  data: ImportErrorsData;
}

export class ImportErrorsTab implements m.ClassComponent<ImportErrorsTabAttrs> {
  // Track current pagination window per category
  private logsWindows = new Map<
    string,
    {offset: number; limit: number; rowHeightPx: number}
  >();

  view({attrs}: m.CVnode<ImportErrorsTabAttrs>) {
    const categories = attrs.data.categories;

    return m(
      '.pf-trace-info-page__tab-content',
      // Category cards at the top
      m(
        Section,
        {
          title: 'Import Error Categories',
          subtitle:
            'Summary of import errors grouped by category. These errors occurred during trace processing',
        },
        categories.length === 0
          ? m('')
          : m(
              GridLayout,
              {},
              categories.map((cat) =>
                renderErrorCategoryCard(cat, 'danger', 'error'),
              ),
            ),
      ),
      // Detailed breakdown by category
      categories.length > 0 &&
        m(
          Section,
          {
            title: 'Detailed Breakdown',
            subtitle: 'Individual import error entries grouped by category',
          },
          categories.map((cat) =>
            this.renderCategorySection(cat, attrs.data.isMultiTrace),
          ),
        ),
    );
  }

  private renderCategorySection(
    category: ErrorCategory,
    isMultiTrace: boolean,
  ): m.Children {
    // Check if we have logs and if the count matches
    const hasMatchingLogs = category.logs.length === category.totalCount;
    // Generate ID for the category section
    const categoryId = `category-${category.name.replace(/[^a-zA-Z0-9_-]/g, '_')}`;
    return m(
      '',
      m('h3', {id: categoryId}, category.name),
      category.description && m('p', category.description),
      hasMatchingLogs
        ? this.renderLogsGrid(category, isMultiTrace)
        : this.renderStatsGrid(category),
    );
  }

  private renderLogsGrid(
    category: ErrorCategory,
    isMultiTrace: boolean,
  ): m.Children {
    // Initialize window if needed
    if (!this.logsWindows.has(category.name)) {
      // Calculate row height based on max number of args
      const maxArgCount = category.logs.reduce((max, log) => {
        const argCount = this.getArgCount(log.args ?? null);
        return argCount > max ? argCount : max;
      }, 1);
      const rowHeightPx = maxArgCount * 25 + 10;
      this.logsWindows.set(category.name, {offset: 0, limit: 100, rowHeightPx});
    }
    const window = this.logsWindows.get(category.name)!;

    const columns = [];
    if (isMultiTrace) {
      columns.push({
        key: 'trace_id',
        header: m(GridHeaderCell, 'Trace'),
      });
    }
    columns.push(
      {
        key: 'ts',
        header: m(GridHeaderCell, 'Timestamp'),
      },
      {
        key: 'byte_offset',
        header: m(GridHeaderCell, 'Byte Offset'),
      },
      {
        key: 'args',
        header: m(GridHeaderCell, 'Details'),
      },
    );

    return m(Grid, {
      columns,
      rowData: {
        offset: window.offset,
        total: category.logs.length,
        data: category.logs
          .slice(window.offset, window.offset + window.limit)
          .map((log) => {
            const cells = [];
            if (isMultiTrace) {
              cells.push(
                m(
                  GridCell,
                  log.traceIndex !== null ? String(log.traceIndex) : '-',
                ),
              );
            }
            cells.push(
              m(GridCell, log.ts !== null ? renderTimecode(log.ts) : '-'),
              m(
                GridCell,
                log.byte_offset !== null ? String(log.byte_offset) : '-',
              ),
              m(GridCell, this.renderArgs(log.args)),
            );
            return cells;
          }),
        onLoadData: (offset: number, limit: number) => {
          this.logsWindows.set(category.name, {
            offset,
            limit,
            rowHeightPx: window.rowHeightPx,
          });
        },
      },
      virtualization: {
        rowHeightPx: window.rowHeightPx,
      },
      className: 'pf-trace-info-page__logs-grid',
    });
  }

  // Helper to parse args JSON and return null if empty/invalid
  private parseArgs(args: string | null): Record<string, unknown> | null {
    if (args === null) {
      return null;
    }
    try {
      const parsed = parseJsonWithBigints(args);
      return Object.keys(parsed).length > 0 ? parsed : null;
    } catch {
      return null;
    }
  }

  private getArgCount(args: string | null): number {
    const parsed = this.parseArgs(args);
    return parsed ? Object.keys(parsed).length : 1;
  }

  private renderArgs(args: string | null): m.Children {
    const parsed = this.parseArgs(args);
    if (!parsed) {
      return '-';
    }
    return m(
      '.pf-trace-info-page__args',
      Object.entries(parsed).map(([key, value]) =>
        m(
          '.pf-trace-info-page__arg-item',
          m('span.pf-trace-info-page__arg-key', `${key}:`),
          ' ',
          m('span.pf-trace-info-page__arg-value', String(value)),
        ),
      ),
    );
  }

  private renderStatsGrid(category: ErrorCategory): m.Children {
    return m(Grid, {
      columns: [
        {
          key: 'idx',
          header: m(GridHeaderCell, 'Index'),
        },
        {
          key: 'value',
          header: m(GridHeaderCell, 'Count'),
        },
      ],
      rowData: category.entries.map((row) => [
        m(GridCell, row.idx !== '' ? String(row.idx) : '-'),
        m(GridCell, String(row.value)),
      ]),
    });
  }
}

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
import {Engine} from '../../trace_processor/engine';
import {NUM_NULL, STR} from '../../trace_processor/query_result';
import {Icon} from '../../widgets/icon';
import {Tooltip} from '../../widgets/tooltip';
import {Card} from '../../widgets/card';
import {Grid, GridCell, GridHeaderCell} from '../../widgets/grid';

// All possible tab keys - single source of truth
export const ALL_TAB_KEYS = [
  'overview',
  'config',
  'android',
  'machines',
  'import_errors',
  'trace_errors',
  'data_losses',
  'ui_loading_errors',
  'stats',
] as const;

// Derive the TabKey type from the array
export type TabKey = (typeof ALL_TAB_KEYS)[number];

// Type guard to check if a string is a valid TabKey
export function isValidTabKey(key: string): key is TabKey {
  return ALL_TAB_KEYS.includes(key as TabKey);
}

// Shared stats spec and type
export const statsSpec = {
  name: STR,
  value: NUM_NULL,
  description: STR,
  idx: STR,
  severity: STR,
  source: STR,
};

export type StatsSectionRow = typeof statsSpec;

// Generic error category interface
export interface ErrorCategory {
  name: string;
  description: string;
  totalCount: number;
  entries: StatsSectionRow[];
}

// Load stats with a custom WHERE clause
export async function loadStatsWithFilter(
  engine: Engine,
  whereClause: string,
): Promise<StatsSectionRow[]> {
  const result = await engine.query(`
    select
      name,
      value,
      cast(ifnull(idx, '') as text) as idx,
      description,
      severity,
      source
    from stats
    where ${whereClause}
    order by name, idx
  `);

  const stats: StatsSectionRow[] = [];
  for (const iter = result.iter(statsSpec); iter.valid(); iter.next()) {
    stats.push({
      name: iter.name,
      value: iter.value,
      description: iter.description,
      idx: iter.idx,
      severity: iter.severity,
      source: iter.source,
    });
  }

  return stats;
}

// Group stats by category (name)
export function groupByCategory(stats: StatsSectionRow[]): ErrorCategory[] {
  const categoryMap = new Map<string, ErrorCategory>();
  for (const stat of stats) {
    const existing = categoryMap.get(stat.name);
    if (existing) {
      existing.totalCount += stat.value ?? 0;
      existing.entries.push(stat);
    } else {
      categoryMap.set(stat.name, {
        name: stat.name,
        description: stat.description,
        totalCount: stat.value ?? 0,
        entries: [stat],
      });
    }
  }
  return Array.from(categoryMap.values());
}

// Format file size from bytes to human-readable string
export function formatFileSize(bytes: bigint | number): {
  formatted: string;
  exact: string;
} {
  const numBytes = Number(bytes);
  const exact = `${numBytes.toLocaleString()} bytes`;

  let formatted: string;
  if (numBytes >= 1024 * 1024 * 1024) {
    formatted = `${(numBytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
  } else if (numBytes >= 1024 * 1024) {
    formatted = `${(numBytes / (1024 * 1024)).toFixed(2)} MB`;
  } else if (numBytes >= 1024) {
    formatted = `${(numBytes / 1024).toFixed(2)} KB`;
  } else {
    formatted = `${numBytes} bytes`;
  }

  return {formatted, exact};
}

// Render an error category card
export function renderErrorCategoryCard(
  category: ErrorCategory,
  severity: 'danger' | 'warning',
  icon: string,
): m.Children {
  const scrollToSection = () => {
    const targetId = categoryToId(category.name);
    const element = document.getElementById(targetId);
    if (element) {
      element.scrollIntoView({behavior: 'smooth', block: 'start'});
    }
  };

  return m(
    Card,
    {
      className: `pf-trace-info-page__status-card pf-trace-info-page__status-card--${severity} pf-trace-info-page__status-card--clickable`,
      onclick: scrollToSection,
    },
    m(
      '.pf-trace-info-page__status-card-main',
      m(Icon, {
        icon,
        className: 'pf-trace-info-page__status-icon',
        filled: true,
      }),
      m(
        '.pf-trace-info-page__status-content',
        m(
          '.pf-trace-info-page__status-title',
          {title: category.name},
          category.name,
          category.description &&
            m(
              Tooltip,
              {
                trigger: m(Icon, {
                  icon: 'help_outline',
                  className: 'pf-trace-info-page__help-icon',
                }),
              },
              category.description,
            ),
        ),
        m('.pf-trace-info-page__status-value', category.totalCount),
      ),
      m(
        '.pf-trace-info-page__status-link',
        m(Icon, {
          icon: 'arrow_downward',
          className: 'pf-trace-info-page__status-link-icon',
        }),
      ),
    ),
  );
}

// Helper to generate a safe HTML ID from a category name
function categoryToId(categoryName: string): string {
  return `category-${categoryName.replace(/[^a-zA-Z0-9_-]/g, '_')}`;
}

// Render a category section with detailed breakdown
export function renderCategorySection(
  category: ErrorCategory,
  options?: {className?: string},
): m.Children {
  return m(
    '',
    m('h3', {id: categoryToId(category.name)}, category.name),
    category.description && m('p', category.description),
    m(Grid, {
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
        m(GridCell, row.idx !== '' ? row.idx : '-'),
        m(GridCell, row.value !== null ? row.value : '-'),
      ]),
      className: options?.className,
    }),
  );
}

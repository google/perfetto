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
import {SqlValue} from '../../../trace_processor/query_result';
import {Box} from '../../../widgets/box';
import {Button} from '../../../widgets/button';
import {Chip} from '../../../widgets/chip';
import {Stack, StackAuto} from '../../../widgets/stack';
import {Filter} from './model';
import {DataGridApi} from './datagrid';
import {DataGridExportButton} from './export_button';

export class GridFilterBar implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m(Stack, {orientation: 'horizontal', wrap: true}, children);
  }
}

export interface GridFilterAttrs {
  readonly content: string;
  onRemove(): void;
}

export class GridFilterChip implements m.ClassComponent<GridFilterAttrs> {
  view({attrs}: m.Vnode<GridFilterAttrs>): m.Children {
    return m(Chip, {
      className: 'pf-grid-filter',
      label: attrs.content,
      removable: true,
      onRemove: attrs.onRemove,
      title: attrs.content,
      removeButtonTitle: 'Remove filter',
    });
  }
}

export type OnFilterRemove = (index: number) => void;

// Format a drill-down value for display
function formatDrillDownValue(value: SqlValue): string {
  if (value === null) return 'NULL';
  if (typeof value === 'string') return value;
  if (typeof value === 'number' || typeof value === 'bigint') {
    return value.toLocaleString();
  }
  if (value instanceof Uint8Array) return `<${value.length} bytes>`;
  return String(value);
}

export interface DrillDownIndicatorAttrs {
  readonly onBack: () => void;
  // The groupBy column names in order
  readonly groupBy: ReadonlyArray<string>;
  // The drill-down values keyed by column name
  readonly values: {readonly [key: string]: SqlValue};
  // Function to format a column name for display
  readonly formatColumnName: (columnName: string) => string;
}

export interface DataGridToolbarAttrs {
  readonly filters: ReadonlyArray<Filter>;
  readonly schema: unknown; // SchemaRegistry - avoid circular import
  readonly rootSchema: string;
  readonly totalRows: number;
  readonly showRowCount: boolean;
  readonly showExportButton: boolean;
  readonly toolbarItemsLeft?: m.Children;
  readonly toolbarItemsRight?: m.Children;
  readonly dataGridApi: DataGridApi;
  readonly onFilterRemove: OnFilterRemove;
  readonly formatFilter: (filter: Filter) => string;
  readonly drillDown?: DrillDownIndicatorAttrs;
}

export class DataGridToolbar implements m.ClassComponent<DataGridToolbarAttrs> {
  view({attrs}: m.Vnode<DataGridToolbarAttrs>): m.Children {
    const {
      filters,
      totalRows,
      showRowCount,
      showExportButton,
      toolbarItemsLeft,
      toolbarItemsRight,
      dataGridApi,
      onFilterRemove,
      formatFilter,
      drillDown,
    } = attrs;

    // Build left-side toolbar items
    const leftItems: m.Children[] = [];

    // Show back button and drill-down context when in drill-down mode
    if (drillDown) {
      leftItems.push(
        m(Button, {
          icon: 'arrow_back',
          label: 'Back to pivot',
          onclick: drillDown.onBack,
        }),
      );
      // Show chips for each drill-down value
      drillDown.groupBy.forEach((colName) => {
        const value = drillDown.values[colName];
        const displayName = drillDown.formatColumnName(colName);
        const displayValue = formatDrillDownValue(value);
        leftItems.push(
          m(Chip, {
            label: `${displayName}: ${displayValue}`,
            title: `Drilling down where ${displayName} = ${displayValue}`,
          }),
        );
      });
    }

    if (Boolean(toolbarItemsLeft)) {
      leftItems.push(toolbarItemsLeft);
    }

    // Filter chips in auto-expanding section
    if (filters.length > 0) {
      leftItems.push(
        m(StackAuto, [
          m(GridFilterBar, [
            filters.map((filter) => {
              return m(GridFilterChip, {
                content: formatFilter(filter),
                onRemove: () => {
                  const filterIndex = filters.indexOf(filter);
                  onFilterRemove(filterIndex);
                },
              });
            }),
          ]),
        ]),
      );
    }

    // Build right-side toolbar items
    const rightItems: m.Children[] = [];

    if (showRowCount) {
      const rowCountText = `${totalRows.toLocaleString()} rows`;
      rightItems.push(m('.pf-data-grid__row-count', rowCountText));
    }

    if (Boolean(toolbarItemsRight)) {
      rightItems.push(toolbarItemsRight);
    }

    if (showExportButton) {
      rightItems.push(m(DataGridExportButton, {api: dataGridApi}));
    }

    // Only render toolbar if there are items to show
    if (leftItems.length === 0 && rightItems.length === 0) {
      return undefined;
    }

    return m(
      Box,
      {className: 'pf-data-grid__toolbar', spacing: 'small'},
      m(
        '.pf-data-grid__toolbar-content',
        m(
          Stack,
          {
            className: 'pf-data-grid__toolbar-left',
            orientation: 'horizontal',
            spacing: 'small',
          },
          leftItems,
        ),
        m(
          Stack,
          {
            className: 'pf-data-grid__toolbar-right',
            orientation: 'horizontal',
            spacing: 'small',
          },
          rightItems,
        ),
      ),
    );
  }
}

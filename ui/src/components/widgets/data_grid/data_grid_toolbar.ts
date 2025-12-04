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
import {Icons} from '../../../base/semantic_icons';
import {SqlValue} from '../../../trace_processor/query_result';
import {Box} from '../../../widgets/box';
import {Button} from '../../../widgets/button';
import {Chip} from '../../../widgets/chip';
import {Stack, StackAuto} from '../../../widgets/stack';
import {ColumnDefinition, DataGridFilter, RowDef} from './common';
import {DataGridApi} from './data_grid';
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

export interface DrillDownIndicatorAttrs {
  // The drill-down values (groupBy column values)
  readonly drillDown: RowDef;
  // The groupBy column names in order
  readonly groupByColumns: ReadonlyArray<string>;
  // Callback to exit drill-down mode
  readonly onBack: () => void;
}

export interface PivotIndicatorAttrs {
  // The groupBy column names
  readonly groupByColumns: ReadonlyArray<string>;
  // Callback to exit pivot mode
  readonly onExit: () => void;
}

export interface DataGridToolbarAttrs {
  readonly filters: ReadonlyArray<DataGridFilter>;
  readonly columns: ReadonlyArray<ColumnDefinition>;
  readonly totalRows: number;
  readonly showFilters: boolean;
  readonly showRowCount: boolean;
  readonly showExportButton: boolean;
  readonly toolbarItemsLeft?: m.Children;
  readonly toolbarItemsRight?: m.Children;
  readonly dataGridApi: DataGridApi;
  readonly onFilterRemove: OnFilterRemove;
  readonly formatFilter: (
    filter: DataGridFilter,
    columns: ReadonlyArray<ColumnDefinition>,
  ) => string;
  // Optional pivot mode indicator props
  readonly pivot?: PivotIndicatorAttrs;
  // Optional drill-down indicator props
  readonly drillDown?: DrillDownIndicatorAttrs;
}

function formatDrillDownValue(value: SqlValue): string {
  if (value === null) return 'NULL';
  if (typeof value === 'string') return `"${value}"`;
  return String(value);
}

export class DataGridToolbar implements m.ClassComponent<DataGridToolbarAttrs> {
  view({attrs}: m.Vnode<DataGridToolbarAttrs>): m.Children {
    const {
      filters,
      columns,
      totalRows,
      showFilters,
      showRowCount,
      showExportButton,
      toolbarItemsLeft,
      toolbarItemsRight,
      dataGridApi,
      onFilterRemove,
      formatFilter,
      pivot,
      drillDown,
    } = attrs;

    // Build left-side toolbar items
    const leftItems: m.Children[] = [];

    // Pivot mode indicator (only show when in pivot mode, not drilldown)
    if (pivot && !drillDown) {
      const pivotText = `Grouped by: ${pivot.groupByColumns.join(', ')}`;

      leftItems.push(
        m(
          Stack,
          {orientation: 'horizontal', spacing: 'small', gap: 4},
          m(Chip, {
            className: 'pf-grid-pivot',
            label: pivotText,
            title: pivotText,
            removable: true,
            onRemove: pivot.onExit,
            removeButtonTitle: 'Exit pivot mode',
          }),
        ),
      );
    }

    // Drill-down indicator with back button
    if (drillDown) {
      const drillDownText = drillDown.groupByColumns
        .map(
          (col) => `${col}=${formatDrillDownValue(drillDown.drillDown[col])}`,
        )
        .join(', ');

      leftItems.push(
        m(
          Stack,
          {orientation: 'horizontal', spacing: 'small', gap: 4},
          m(Button, {
            icon: Icons.GoBack,
            label: 'Back to pivot',
            compact: true,
            onclick: drillDown.onBack,
          }),
          m(Chip, {
            className: 'pf-grid-drilldown',
            label: drillDownText,
            title: `Drill-down: ${drillDownText}`,
          }),
        ),
      );
    }

    if (Boolean(toolbarItemsLeft)) {
      leftItems.push(toolbarItemsLeft);
    }

    // Filter chips in auto-expanding section
    if (showFilters && filters.length > 0) {
      leftItems.push(
        m(StackAuto, [
          m(GridFilterBar, [
            filters.map((filter) => {
              return m(GridFilterChip, {
                content: formatFilter(filter, columns),
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

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
import {Row} from '../../../trace_processor/query_result';
import {Box} from '../../../widgets/box';
import {Button} from '../../../widgets/button';
import {Chip} from '../../../widgets/chip';
import {Stack, StackAuto} from '../../../widgets/stack';
import {isEmpty} from '../../../base/mithril_utils';

export class GridFilterBar implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m(Stack, {orientation: 'horizontal', wrap: true}, children);
  }
}

export interface GridFilterChipAttrs {
  readonly content: m.Children;
  onRemove(): void;
}

export class GridFilterChip implements m.ClassComponent<GridFilterChipAttrs> {
  view({attrs}: m.Vnode<GridFilterChipAttrs>): m.Children {
    return m(Chip, {
      className: 'pf-grid-filter',
      label: attrs.content,
      removable: true,
      onRemove: attrs.onRemove,
      removeButtonTitle: 'Remove filter',
    });
  }
}

export interface DataGridToolbarAttrs {
  readonly leftItems?: m.Children;
  readonly rightItems?: m.Children;
  readonly filterChips?: m.Children;

  // Drill-down state - when set, shows "Back to pivot" and drill-down values
  readonly drillDown?: Row;
  readonly drillDownFields?: readonly string[];
  readonly onExitDrillDown?: () => void;
}

export class DataGridToolbar implements m.ClassComponent<DataGridToolbarAttrs> {
  view({attrs}: m.Vnode<DataGridToolbarAttrs>): m.Children {
    const {
      leftItems,
      rightItems,
      filterChips,
      drillDown,
      drillDownFields,
      onExitDrillDown,
    } = attrs;

    // Build drill-down indicator
    const drillDownIndicator =
      drillDown &&
      drillDownFields &&
      onExitDrillDown &&
      m(
        Stack,
        {orientation: 'horizontal', spacing: 'small'},
        m(Button, {
          label: 'Back to pivot',
          icon: 'arrow_back',
          onclick: onExitDrillDown,
        }),
        drillDownFields.map((field) =>
          m(Chip, {
            className: 'pf-drill-down-chip',
            label: `${field} = ${formatDrillDownValue(drillDown[field])}`,
          }),
        ),
      );

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
          drillDownIndicator,
          !isEmpty(filterChips) &&
            m(
              StackAuto,
              m(Stack, {orientation: 'horizontal', wrap: true}, filterChips),
            ),
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

function formatDrillDownValue(value: unknown): string {
  if (value === null) return 'NULL';
  if (value === undefined) return '';
  if (typeof value === 'string') return `"${value}"`;
  return String(value);
}

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
import {BaseNodeData} from './node_types';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {Select} from '../../widgets/select';
import {ColumnPicker} from './column_picker';
import {ColumnDef} from './graph_utils';

export interface SortCondition {
  readonly column: string;
  readonly order: 'ASC' | 'DESC';
}

export interface SortNodeData extends BaseNodeData {
  readonly type: 'sort';
  // Legacy single-column fields (kept for backwards compat).
  readonly sortColumn: string;
  readonly sortOrder: 'ASC' | 'DESC';
  // New multi-column sort conditions.
  readonly conditions?: SortCondition[];
}

export function createSortNode(id: string, x: number, y: number): SortNodeData {
  return {
    type: 'sort',
    id,
    x,
    y,
    sortColumn: '',
    sortOrder: 'ASC',
    conditions: [],
  };
}

// Get the effective sort conditions, migrating legacy single-column data.
export function getSortConditions(node: SortNodeData): SortCondition[] {
  if (node.conditions && node.conditions.length > 0) {
    return node.conditions;
  }
  // Migrate legacy single-column format.
  if (node.sortColumn) {
    return [{column: node.sortColumn, order: node.sortOrder}];
  }
  return [];
}

// Convert sort conditions to SQL ORDER BY clause.
export function sortConditionsToSql(conditions: SortCondition[]): string {
  return conditions
    .filter((c) => c.column)
    .map((c) => `${c.column} ${c.order}`)
    .join(', ');
}

export function renderSortNode(
  node: SortNodeData,
  updateNode: (updates: Partial<Omit<SortNodeData, 'type' | 'id'>>) => void,
  availableColumns: ColumnDef[],
): m.Children {
  const conditions = getSortConditions(node);

  const updateConditions = (newConditions: SortCondition[]) => {
    updateNode({
      conditions: newConditions,
      // Keep legacy fields in sync with first condition.
      sortColumn: newConditions.length > 0 ? newConditions[0].column : '',
      sortOrder: newConditions.length > 0 ? newConditions[0].order : 'ASC',
    });
  };

  return m('.pf-qb-stack', [
    m('.pf-qb-filter-list', [
      ...conditions.map((cond, i) =>
        m(
          '.pf-qb-filter-row',
          {
            key: i,
            draggable: true,
            ondragstart: (e: DragEvent) => {
              e.dataTransfer!.effectAllowed = 'move';
              e.dataTransfer!.setData('text/plain', String(i));
              (e.currentTarget as HTMLElement).classList.add('pf-dragging');
            },
            ondragend: (e: DragEvent) => {
              (e.currentTarget as HTMLElement).classList.remove('pf-dragging');
            },
            ondragover: (e: DragEvent) => {
              e.preventDefault();
              e.dataTransfer!.dropEffect = 'move';
              const el = e.currentTarget as HTMLElement;
              const rect = el.getBoundingClientRect();
              const isBottom = e.clientY > rect.top + rect.height / 2;
              el.classList.toggle('pf-drag-over-top', !isBottom);
              el.classList.toggle('pf-drag-over-bottom', isBottom);
            },
            ondragleave: (e: DragEvent) => {
              const el = e.currentTarget as HTMLElement;
              el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
            },
            ondrop: (e: DragEvent) => {
              e.preventDefault();
              const el = e.currentTarget as HTMLElement;
              const isBottom = el.classList.contains('pf-drag-over-bottom');
              el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
              const fromIdx = parseInt(e.dataTransfer!.getData('text/plain'));
              let toIdx = isBottom ? i + 1 : i;
              if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                const updated = [...conditions];
                const [moved] = updated.splice(fromIdx, 1);
                if (fromIdx < toIdx) toIdx--;
                updated.splice(toIdx, 0, moved);
                updateConditions(updated);
              }
            },
          },
          [
            m(Icon, {
              icon: 'drag_indicator',
              className: 'pf-qb-drag-handle',
            }),
            m(ColumnPicker, {
              value: cond.column,
              columns: availableColumns,
              placeholder: 'column',
              onSelect: (value: string) => {
                const updated = [...conditions];
                updated[i] = {...cond, column: value};
                updateConditions(updated);
              },
            }),
            m(
              Select,
              {
                value: cond.order,
                onchange: (e: Event) => {
                  const updated = [...conditions];
                  updated[i] = {
                    ...cond,
                    order: (e.target as HTMLSelectElement).value as
                      | 'ASC'
                      | 'DESC',
                  };
                  updateConditions(updated);
                },
              },
              [
                m('option', {value: 'ASC'}, 'ASC'),
                m('option', {value: 'DESC'}, 'DESC'),
              ],
            ),
            m(Button, {
              icon: 'delete',
              intent: Intent.Danger,
              title: 'Remove sort condition',
              onclick: () => {
                updateConditions(conditions.filter((_, j) => j !== i));
              },
            }),
          ],
        ),
      ),
    ]),
    m(Button, {
      label: 'Add sort',
      icon: 'add',
      variant: ButtonVariant.Filled,
      onclick: () => {
        updateConditions([...conditions, {column: '', order: 'ASC'}]);
      },
    }),
  ]);
}

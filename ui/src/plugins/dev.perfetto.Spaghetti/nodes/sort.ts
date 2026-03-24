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
import {NodeManifest, RenderContext, SqlStatement} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {ColumnPicker} from '../widgets/column_picker';

export interface SortCondition {
  readonly column: string;
  readonly order: 'ASC' | 'DESC';
}

export interface SortConfig {
  readonly sortColumn: string;
  readonly sortOrder: 'ASC' | 'DESC';
  readonly conditions?: SortCondition[];
}

// Get the effective sort conditions, migrating legacy single-column data.
export function getSortConditions(config: SortConfig): SortCondition[] {
  if (config.conditions && config.conditions.length > 0) {
    return config.conditions;
  }
  // Migrate legacy single-column format.
  if (config.sortColumn) {
    return [{column: config.sortColumn, order: config.sortOrder}];
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

function SortNodeContent(): m.Component<{
  config: SortConfig;
  updateConfig: (updates: Partial<SortConfig>) => void;
  ctx: RenderContext;
}> {
  let dragging = false;
  let binHover = false;

  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const availableColumns = ctx.availableColumns;
      const conditions = getSortConditions(config);

      const updateConditions = (newConditions: SortCondition[]) => {
        updateConfig({
          conditions: newConditions,
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
                  dragging = true;
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove(
                    'pf-dragging',
                  );
                  dragging = false;
                  binHover = false;
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
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                },
                ondrop: (e: DragEvent) => {
                  e.preventDefault();
                  const el = e.currentTarget as HTMLElement;
                  const isBottom = el.classList.contains('pf-drag-over-bottom');
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                  const fromIdx = parseInt(
                    e.dataTransfer!.getData('text/plain'),
                  );
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
                m(SegmentedButtons, {
                  options: [{label: 'ASC'}, {label: 'DESC'}],
                  selectedOption: cond.order === 'ASC' ? 0 : 1,
                  onOptionSelected: (idx: number) => {
                    const updated = [...conditions];
                    updated[i] = {...cond, order: idx === 0 ? 'ASC' : 'DESC'};
                    updateConditions(updated);
                  },
                }),
              ],
            ),
          ),
        ]),
        m('.pf-qb-add-bin-wrapper', [
          m(Button, {
            label: 'Add sort by',
            icon: 'add',
            variant: ButtonVariant.Filled,
            onclick: () => {
              updateConditions([...conditions, {column: '', order: 'ASC'}]);
            },
          }),
          dragging
            ? m(
                '.pf-qb-drag-bin',
                {
                  className: binHover ? 'pf-drag-bin-hover' : '',
                  ondragover: (e: DragEvent) => {
                    e.preventDefault();
                    e.dataTransfer!.dropEffect = 'move';
                    binHover = true;
                  },
                  ondragleave: () => {
                    binHover = false;
                  },
                  ondrop: (e: DragEvent) => {
                    e.preventDefault();
                    binHover = false;
                    dragging = false;
                    const fromIdx = parseInt(
                      e.dataTransfer!.getData('text/plain'),
                    );
                    updateConditions(
                      conditions.filter((_, j) => j !== fromIdx),
                    );
                  },
                },
                m(Icon, {icon: 'delete'}),
              )
            : null,
        ]),
      ]);
    },
  };
}

function renderSortNode(
  config: SortConfig,
  updateConfig: (updates: Partial<SortConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(SortNodeContent, {config, updateConfig, ctx});
}

function isValid(config: SortConfig): boolean {
  return (
    (config.conditions ?? []).some((c) => c.column !== '') ||
    config.sortColumn !== ''
  );
}

function tryFold(stmt: SqlStatement, config: SortConfig): boolean {
  if (stmt.orderBy !== undefined || stmt.limit !== undefined) return false;
  const sortConds = getSortConditions(config);
  const orderBy = sortConditionsToSql(sortConds);
  if (orderBy) {
    stmt.orderBy = orderBy;
  }
  return true;
}

export const manifest: NodeManifest<SortConfig> = {
  title: 'Sort',
  icon: 'sort',
  inputs: [{name: 'input', content: 'Input', direction: 'left'}],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 178,
  defaultConfig: () => ({sortColumn: '', sortOrder: 'ASC', conditions: []}),
  render: renderSortNode,
  isValid,
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  tryFold,
};

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
import {TextInput} from '../../widgets/text_input';
import {ColumnPicker} from './column_picker';
import {ColumnDef} from './graph_utils';

export interface Aggregation {
  readonly func: 'COUNT' | 'SUM' | 'AVG' | 'MIN' | 'MAX';
  readonly column: string;
  readonly alias: string;
}

export interface GroupByNodeData extends BaseNodeData {
  readonly type: 'groupby';
  readonly groupColumns: string[];
  readonly aggregations: Aggregation[];
}

export function createGroupByNode(
  id: string,
  x: number,
  y: number,
): GroupByNodeData {
  return {type: 'groupby', id, x, y, groupColumns: [], aggregations: []};
}

export function renderGroupByNode(
  node: GroupByNodeData,
  updateNode: (updates: Partial<Omit<GroupByNodeData, 'type' | 'id'>>) => void,
  availableColumns: ColumnDef[],
): m.Children {
  return m('.pf-qb-stack', {style: {minWidth: '250px'}}, [
    m('.pf-qb-section-label', 'Group by'),
    m('.pf-qb-filter-list', [
      ...node.groupColumns.map((col, i) =>
        m(
          '.pf-qb-filter-row',
          {
            key: i,
            draggable: true,
            ondragstart: (e: DragEvent) => {
              e.dataTransfer!.effectAllowed = 'move';
              e.dataTransfer!.setData('text/plain', `group:${i}`);
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
              const data = e.dataTransfer!.getData('text/plain');
              if (!data.startsWith('group:')) return;
              const fromIdx = parseInt(data.slice(6));
              let toIdx = isBottom ? i + 1 : i;
              if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                const updated = [...node.groupColumns];
                const [moved] = updated.splice(fromIdx, 1);
                if (fromIdx < toIdx) toIdx--;
                updated.splice(toIdx, 0, moved);
                updateNode({groupColumns: updated});
              }
            },
          },
          [
            m(Icon, {
              icon: 'drag_indicator',
              className: 'pf-qb-drag-handle',
            }),
            m(ColumnPicker, {
              value: col,
              columns: availableColumns,
              placeholder: 'column',
              onSelect: (value: string) => {
                const updated = [...node.groupColumns];
                updated[i] = value;
                updateNode({groupColumns: updated});
              },
            }),
            m(Button, {
              icon: 'delete',
              intent: Intent.Danger,
              title: 'Remove grouping column',
              onclick: () => {
                updateNode({
                  groupColumns: node.groupColumns.filter((_, j) => j !== i),
                });
              },
            }),
          ],
        ),
      ),
    ]),
    m(Button, {
      label: 'Grouping',
      icon: 'add',
      variant: ButtonVariant.Filled,
      onclick: () => {
        updateNode({groupColumns: [...node.groupColumns, '']});
      },
    }),
    m('.pf-qb-section-label', 'Aggregations'),
    m('.pf-qb-filter-list', [
      ...node.aggregations.map((agg, i) =>
        m(
          '.pf-qb-filter-row',
          {
            key: i,
            draggable: true,
            ondragstart: (e: DragEvent) => {
              e.dataTransfer!.effectAllowed = 'move';
              e.dataTransfer!.setData('text/plain', `agg:${i}`);
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
              const data = e.dataTransfer!.getData('text/plain');
              if (!data.startsWith('agg:')) return;
              const fromIdx = parseInt(data.slice(4));
              let toIdx = isBottom ? i + 1 : i;
              if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                const newAggs = [...node.aggregations];
                const [moved] = newAggs.splice(fromIdx, 1);
                if (fromIdx < toIdx) toIdx--;
                newAggs.splice(toIdx, 0, moved);
                updateNode({aggregations: newAggs});
              }
            },
          },
          [
            m(Icon, {
              icon: 'drag_indicator',
              className: 'pf-qb-drag-handle',
            }),
            m(
              Select,
              {
                value: agg.func,
                onchange: (e: Event) => {
                  const newAggs = [...node.aggregations];
                  newAggs[i] = {
                    ...agg,
                    func: (e.target as HTMLSelectElement)
                      .value as Aggregation['func'],
                  };
                  updateNode({aggregations: newAggs});
                },
              },
              [
                m('option', {value: 'COUNT'}, 'COUNT'),
                m('option', {value: 'SUM'}, 'SUM'),
                m('option', {value: 'AVG'}, 'AVG'),
                m('option', {value: 'MIN'}, 'MIN'),
                m('option', {value: 'MAX'}, 'MAX'),
              ],
            ),
            m(ColumnPicker, {
              value: agg.column,
              columns: [{name: '*'}, ...availableColumns],
              placeholder: 'column',
              onSelect: (value: string) => {
                const newAggs = [...node.aggregations];
                newAggs[i] = {
                  ...agg,
                  column: value,
                  alias: agg.alias || `${agg.func.toLowerCase()}_${value}`,
                };
                updateNode({aggregations: newAggs});
              },
            }),
            m(TextInput, {
              placeholder: 'alias',
              value: agg.alias,
              onChange: (value: string) => {
                const newAggs = [...node.aggregations];
                newAggs[i] = {...agg, alias: value};
                updateNode({aggregations: newAggs});
              },
            }),
            m(Button, {
              icon: 'delete',
              intent: Intent.Danger,
              title: 'Remove aggregation',
              onclick: () => {
                const newAggs = node.aggregations.filter((_, j) => j !== i);
                updateNode({aggregations: newAggs});
              },
            }),
          ],
        ),
      ),
    ]),
    m(Button, {
      label: 'Aggregation',
      icon: 'add',
      variant: ButtonVariant.Filled,
      onclick: () => {
        updateNode({
          aggregations: [
            ...node.aggregations,
            {func: 'COUNT', column: '*', alias: ''},
          ],
        });
      },
    }),
  ]);
}

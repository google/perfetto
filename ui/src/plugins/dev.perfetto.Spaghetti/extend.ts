// Copyright (C) 2026 The Android Open Source Project
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
import {TextInput} from '../../widgets/text_input';
import {ColumnPicker} from './column_picker';
import {ColumnDef} from './graph_utils';

export interface ExtendColumn {
  readonly column: string;
  readonly alias: string;
}

export interface ExtendNodeData extends BaseNodeData {
  readonly type: 'extend';
  // Column from the left side to join on.
  readonly leftColumn: string;
  // Column from the right side to join on.
  readonly rightColumn: string;
  // Columns to pick from the right side. Duplicates allowed.
  readonly columns: ExtendColumn[];
}

export function createExtendNode(
  id: string,
  x: number,
  y: number,
): ExtendNodeData {
  return {
    type: 'extend',
    id,
    x,
    y,
    leftColumn: '',
    rightColumn: '',
    columns: [],
  };
}

// Compute the resolved aliases for all extend columns.
export function getExtendColumnAliases(
  node: ExtendNodeData,
  leftAvail: string[],
): ExtendColumn[] {
  const leftSet = new Set(leftAvail);
  return node.columns.map((c) => ({
    column: c.column,
    alias: c.alias || (leftSet.has(c.column) ? `right_${c.column}` : c.column),
  }));
}

export interface ExtendNodeRenderAttrs {
  readonly leftColumns: ColumnDef[];
  readonly rightColumns: ColumnDef[];
}

export function renderExtendNode(
  node: ExtendNodeData,
  updateNode: (updates: Partial<Omit<ExtendNodeData, 'type' | 'id'>>) => void,
  attrs: ExtendNodeRenderAttrs,
): m.Children {
  const {leftColumns, rightColumns} = attrs;
  const leftSet = new Set(leftColumns.map((c) => c.name));

  return m('.pf-qb-stack', {style: {minWidth: '200px'}}, [
    // Join key pickers side-by-side
    m('.pf-qb-section-label', 'Join on'),
    m('.pf-qb-group-grid', [
      m(ColumnPicker, {
        value: node.leftColumn,
        columns: leftColumns,
        placeholder: 'left col',
        onSelect: (value: string) => {
          updateNode({leftColumn: value});
        },
      }),
      m(ColumnPicker, {
        value: node.rightColumn,
        columns: rightColumns,
        placeholder: 'right col',
        onSelect: (value: string) => {
          updateNode({rightColumn: value});
        },
      }),
    ]),

    // Columns to add from right side
    m('.pf-qb-section-label', 'Columns to add'),
    m('.pf-qb-filter-list', [
      ...node.columns.map((entry, i) => {
        const defaultAlias = leftSet.has(entry.column)
          ? `right_${entry.column}`
          : entry.column;
        return m(
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
                const updated = [...node.columns];
                const [moved] = updated.splice(fromIdx, 1);
                if (fromIdx < toIdx) toIdx--;
                updated.splice(toIdx, 0, moved);
                updateNode({columns: updated});
              }
            },
          },
          [
            m(Icon, {
              icon: 'drag_indicator',
              className: 'pf-qb-drag-handle',
            }),
            m(ColumnPicker, {
              value: entry.column,
              columns: rightColumns,
              placeholder: 'column',
              onSelect: (value: string) => {
                const updated = [...node.columns];
                updated[i] = {...entry, column: value};
                updateNode({columns: updated});
              },
            }),
            m(TextInput, {
              placeholder: defaultAlias,
              value: entry.alias,
              onChange: (value: string) => {
                const updated = [...node.columns];
                updated[i] = {...entry, alias: value};
                updateNode({columns: updated});
              },
            }),
            m(Button, {
              icon: 'delete',
              intent: Intent.Danger,
              title: 'Remove column',
              onclick: () => {
                updateNode({
                  columns: node.columns.filter((_, j) => j !== i),
                });
              },
            }),
          ],
        );
      }),
    ]),
    m(Button, {
      label: 'Column',
      icon: 'add',
      variant: ButtonVariant.Filled,
      onclick: () => {
        updateNode({
          columns: [...node.columns, {column: '', alias: ''}],
        });
      },
    }),
  ]);
}

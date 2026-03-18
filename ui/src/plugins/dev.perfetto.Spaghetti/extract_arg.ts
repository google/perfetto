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

export interface ExtractArgEntry {
  readonly column: string;
  readonly argName: string;
  readonly alias: string;
}

export interface ExtractArgNodeData extends BaseNodeData {
  readonly type: 'extract_arg';
  readonly extractions: ExtractArgEntry[];
}

export function createExtractArgNode(
  id: string,
  x: number,
  y: number,
): ExtractArgNodeData {
  return {type: 'extract_arg', id, x, y, extractions: []};
}

export function renderExtractArgNode(
  node: ExtractArgNodeData,
  updateNode: (
    updates: Partial<Omit<ExtractArgNodeData, 'type' | 'id'>>,
  ) => void,
  availableColumns: ColumnDef[],
): m.Children {
  return m('.pf-qb-stack', [
    m('.pf-qb-filter-list', [
      ...node.extractions.map((entry, i) =>
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
                const newExtractions = [...node.extractions];
                const [moved] = newExtractions.splice(fromIdx, 1);
                if (fromIdx < toIdx) toIdx--;
                newExtractions.splice(toIdx, 0, moved);
                updateNode({extractions: newExtractions});
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
              columns: availableColumns,
              placeholder: 'column',
              onSelect: (value: string) => {
                const newExtractions = [...node.extractions];
                newExtractions[i] = {...entry, column: value};
                updateNode({extractions: newExtractions});
              },
            }),
            m(TextInput, {
              placeholder: 'arg name',
              value: entry.argName,
              onChange: (value: string) => {
                const newExtractions = [...node.extractions];
                newExtractions[i] = {...entry, argName: value};
                updateNode({extractions: newExtractions});
              },
            }),
            m(TextInput, {
              placeholder: 'alias',
              value: entry.alias,
              onChange: (value: string) => {
                const newExtractions = [...node.extractions];
                newExtractions[i] = {...entry, alias: value};
                updateNode({extractions: newExtractions});
              },
            }),
            m(Button, {
              icon: 'delete',
              intent: Intent.Danger,
              title: 'Remove extraction',
              onclick: () => {
                const newExtractions = node.extractions.filter(
                  (_, j) => j !== i,
                );
                updateNode({extractions: newExtractions});
              },
            }),
          ],
        ),
      ),
    ]),
    m(Button, {
      label: 'Add extraction',
      variant: ButtonVariant.Filled,
      icon: 'add',
      onclick: () => {
        updateNode({
          extractions: [
            ...node.extractions,
            {column: '', argName: '', alias: ''},
          ],
        });
      },
    }),
  ]);
}

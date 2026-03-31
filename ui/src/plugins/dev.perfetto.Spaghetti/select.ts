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
import {MultiSelectDiff, PopupMultiSelect} from '../../widgets/multiselect';
import {TextInput} from '../../widgets/text_input';
import {ColumnDef} from './graph_utils';

export interface SelectExpression {
  readonly expression: string;
  readonly alias: string;
}

export interface SelectNodeData extends BaseNodeData {
  readonly type: 'select';
  readonly columns: Record<string, boolean>;
  readonly expressions: SelectExpression[];
}

export function createSelectNode(
  id: string,
  x: number,
  y: number,
): SelectNodeData {
  return {type: 'select', id, x, y, columns: {}, expressions: []};
}

export function renderSelectNode(
  node: SelectNodeData,
  updateNode: (updates: Partial<Omit<SelectNodeData, 'type' | 'id'>>) => void,
  availableColumns: ColumnDef[],
): m.Children {
  if (availableColumns.length === 0) {
    return m('span.pf-qb-placeholder', 'Connect to a table source');
  }

  // Merge available columns with current selection state.
  // Columns that exist upstream but aren't in node.columns yet default to true.
  const mergedColumns: Record<string, boolean> = {};
  for (const col of availableColumns) {
    mergedColumns[col.name] =
      col.name in node.columns ? node.columns[col.name] : true;
  }

  const options = availableColumns.map((col) => ({
    id: col.name,
    name: col.name,
    checked: mergedColumns[col.name],
  }));

  return m('.pf-qb-stack', [
    m(PopupMultiSelect, {
      label: 'Columns',
      showNumSelected: true,
      options,
      onChange: (diffs: MultiSelectDiff[]) => {
        const updated = {...mergedColumns};
        for (const diff of diffs) {
          updated[diff.id] = diff.checked;
        }
        updateNode({columns: updated});
      },
    }),
    m('.pf-qb-section-label', 'Expressions'),
    m('.pf-qb-3col-grid', [
      ...node.expressions.flatMap((expr, i) => [
        m(TextInput, {
          placeholder: 'expression',
          value: expr.expression,
          onChange: (value: string) => {
            const newExprs = [...node.expressions];
            newExprs[i] = {...expr, expression: value};
            updateNode({expressions: newExprs});
          },
        }),
        m(TextInput, {
          placeholder: 'alias',
          value: expr.alias,
          onChange: (value: string) => {
            const newExprs = [...node.expressions];
            newExprs[i] = {...expr, alias: value};
            updateNode({expressions: newExprs});
          },
        }),
        m(Button, {
          icon: 'delete',
          intent: Intent.Danger,
          title: 'Remove expression',
          onclick: () => {
            const newExprs = node.expressions.filter((_, j) => j !== i);
            updateNode({expressions: newExprs});
          },
        }),
      ]),
    ]),
    m(Button, {
      label: 'Add expression',
      variant: ButtonVariant.Filled,
      icon: 'add',
      onclick: () => {
        updateNode({
          expressions: [...node.expressions, {expression: '', alias: ''}],
        });
      },
    }),
  ]);
}

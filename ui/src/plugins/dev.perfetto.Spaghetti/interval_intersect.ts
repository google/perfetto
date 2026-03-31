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
import {Checkbox} from '../../widgets/checkbox';
import {BaseNodeData} from './node_types';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {TextInput} from '../../widgets/text_input';

export interface IntervalIntersectNodeData extends BaseNodeData {
  readonly type: 'interval_intersect';
  // Columns to partition by during interval intersection.
  readonly partitionColumns: string[];
  // Filter out rows with dur < 0.
  readonly filterNegativeDur: boolean;
}

export function createIntervalIntersectNode(
  id: string,
  x: number,
  y: number,
): IntervalIntersectNodeData {
  return {
    type: 'interval_intersect',
    id,
    x,
    y,
    partitionColumns: [],
    filterNegativeDur: true,
  };
}

export function renderIntervalIntersectNode(
  node: IntervalIntersectNodeData,
  updateNode: (
    updates: Partial<Omit<IntervalIntersectNodeData, 'type' | 'id'>>,
  ) => void,
): m.Children {
  return m('.pf-qb-stack', [
    m(Checkbox, {
      label: 'Filter dur >= 0',
      checked: node.filterNegativeDur,
      onchange: () => updateNode({filterNegativeDur: !node.filterNegativeDur}),
    }),
    m('.pf-qb-section-label', 'Partition by'),
    m('.pf-qb-filter-list', [
      ...node.partitionColumns.map((col, i) =>
        m('.pf-qb-filter-row', {key: i}, [
          m(TextInput, {
            value: col,
            placeholder: 'column',
            onChange: (value: string) => {
              const updated = [...node.partitionColumns];
              updated[i] = value;
              updateNode({partitionColumns: updated});
            },
          }),
          m(Button, {
            icon: 'delete',
            intent: Intent.Danger,
            title: 'Remove partition column',
            onclick: () => {
              updateNode({
                partitionColumns: node.partitionColumns.filter(
                  (_, j) => j !== i,
                ),
              });
            },
          }),
        ]),
      ),
    ]),
    m(Button, {
      label: 'Column',
      icon: 'add',
      variant: ButtonVariant.Filled,
      onclick: () => {
        updateNode({
          partitionColumns: [...node.partitionColumns, ''],
        });
      },
    }),
  ]);
}

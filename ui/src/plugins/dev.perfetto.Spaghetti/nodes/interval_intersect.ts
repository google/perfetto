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
import {Checkbox} from '../../../widgets/checkbox';
import {NodeManifest} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {TextInput} from '../../../widgets/text_input';

export interface IntervalIntersectConfig {
  readonly partitionColumns: string[];
  readonly filterNegativeDur: boolean;
}

export const manifest: NodeManifest<IntervalIntersectConfig> = {
  title: 'Interval Intersect',
  icon: 'compare_arrows',
  inputs: [
    {name: 'input_1', content: 'Input 1', direction: 'left'},
    {name: 'input_2', content: 'Input 2', direction: 'left'},
  ],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 340,
  defaultConfig: () => ({partitionColumns: [], filterNegativeDur: true}),
  isValid: () => true,
  getOutputColumns(config) {
    const result = [
      {name: 'ts', type: {kind: 'timestamp' as const}},
      {name: 'dur', type: {kind: 'duration' as const}},
      {name: 'id_0', type: {kind: 'int' as const}},
      {name: 'id_1', type: {kind: 'int' as const}},
    ];
    for (const col of config.partitionColumns) {
      if (col) result.push({name: col, type: undefined as any});
    }
    return result;
  },
  emitIr(config, ctx) {
    const leftRef = ctx.getInputRef('input_1');
    const rightRef = ctx.getInputRef('input_2');
    const leftArg = config.filterNegativeDur
      ? `(SELECT * FROM ${leftRef} WHERE dur >= 0)`
      : leftRef;
    const rightArg = config.filterNegativeDur
      ? `(SELECT * FROM ${rightRef} WHERE dur >= 0)`
      : rightRef;
    const partitionCols = config.partitionColumns.filter((c) => c);
    const partitionClause =
      partitionCols.length > 0 ? partitionCols.join(', ') : '';
    const sql = `SELECT *\nFROM _interval_intersect!(\n  (${leftArg},\n   ${rightArg}),\n  (${partitionClause})\n)`;
    return {sql, includes: ['intervals.intersect']};
  },
  render(config, updateConfig) {
    return m('.pf-qb-stack', [
      m(Checkbox, {
        label: 'Filter dur >= 0',
        checked: config.filterNegativeDur,
        onchange: () =>
          updateConfig({filterNegativeDur: !config.filterNegativeDur}),
      }),
      m('.pf-qb-section-label', 'Partition by'),
      m('.pf-qb-filter-list', [
        ...config.partitionColumns.map((col, i) =>
          m('.pf-qb-filter-row', {key: i}, [
            m(TextInput, {
              value: col,
              placeholder: 'column',
              onChange: (value: string) => {
                const updated = [...config.partitionColumns];
                updated[i] = value;
                updateConfig({partitionColumns: updated});
              },
            }),
            m(Button, {
              icon: 'delete',
              variant: ButtonVariant.Filled,
              intent: Intent.Danger,
              className: 'pf-qb-row-delete',
              title: 'Remove partition column',
              onclick: () => {
                updateConfig({
                  partitionColumns: config.partitionColumns.filter(
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
          updateConfig({
            partitionColumns: [...config.partitionColumns, ''],
          });
        },
      }),
    ]);
  },
};

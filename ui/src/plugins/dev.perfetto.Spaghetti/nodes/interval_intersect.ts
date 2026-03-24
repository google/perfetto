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
import {NodeManifest, RenderContext} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {TextInput} from '../../../widgets/text_input';
import {ColumnDef} from '../graph_utils';

export interface IntervalIntersectConfig {
  readonly partitionColumns: string[];
  readonly filterNegativeDur: boolean;
}

function IntervalIntersectContent(): m.Component<{
  config: IntervalIntersectConfig;
  updateConfig: (updates: Partial<IntervalIntersectConfig>) => void;
  ctx: RenderContext;
}> {
  let dragging = false;
  let binHover = false;

  return {
    view({attrs: {config, updateConfig}}) {
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
                    const updated = [...config.partitionColumns];
                    const [moved] = updated.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    updated.splice(toIdx, 0, moved);
                    updateConfig({partitionColumns: updated});
                  }
                },
              },
              [
                m(Icon, {
                  icon: 'drag_indicator',
                  className: 'pf-qb-drag-handle',
                }),
                m(TextInput, {
                  value: col,
                  placeholder: 'column',
                  onChange: (value: string) => {
                    const updated = [...config.partitionColumns];
                    updated[i] = value;
                    updateConfig({partitionColumns: updated});
                  },
                }),
              ],
            ),
          ),
        ]),
        m('.pf-qb-add-bin-wrapper', [
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
                    updateConfig({
                      partitionColumns: config.partitionColumns.filter(
                        (_, j) => j !== fromIdx,
                      ),
                    });
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
  getOutputColumns(config, ctx) {
    const result: ColumnDef[] = [
      {name: 'ts', type: {kind: 'timestamp' as const}},
      {name: 'dur', type: {kind: 'duration' as const}},
      {name: 'id_0', type: {kind: 'int' as const}},
      {name: 'id_1', type: {kind: 'int' as const}},
    ];
    for (const col of config.partitionColumns) {
      const inputCols = ctx.getInputColumns('input_1');
      const orig = inputCols?.find((c) => c.name === col);
      if (col) result.push({name: col, type: orig?.type});
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
  render(config, updateConfig, ctx) {
    return m(IntervalIntersectContent, {config, updateConfig, ctx});
  },
};

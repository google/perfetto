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
import {ManifestPort, NodeManifest, RenderContext} from '../node_types';
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
    view({attrs: {config, updateConfig, ctx}}) {
      const n = ctx.inputPorts.length;
      const canRemove = ctx.removeLastInput !== undefined && n > 2;
      return m('.pf-qb-stack', [
        m(Checkbox, {
          label: 'Filter dur >= 0',
          checked: config.filterNegativeDur,
          onchange: () =>
            updateConfig({filterNegativeDur: !config.filterNegativeDur}),
        }),
        m('div', {style: {display: 'flex', gap: '4px'}}, [
          canRemove &&
            m(Button, {
              label: '− Input',
              variant: ButtonVariant.Filled,
              style: {flex: '1'},
              onclick: ctx.removeLastInput,
            }),
          ctx.addInput &&
            m(Button, {
              label: '+ Input',
              variant: ButtonVariant.Filled,
              style: {flex: '1'},
              onclick: () =>
                ctx.addInput!({
                  name: `input_${n + 1}`,
                  content: `Input ${n + 1}`,
                  direction: 'left' as const,
                }),
            }),
        ]),
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

// Columns intrinsic to _interval_intersect!() output — skip these when
// pulling in source columns to avoid duplicates.
const SKIP_COLS = new Set(['id', 'ts', 'dur']);

interface SourceColumn {
  readonly name: string; // original column name in the source table
  readonly alias: string; // output name (may have suffix for collisions)
  readonly type?: ColumnDef['type'];
}

// Compute extra columns to pull from N inputs, with collision handling.
// Earlier inputs keep original names; later inputs get a _{inputIdx} suffix
// when a name collides with an already-taken name.
function getSourceColumnsForInputs(
  inputCols: (ColumnDef[] | undefined)[],
  numInputs: number,
): SourceColumn[][] {
  const taken = new Set(SKIP_COLS);
  for (let i = 0; i < numInputs; i++) {
    taken.add(`id_${i}`);
  }

  return inputCols.map((cols, inputIdx) => {
    const sourceCols: SourceColumn[] = [];
    for (const col of cols ?? []) {
      if (SKIP_COLS.has(col.name)) continue;
      let alias = col.name;
      if (taken.has(alias)) {
        let suffix = inputIdx;
        alias = `${col.name}_${suffix}`;
        while (taken.has(alias)) {
          suffix++;
          alias = `${col.name}_${suffix}`;
        }
      }
      taken.add(alias);
      sourceCols.push({name: col.name, alias, type: col.type});
    }
    return sourceCols;
  });
}

export const manifest: NodeManifest<IntervalIntersectConfig> = {
  title: 'Interval Intersect',
  icon: 'compare_arrows',
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 340,
  defaultInputs(): ManifestPort[] {
    return [
      {name: 'input_1', content: 'Input 1', direction: 'left'},
      {name: 'input_2', content: 'Input 2', direction: 'left'},
    ];
  },
  defaultConfig: () => ({partitionColumns: [], filterNegativeDur: true}),
  isValid: () => true,
  getOutputColumns(_config, ctx) {
    const connectedPorts = ctx.inputPorts.filter(
      (p) => ctx.getInputColumns(p.name) !== undefined,
    );
    if (connectedPorts.length === 0) return undefined;
    const inputCols = connectedPorts.map((p) => ctx.getInputColumns(p.name));
    const sourceCols = getSourceColumnsForInputs(
      inputCols,
      connectedPorts.length,
    );

    const result: ColumnDef[] = [
      {name: 'ts', type: {kind: 'timestamp' as const}},
      {name: 'dur', type: {kind: 'duration' as const}},
    ];
    for (let i = 0; i < connectedPorts.length; i++) {
      const idType = inputCols[i]?.find((c) => c.name === 'id')?.type;
      result.push({name: `id_${i}`, type: idType ?? {kind: 'int' as const}});
    }
    for (const cols of sourceCols) {
      for (const col of cols) {
        result.push({name: col.alias, type: col.type});
      }
    }
    return result;
  },
  emitIr(config, ctx) {
    const connectedPorts = ctx.inputPorts.filter(
      (p) => ctx.getInputRef(p.name) !== '',
    );
    if (connectedPorts.length === 0) return undefined;
    const refs = connectedPorts.map((p) => ctx.getInputRef(p.name));
    const inputCols = connectedPorts.map((p) => ctx.getInputColumns(p.name));
    const sourceCols = getSourceColumnsForInputs(
      inputCols,
      connectedPorts.length,
    );

    const partitionCols = config.partitionColumns.filter((c) => c);
    const partitionClause =
      partitionCols.length > 0 ? partitionCols.join(', ') : '';

    const args = refs.map((ref) =>
      config.filterNegativeDur ? `(SELECT * FROM ${ref} WHERE dur >= 0)` : ref,
    );

    const n = connectedPorts.length;
    const selectParts = ['ii.ts', 'ii.dur'];
    for (let i = 0; i < n; i++) {
      selectParts.push(`ii.id_${i}`);
    }
    const tableAliases = refs.map((_, i) => `t${i + 1}`);
    for (let i = 0; i < n; i++) {
      for (const col of sourceCols[i]) {
        const expr = `${tableAliases[i]}.${col.name}`;
        selectParts.push(
          col.alias !== col.name ? `${expr} AS ${col.alias}` : expr,
        );
      }
    }

    const hasSourceCols = sourceCols.some((cols) => cols.length > 0);
    const argsStr = args.join(',\n   ');

    let sql: string;
    if (hasSourceCols) {
      const joins = refs
        .map(
          (ref, i) =>
            `JOIN ${ref} ${tableAliases[i]} ON ii.id_${i} = ${tableAliases[i]}.id`,
        )
        .join('\n');
      sql =
        `SELECT\n${selectParts.map((s) => `  ${s}`).join(',\n')}` +
        `\nFROM _interval_intersect!(\n  (${argsStr}),\n  (${partitionClause})\n) ii` +
        `\n${joins}`;
    } else {
      sql = `SELECT *\nFROM _interval_intersect!(\n  (${argsStr}),\n  (${partitionClause})\n)`;
    }
    return {sql, includes: ['intervals.intersect']};
  },
  render(config, updateConfig, ctx) {
    return m(IntervalIntersectContent, {config, updateConfig, ctx});
  },
};

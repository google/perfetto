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
import {z} from 'zod';
import {Select} from '../../../widgets/select';
import type {ColumnDef} from '../graph_utils';
import type {PipelineAggFunc} from '../sql_types';
import {ColumnPicker} from '../widgets/column_picker';
import {AliasTag} from './alias_tag';

const PIPELINE_AGG_FUNCS: readonly PipelineAggFunc[] = [
  'COUNT',
  'SUM',
  'MIN',
  'MAX',
];

export const PipelineAggEntrySchema = z.object({
  func: z.enum(['COUNT', 'SUM', 'MIN', 'MAX']),
  column: z.string(),
  alias: z.string(),
});

export type PipelineAggEntry = z.infer<typeof PipelineAggEntrySchema>;

export function pipelineAggAlias(
  entry: PipelineAggEntry,
  prefix?: string,
): string {
  if (entry.alias !== '') return entry.alias;
  const base =
    entry.func === 'COUNT'
      ? 'count'
      : `${entry.func.toLowerCase()}_${entry.column}`;
  return prefix ? `${prefix}_${base}` : base;
}

export interface PipelineAggRowAttrs {
  readonly value: PipelineAggEntry;
  readonly columns: ColumnDef[];
  readonly aliasPlaceholder?: string;
  readonly onChange: (updated: PipelineAggEntry) => void;
}

export class PipelineAggRow implements m.ClassComponent<PipelineAggRowAttrs> {
  view({attrs}: m.CVnode<PipelineAggRowAttrs>): m.Children {
    const {value, columns, aliasPlaceholder, onChange} = attrs;

    return [
      m(
        Select,
        {
          value: value.func,
          onchange: (e: Event) => {
            const nextFunc = (e.target as HTMLSelectElement)
              .value as PipelineAggFunc;
            onChange({
              ...value,
              func: nextFunc,
              column:
                nextFunc === 'COUNT'
                  ? '*'
                  : value.column === '*'
                    ? ''
                    : value.column,
            });
          },
        },
        PIPELINE_AGG_FUNCS.map((fn) => m('option', {value: fn}, fn)),
      ),
      value.func !== 'COUNT' &&
        m(ColumnPicker, {
          value: value.column,
          columns,
          placeholder: 'column',
          onSelect: (column) => onChange({...value, column}),
        }),
      m(AliasTag, {
        alias: value.alias,
        placeholder: aliasPlaceholder ?? pipelineAggAlias(value),
        onChange: (alias) => onChange({...value, alias}),
      }),
    ];
  }
}

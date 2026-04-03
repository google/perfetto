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
import {IrEntry} from '../ir';
import {EntryReport} from '../materialization';

export interface IrTabAttrs {
  readonly irEntries: IrEntry[];
  readonly reportByHash: Map<string, EntryReport>;
  readonly activeNodeId: string | undefined;
}

function renderIrBlock(
  entry: IrEntry,
  reportByHash: Map<string, EntryReport>,
): m.Children {
  const meta: string[] = [];
  if (entry.nodeIds.length > 0) meta.push(`nodes: ${entry.nodeIds.join(', ')}`);
  if (entry.deps.length > 0) meta.push(`deps: ${entry.deps.join(', ')}`);
  if (entry.includes.length > 0)
    {meta.push(`includes: ${entry.includes.join(', ')}`);}
  const report = reportByHash.get(entry.hash);
  return m('.pf-qb-ir-block', [
    m('.pf-qb-ir-block-header', [
      m('span.pf-qb-ir-hash', entry.hash),
      meta.length > 0 && m('span.pf-qb-ir-meta', meta.join(' · ')),
      report &&
        m('.pf-qb-ir-badges', [
          report.cacheHit &&
            m(
              'span.pf-qb-ir-badge',
              {className: 'pf-qb-ir-badge--hit'},
              'CACHED',
            ),
          !report.cacheHit &&
            m('span.pf-qb-ir-time', `${report.timeMs.toFixed(1)}ms`),
        ]),
    ]),
    m('pre.pf-qb-ir-sql', entry.sql),
  ]);
}

export function renderIrTab(attrs: IrTabAttrs): m.Children {
  const {irEntries, reportByHash, activeNodeId} = attrs;
  return m(
    '',
    {
      style: {
        display: 'flex',
        flexDirection: 'column',
        flex: '1',
        overflow: 'auto',
        padding: '8px',
        gap: '8px',
      },
    },
    irEntries.length > 0
      ? irEntries.map((e) => renderIrBlock(e, reportByHash))
      : m(
          'span',
          {style: {opacity: '0.5', fontSize: '12px'}},
          activeNodeId ? 'No IR available' : 'Select a node',
        ),
  );
}

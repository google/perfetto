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
import type {IrEntry} from '../ir';
import type {EntryReport} from '../materialization';
import {Card} from '../components/card';
import './materialization_tab.scss';

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
  if (entry.includes.length > 0) {
    meta.push(`includes: ${entry.includes.join(', ')}`);
  }
  const report = reportByHash.get(entry.hash);
  const badges = report
    ? [
        report.cacheHit && m(Card.Badge, {variant: 'hit', label: 'CACHED'}),
        !report.cacheHit &&
          m(Card.Time, {label: `${report.timeMs.toFixed(1)}ms`}),
      ]
    : undefined;

  return m(Card, {
    title: entry.hash,
    content: entry.sql,
    meta: meta.length > 0 ? meta.join(' · ') : undefined,
    badges,
  });
}

export class IrTab implements m.ClassComponent<IrTabAttrs> {
  view({attrs}: m.Vnode<IrTabAttrs>) {
    const {irEntries, reportByHash, activeNodeId} = attrs;
    return m(
      '.pf-spag-materialization-tab',
      irEntries.length > 0
        ? irEntries.map((e) => renderIrBlock(e, reportByHash))
        : m(
            'span.pf-spag-materialization-tab-empty',
            activeNodeId ? 'No IR available' : 'Select a node',
          ),
    );
  }
}

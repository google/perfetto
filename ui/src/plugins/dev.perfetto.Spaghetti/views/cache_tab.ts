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
import {Button, ButtonVariant} from '../../../widgets/button';
import {CacheEntry} from '../materialization';

export interface CacheTabAttrs {
  readonly cacheEntries: readonly CacheEntry[];
  readonly onClearCache: () => void;
}

function formatTimestamp(perfNow: number): string {
  const wallMs = Date.now() - (performance.now() - perfNow);
  const d = new Date(wallMs);
  return d.toLocaleTimeString();
}

export function renderCacheTab(attrs: CacheTabAttrs): m.Children {
  const {cacheEntries, onClearCache} = attrs;
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
    cacheEntries.length > 0
      ? [
          m(
            '',
            {style: {display: 'flex', justifyContent: 'flex-end'}},
            m(Button, {
              variant: ButtonVariant.Filled,
              icon: 'delete_sweep',
              label: 'Clear cache',
              onclick: onClearCache,
            }),
          ),
          ...[...cacheEntries]
            .sort((a, b) => b.lastHitAt - a.lastHitAt)
            .map((entry) =>
              m('.pf-qb-ir-block', [
                m('.pf-qb-ir-block-header', [
                  m('span.pf-qb-ir-hash', entry.hash),
                  m(
                    'span.pf-qb-ir-meta',
                    `created ${formatTimestamp(entry.createdAt)} · last hit ${formatTimestamp(entry.lastHitAt)}`,
                  ),
                  m('.pf-qb-ir-badges', [
                    m(
                      'span.pf-qb-ir-badge.pf-qb-ir-badge--hits',
                      `${entry.hitCount} ${entry.hitCount === 1 ? 'hit' : 'hits'}`,
                    ),
                    m(
                      'span.pf-qb-ir-time',
                      `${entry.materializeTimeMs.toFixed(1)}ms`,
                    ),
                  ]),
                ]),
                m('pre.pf-qb-ir-sql', entry.sql),
              ]),
            ),
        ]
      : m(
          'span',
          {style: {opacity: '0.5', fontSize: '12px'}},
          'Cache is empty',
        ),
  );
}

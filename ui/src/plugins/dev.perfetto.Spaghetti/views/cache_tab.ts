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
import type {CacheEntry} from '../materialization';
import {Card} from '../components/card';
import './cache_tab.scss';

export interface CacheTabAttrs {
  readonly cacheEntries: readonly CacheEntry[];
  readonly onClearCache: () => void;
}

function formatTimestamp(perfNow: number): string {
  const wallMs = Date.now() - (performance.now() - perfNow);
  const d = new Date(wallMs);
  return d.toLocaleTimeString();
}

export class CacheTab implements m.ClassComponent<CacheTabAttrs> {
  view({attrs}: m.Vnode<CacheTabAttrs>) {
    const {cacheEntries, onClearCache} = attrs;
    return m(
      '.pf-spag-cache-tab',
      cacheEntries.length > 0
        ? [
            m(
              '.pf-spag-cache-tab-toolbar',
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
                m(Card, {
                  title: entry.hash,
                  content: entry.sql,
                  meta: `created ${formatTimestamp(entry.createdAt)} · last hit ${formatTimestamp(entry.lastHitAt)}`,
                  badges: [
                    m(Card.Badge, {
                      variant: 'hits',
                      label: `${entry.hitCount} ${entry.hitCount === 1 ? 'hit' : 'hits'}`,
                    }),
                    m(Card.Time, {
                      label: `${entry.materializeTimeMs.toFixed(1)}ms`,
                    }),
                  ],
                }),
              ),
          ]
        : m('span.pf-spag-cache-tab-empty', 'Cache is empty'),
    );
  }
}

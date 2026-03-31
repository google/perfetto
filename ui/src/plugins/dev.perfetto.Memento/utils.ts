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
import {type SnapshotData} from './memento_session';

/** Compute the earliest timestamp across all data. */
export function computeT0(data: SnapshotData): number {
  let minTs = Infinity;
  for (const arr of data.systemCounters.values()) {
    if (arr.length > 0 && arr[0].ts < minTs) minTs = arr[0].ts;
  }
  for (const counterMap of data.processCountersByName.values()) {
    for (const byTs of counterMap.values()) {
      const firstTs = byTs.keys().next().value;
      if (firstTs !== undefined && firstTs < minTs) minTs = firstTs;
    }
  }
  return minTs < Infinity ? minTs : 0;
}

export function panel(
  title: string,
  subtitle: string | undefined,
  body: m.Children,
): m.Children {
  return m(
    '.pf-memento-panel',
    m(
      '.pf-memento-panel__header',
      m('h2', title),
      subtitle !== undefined && m('p', subtitle),
    ),
    m('.pf-memento-panel__body', body),
  );
}

export function formatKb(kb: number): string {
  if (kb < 1024) return `${kb.toLocaleString()} KB`;
  if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(1)} MB`;
  return `${(kb / (1024 * 1024)).toFixed(1)} GB`;
}

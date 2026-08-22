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
import {classNames} from '../../base/classnames';
import type {TracePreset} from '../query/bigtrace_query_client';

// Group presets by CUJ (their `category`), preserving first-seen order.
// Shared by the home page (cards) and the settings page (chips).
export function groupPresetsByCuj(presets: ReadonlyArray<TracePreset>): {
  groups: Array<[string, TracePreset[]]>;
  byCuj: Map<string, TracePreset[]>;
} {
  const groups: Array<[string, TracePreset[]]> = [];
  const byCuj = new Map<string, TracePreset[]>();
  for (const t of presets) {
    const cuj = t.category || 'Other';
    let arr = byCuj.get(cuj);
    if (arr === undefined) {
      arr = [];
      byCuj.set(cuj, arr);
      groups.push([cuj, arr]);
    }
    arr.push(t);
  }
  return {groups, byCuj};
}

// Flat segmented selector for CUJ groups. Stateless — the caller owns the
// active key and updates it from onSelect. Renders nothing for a single group.
export function renderCujSelector(
  cujs: ReadonlyArray<string>,
  active: string,
  onSelect: (cuj: string) => void,
): m.Children {
  if (cujs.length <= 1) return null;
  return m(
    '.pf-bt-cuj-selector',
    cujs.map((cuj) =>
      m(
        'button.pf-bt-cuj-selector__item',
        {
          className: classNames(
            cuj === active && 'pf-bt-cuj-selector__item--active',
          ),
          onclick: () => onSelect(cuj),
        },
        cuj,
      ),
    ),
  );
}

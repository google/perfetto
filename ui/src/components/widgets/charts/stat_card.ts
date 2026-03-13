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
import {Spinner} from '../../../widgets/spinner';
import {type StatCardData} from './stat_card_loader';

export interface StatCardAttrs {
  /** The aggregated data to display, or undefined if loading. */
  readonly data: StatCardData | undefined;
  /** Whether the data is still being fetched. */
  readonly isPending: boolean;
  /** Label to display below the value (e.g. "Count" or "AVG(dur)"). */
  readonly label: string;
}

/**
 * A stat card widget that displays a single aggregated value with a label.
 */
export class StatCard implements m.ClassComponent<StatCardAttrs> {
  view({attrs}: m.CVnode<StatCardAttrs>) {
    const {data, isPending, label} = attrs;

    return m('.pf-stat-card', [
      m(
        '.pf-stat-card__value',
        isPending && data === undefined
          ? m(Spinner)
          : formatStatValue(data?.value),
      ),
      m('.pf-stat-card__label', label),
    ]);
  }
}

function formatStatValue(value: number | undefined): string {
  if (value === undefined) return '\u2014'; // em dash
  if (Number.isInteger(value)) return value.toLocaleString();
  return value.toLocaleString(undefined, {
    minimumFractionDigits: 0,
    maximumFractionDigits: 2,
  });
}

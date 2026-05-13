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
import {classNames} from '../../../base/classnames';
import {Spinner} from '../../../widgets/spinner';

export interface ScorecardAttrs {
  /** Metric name displayed above the value. */
  readonly label: string;
  /**
   * Primary display value. Accepts a pre-formatted string (e.g. "4.2s"),
   * a raw number (formatted via `formatValue` or default formatter),
   * or undefined (shows em-dash placeholder).
   */
  readonly value: string | number | undefined;
  /** Whether the data is still being fetched. */
  readonly isPending?: boolean;
  /** Custom formatter applied to the raw numeric value for display. */
  readonly formatValue?: (value: number) => string;
  /** Fill the parent container width/height. Defaults to false. */
  readonly fillParent?: boolean;
}

/**
 * A scorecard/KPI widget that displays a single metric prominently.
 *
 * Layout:
 * ```
 * ┌─────────────────┐
 * │  Label           │  <- metric name
 * │     4.2s         │  <- big primary value
 * └─────────────────┘
 * ```
 *
 * This is a pure Mithril/HTML/CSS component (not ECharts-based).
 */
export class Scorecard implements m.ClassComponent<ScorecardAttrs> {
  view({attrs}: m.CVnode<ScorecardAttrs>) {
    const {label, value, isPending, formatValue, fillParent} = attrs;

    const isLoading = isPending && value === undefined;

    return m(
      '.pf-scorecard',
      {className: classNames(fillParent && 'pf-scorecard--fill')},
      [
        m('.pf-scorecard__label', label),
        isLoading
          ? m('.pf-scorecard__loading', m(Spinner))
          : m('.pf-scorecard__value', formatDisplayValue(value, formatValue)),
      ],
    );
  }
}

export function formatDisplayValue(
  value: string | number | undefined,
  formatValue?: (v: number) => string,
): string {
  if (value === undefined) return '\u2014'; // em-dash
  if (typeof value === 'string') return value;
  if (formatValue !== undefined) return formatValue(value);
  return defaultFormatNumber(value);
}

function defaultFormatNumber(value: number): string {
  if (Number.isInteger(value)) return value.toLocaleString();
  return value.toLocaleString(undefined, {
    minimumFractionDigits: 0,
    maximumFractionDigits: 2,
  });
}

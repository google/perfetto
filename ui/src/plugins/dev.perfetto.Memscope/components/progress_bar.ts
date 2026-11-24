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

import './progress_bar.scss';
import m from 'mithril';
import {clamp} from '../../../base/math_utils';

// ProgressBar — a horizontal "X of Y used" meter: a filled track (colored fill
// on a grey track) with a percentage, plus an optional left label and a
// right-aligned value/suffix (e.g. " / 640 MB"). Use it to show a bounded
// ratio at a glance — memory or swap utilisation, cache fill, quota consumed,
// or a row's share of a whole in a table. Without a label it renders as a
// compact inline bar (track + rounded %) suitable for table cells. The pct is
// clamped to 0..100 so out-of-range inputs can't overflow the track.

export interface ProgressBarAttrs {
  // Percentage 0..100. Values outside the range are clamped.
  readonly pct: number;
  // Optional label shown to the left of the track. When omitted the bar
  // renders in its compact inline form.
  readonly label?: m.Children;
  // Optional value shown to the right. If omitted, "<pct>%" is shown.
  readonly value?: m.Children;
  // Optional muted suffix appended after the value (e.g. " / 640 MB").
  readonly suffix?: m.Children;
  // Optional fill colour. Defaults to the accent colour.
  readonly color?: string;
}

export class ProgressBar implements m.ClassComponent<ProgressBarAttrs> {
  view({attrs}: m.Vnode<ProgressBarAttrs>) {
    const clamped = clamp(attrs.pct, 0, 100);
    const compact = attrs.label === undefined;
    const value =
      attrs.value ??
      (compact ? `${Math.round(clamped)}%` : `${clamped.toFixed(1)}%`);
    const fillStyle =
      attrs.color !== undefined
        ? {width: `${clamped}%`, background: attrs.color}
        : {width: `${clamped}%`};
    return m(
      compact ? '.pf-progress.pf-progress--compact' : '.pf-progress',
      attrs.label !== undefined && m('.pf-progress__label', attrs.label),
      m('.pf-progress__track', m('.pf-progress__fill', {style: fillStyle})),
      m(
        '.pf-progress__value',
        value,
        attrs.suffix !== undefined &&
          m('span.pf-progress__suffix', attrs.suffix),
      ),
    );
  }
}

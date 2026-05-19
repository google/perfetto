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

export interface ProgressBarAttrs {
  // Percentage 0..100. Values outside the range are clamped.
  readonly pct: number;
  // Optional label shown to the left of the track.
  readonly label?: m.Children;
  // Optional value shown to the right. If omitted, "<pct>%" is shown.
  readonly value?: m.Children;
  // Optional muted suffix appended after the value (e.g. " / 640 MB").
  readonly suffix?: m.Children;
}

export class ProgressBar implements m.ClassComponent<ProgressBarAttrs> {
  view({attrs}: m.Vnode<ProgressBarAttrs>) {
    const clamped = clamp(attrs.pct, 0, 100);
    const value = attrs.value ?? `${clamped.toFixed(1)}%`;
    return m(
      '.pf-progress',
      attrs.label !== undefined && m('.pf-progress__label', attrs.label),
      m(
        '.pf-progress__track',
        m('.pf-progress__fill', {style: {width: `${clamped}%`}}),
      ),
      m(
        '.pf-progress__value',
        value,
        attrs.suffix !== undefined &&
          m('span.pf-progress__suffix', attrs.suffix),
      ),
    );
  }
}

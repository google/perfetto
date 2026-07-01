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

import './share_bar.scss';
import m from 'mithril';

export interface ShareBarAttrs {
  // The fraction of the whole, 0..1 (clamped).
  readonly frac: number;
}

// A tiny inline "share %" bar — a filled track followed by its percentage —
// used in table columns to show a row's fraction of the whole at a glance.
export class ShareBar implements m.ClassComponent<ShareBarAttrs> {
  view({attrs}: m.Vnode<ShareBarAttrs>): m.Children {
    const pct = Math.max(0, Math.min(100, attrs.frac * 100));
    return m('.pf-memscope-sharebar', [
      m(
        '.pf-memscope-sharebar__track',
        m('.pf-memscope-sharebar__fill', {style: {width: `${pct}%`}}),
      ),
      m('span.pf-memscope-sharebar__pct', `${Math.round(pct)}%`),
    ]);
  }
}

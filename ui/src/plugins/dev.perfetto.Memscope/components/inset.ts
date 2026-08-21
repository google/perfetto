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

import './inset.scss';
import m from 'mithril';

// A recessed well meant to sit inside a Panel (or other surface). Where a
// Panel reads as a raised card against the page, an Inset reads as carved into
// it — use it to group secondary content (ratios, nested billboards, callouts)
// within a panel so it doesn't disappear surface-on-surface.
export interface InsetAttrs {
  readonly className?: string;
}

export class Inset implements m.ClassComponent<InsetAttrs> {
  view({attrs, children}: m.CVnode<InsetAttrs>): m.Children {
    return m('.pf-memscope-inset', {className: attrs.className}, children);
  }
}

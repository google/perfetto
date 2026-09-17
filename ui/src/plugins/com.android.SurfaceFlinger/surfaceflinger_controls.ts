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

// The Surface-view controls (shading / only-visible / spacing / rotation),
// shared by the full-screen page and the timeline details panel. Both bind to
// the same SfViewOptions on the session, so the two views stay in sync.

import m from 'mithril';
import {Checkbox} from '../../widgets/checkbox';
import {Select} from '../../widgets/select';
import type {SfRectsOptions} from './surfaceflinger_rects';
import type {SfViewOptions} from './surfaceflinger_session';

export function rectsOptionsFrom(o: SfViewOptions): SfRectsOptions {
  return {
    showOnlyVisible: o.rectsOnlyVisible,
    explode: o.explode,
    rotation: o.rotation,
    shading: o.shading,
  };
}

export function renderSurfaceControls(o: SfViewOptions): m.Children {
  return m('.pf-sf-toolbar', [
    m(
      Select,
      {
        title: 'Shading mode',
        onchange: (e: Event) =>
          (o.shading = (e.target as HTMLSelectElement).value as
            | 'gradient'
            | 'opacity'
            | 'wireframe'),
      },
      ['gradient', 'opacity', 'wireframe'].map((v) =>
        m('option', {value: v, selected: o.shading === v}, v),
      ),
    ),
    m(Checkbox, {
      label: 'Only visible',
      checked: o.rectsOnlyVisible,
      onchange: () => (o.rectsOnlyVisible = !o.rectsOnlyVisible),
    }),
    m('label.pf-sf-slider', [
      'Spacing',
      m('input[type=range]', {
        min: 0,
        max: 100,
        title: 'Z separation between layers (visible when rotated)',
        value: o.explode * 100,
        oninput: (e: Event) =>
          (o.explode = Number((e.target as HTMLInputElement).value) / 100),
      }),
    ]),
    m('label.pf-sf-slider', [
      'Rotation',
      m('input[type=range]', {
        min: 0,
        max: 100,
        title: 'Rotate the 3D layer stack',
        value: o.rotation * 100,
        oninput: (e: Event) =>
          (o.rotation = Number((e.target as HTMLInputElement).value) / 100),
      }),
    ]),
  ]);
}

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
import {Checkbox} from '../../widgets/checkbox';
import {Select} from '../../widgets/select';
import type {UiHierarchyViewOptions} from './ui_hierarchy_session';

export interface UiHierarchyControlsFlags {
  readonly screenLevel: boolean;
  readonly canShowScreenContext: boolean;
  readonly canShowInput: boolean;
}

export function renderUiHierarchyControls(
  o: UiHierarchyViewOptions,
  flags: UiHierarchyControlsFlags,
): m.Children {
  return m('.pf-uih-toolbar', [
    m(Checkbox, {
      label: '3D Stack',
      checked: o.projection3d,
      onchange: () => (o.projection3d = !o.projection3d),
    }),
    m(Checkbox, {
      label: 'Only visible',
      checked: o.onlyVisible,
      onchange: () => (o.onlyVisible = !o.onlyVisible),
    }),
    !flags.screenLevel &&
      m(Checkbox, {
        label: 'Only clickable',
        checked: o.onlyClickable,
        onchange: () => (o.onlyClickable = !o.onlyClickable),
      }),
    flags.canShowScreenContext &&
      m(Checkbox, {
        label: 'Screen context',
        title: 'Outline the other on-screen windows',
        checked: o.showScreenContext,
        onchange: () => (o.showScreenContext = !o.showScreenContext),
      }),
    flags.canShowInput &&
      m(Checkbox, {
        label: 'Input',
        title: 'Draw the input windows and shade where they take touches',
        checked: o.showInput,
        onchange: () => (o.showInput = !o.showInput),
      }),
    m(
      Select,
      {
        title: 'Shading mode',
        onchange: (e: Event) =>
          (o.shading = (e.target as HTMLSelectElement).value as
            'solid' | 'gradient' | 'wireframe'),
      },
      ['solid', 'gradient', 'wireframe'].map((v) =>
        m('option', {value: v, selected: o.shading === v}, v),
      ),
    ),
    o.projection3d &&
      m('label.pf-uih-slider', [
        'Spacing',
        m('input[type=range]', {
          min: 0,
          max: 100,
          title: 'Z separation between nodes in 3D stack',
          value: o.explode * 100,
          oninput: (e: Event) =>
            (o.explode = Number((e.target as HTMLInputElement).value) / 100),
        }),
      ]),
    o.projection3d &&
      m('label.pf-uih-slider', [
        'Rotation',
        m('input[type=range]', {
          min: 0,
          max: 100,
          title: 'Rotate the 3D node stack',
          value: o.rotation * 100,
          oninput: (e: Event) =>
            (o.rotation = Number((e.target as HTMLInputElement).value) / 100),
        }),
      ]),
  ]);
}

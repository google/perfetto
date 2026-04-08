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
import {Button, ButtonGroup, ButtonVariant} from '../../button';

export const ZOOM_LEVELS: readonly number[] = [
  0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 3,
];

export interface ToolbarAttrs {
  readonly zoom: number;
  readonly onZoom: (level: number) => void;
  readonly onFit: () => void;
  readonly extraItems?: m.Children;
}

export const NGToolbar: m.Component<ToolbarAttrs> = {
  view({attrs: {zoom, onZoom, onFit, extraItems}}) {
    const zoomIn =
      ZOOM_LEVELS.find((l) => l > zoom + 0.01) ??
      ZOOM_LEVELS[ZOOM_LEVELS.length - 1];
    const zoomOut =
      [...ZOOM_LEVELS].reverse().find((l) => l < zoom - 0.01) ?? ZOOM_LEVELS[0];

    return m(
      '.pf-ng__toolbar',
      {
        onpointerdown: (e: PointerEvent) => {
          e.stopPropagation(); // Prevent toolbar clicks from starting a canvas drag
        },
      },
      [
        extraItems,
        m(
          ButtonGroup,
          m(Button, {
            title: 'Fit to screen',
            icon: 'center_focus_strong',
            variant: ButtonVariant.Filled,
            onclick: onFit,
          }),
          m(Button, {
            label: `${Math.round(zoom * 100)}%`,
            title: 'Select zoom level',
            variant: ButtonVariant.Filled,
            onclick: () => onZoom(1),
          }),
          m(Button, {
            title: 'Zoom in',
            icon: 'zoom_in',
            variant: ButtonVariant.Filled,
            onclick: () => onZoom(zoomIn),
          }),
          m(Button, {
            title: 'Zoom out',
            icon: 'zoom_out',
            variant: ButtonVariant.Filled,
            onclick: () => onZoom(zoomOut),
          }),
        ),
      ],
    );
  },
};

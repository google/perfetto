// Copyright (C) 2025 The Android Open Source Project
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

export interface Port {
  x: number;
  y: number;
}

export interface ArrowAttrs {
  from: Port;
  to: Port;
}

export const Arrow: m.Component<ArrowAttrs> = {
  view({attrs}) {
    const {from, to} = attrs;
    return m(
      'svg.pf-node-graph-arrow',
      {
        style: {
          position: 'absolute',
          left: 0,
          top: 0,
          width: '100%',
          height: '100%',
          pointerEvents: 'none',
        },
      },
      m('line', {
        'x1': from.x,
        'y1': from.y,
        'x2': to.x,
        'y2': to.y,
        'stroke': 'var(--pf-color-border)',
        'stroke-width': 2,
      }),
    );
  },
};

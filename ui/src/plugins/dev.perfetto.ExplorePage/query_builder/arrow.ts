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
import {NodeBoxLayout} from './node_box';

export interface ArrowAttrs {
  from: NodeBoxLayout;
  to: NodeBoxLayout;
}

export const Arrow: m.Component<ArrowAttrs> = {
  view({attrs}) {
    const {from, to} = attrs;
    const x1 = from.x + (from.width ?? 0) / 2;
    const y1 = from.y + (from.height ?? 0);
    const x2 = to.x + (to.width ?? 0) / 2;
    const y2 = to.y;
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
        x1,
        y1,
        x2,
        y2,
        'stroke': 'var(--pf-color-border)',
        'stroke-width': 2,
      }),
    );
  },
};

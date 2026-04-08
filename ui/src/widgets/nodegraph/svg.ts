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

const ARROWHEAD_LENGTH = 4;
const SHORTEN_END = 16;

export function arrowheadMarker(markerId: string): m.Vnode {
  return m(
    'marker',
    {
      id: markerId,
      viewBox: `0 0 ${ARROWHEAD_LENGTH} 10`,
      refX: '0',
      refY: '5',
      markerWidth: `${ARROWHEAD_LENGTH}`,
      markerHeight: '10',
      orient: 'auto',
    },
    m('polygon', {
      points: `0 2.5, ${ARROWHEAD_LENGTH} 5, 0 7.5`,
      fill: 'context-stroke',
    }),
  );
}

export type PortDirection = 'top' | 'bottom' | 'left' | 'right';

export function connectionPath(
  from: {x: number; y: number},
  to: {x: number; y: number},
  markerId: string,
  fromDir: PortDirection = 'right',
  toDir: PortDirection = 'left',
  attrs?: Record<string, unknown>,
): m.Vnode {
  const d = createCurve(
    from.x,
    from.y,
    to.x,
    to.y,
    fromDir,
    toDir,
    SHORTEN_END,
  );
  return m('path', {
    d,
    'stroke': 'var(--pf-color-primary)',
    'stroke-width': 2,
    'fill': 'none',
    'stroke-linecap': 'round',
    'marker-end': `url(#${markerId})`,
    ...attrs,
  });
}

export function createCurve(
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  fromPortType?: 'top' | 'bottom' | 'left' | 'right',
  toPortType?: 'top' | 'bottom' | 'left' | 'right',
  shortenEnd = 0,
): string {
  const dx = x2 - x1;
  const dy = y2 - y1;
  const distance = Math.sqrt(dx * dx + dy * dy);

  let cx1: number;
  let cy1: number;
  let cx2: number;
  let cy2: number;

  if (shortenEnd > 0) {
    if (toPortType === 'bottom') {
      y2 += shortenEnd;
    } else if (toPortType === 'top') {
      y2 -= shortenEnd;
    } else if (toPortType === 'left') {
      x2 -= shortenEnd;
    } else if (toPortType === 'right') {
      x2 += shortenEnd;
    }
  }

  // For top/bottom ports, control points extend vertically.
  // For left/right ports, control points extend horizontally.
  if (fromPortType === 'bottom' || fromPortType === 'top') {
    const verticalOffset = Math.max(Math.abs(dy) * 0.5, distance * 0.5);
    cx1 = x1;
    cy1 = fromPortType === 'bottom' ? y1 + verticalOffset : y1 - verticalOffset;
  } else {
    const horizontalOffset = Math.max(Math.abs(dx) * 0.5, distance * 0.5);
    cx1 = x1 + horizontalOffset;
    cy1 = y1;
  }

  if (toPortType === 'bottom' || toPortType === 'top') {
    const verticalOffset = Math.max(Math.abs(dy) * 0.5, distance * 0.5);
    cx2 = x2;
    cy2 = toPortType === 'bottom' ? y2 + verticalOffset : y2 - verticalOffset;
  } else {
    const horizontalOffset = Math.max(Math.abs(dx) * 0.5, distance * 0.5);
    cx2 = x2 - horizontalOffset;
    cy2 = y2;
  }

  return `M ${x1} ${y1} C ${cx1} ${cy1}, ${cx2} ${cy2}, ${x2} ${y2}`;
}

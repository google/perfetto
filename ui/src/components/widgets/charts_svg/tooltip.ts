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

import {classNames} from '../../../base/classnames';
import {CursorTooltip} from '../../../widgets/cursor_tooltip';
import {chartColorVar} from './common';
import {LineChartSeries} from '../charts/line_chart';
import m from 'mithril';
import {PopupPosition} from '../../../widgets/popup';

export function renderTooltip(
  series: readonly {readonly style: string; readonly series: LineChartSeries}[],
  index: number,
  fmtX: (v: number) => string,
  fmtY: (v: number) => string,
): m.Children {
  // X value comes from the first series with a point at `index`.
  let xValue: number | undefined;
  for (const s of series) {
    if (s.series.points[index] !== undefined) {
      xValue = s.series.points[index].x;
      break;
    }
  }
  return m(
    CursorTooltip,
    {
      className: 'pf-chart-svg__tooltip',
      key: 'tooltip',
      position: PopupPosition.RightStart,
      offset: 20,
    },
    m(
      '.pf-chart-svg__tooltip-content',
      xValue !== undefined
        ? m('.pf-chart-svg__tooltip-header', fmtX(xValue))
        : null,
      series.map((s, i) => {
        const p = s.series.points[index];
        if (p === undefined) return null;
        const color = s.series.color ?? chartColorVar(i);
        const isHovered = s.style === 'emphasis';
        const isMuted = s.style === 'muted';
        return m(
          '.pf-chart-svg__tooltip-row',
          {
            className: classNames(
              isHovered && 'pf-chart-svg__tooltip-row--hovered',
              isMuted && 'pf-chart-svg__tooltip-row--muted',
            ),
          },
          [
            m('.pf-chart-svg__tooltip-swatch', {
              style: {backgroundColor: color},
            }),
            m('.pf-chart-svg__tooltip-name', s.series.name),
            m('.pf-chart-svg__tooltip-value', fmtY(p.y)),
          ],
        );
      }),
    ),
  );
}

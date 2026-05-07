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
import {chartColorVar} from './common';
import {LineChartData} from './line_chart_svg';
import m from 'mithril';

export function renderLegend(
  data: LineChartData,
  fmtY: (v: number) => string,
  hidden: ReadonlySet<string>,
  onToggle: (name: string) => void,
): m.Children {
  return m(
    '.pf-chart-svg__legend',
    {key: 'legend'},
    data.series.map((s, i) => {
      const last =
        s.points.length > 0 ? s.points[s.points.length - 1].y : undefined;
      const color = s.color ?? chartColorVar(i);
      const isHidden = hidden.has(s.name);
      return m(
        '.pf-chart-svg__legend-entry',
        {
          className: classNames(
            isHidden && 'pf-chart-svg__legend-entry--hidden',
          ),
          style: {cursor: 'pointer'},
          onclick: () => onToggle(s.name),
        },
        [
          m('.pf-chart-svg__legend-swatch', {
            style: {backgroundColor: color},
          }),
          m('.pf-chart-svg__legend-name', s.name),
          last !== undefined
            ? m('.pf-chart-svg__legend-value', fmtY(last))
            : null,
        ],
      );
    }),
  );
}

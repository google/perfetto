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
import {classNames} from '../../../base/classnames';
import {chartColorVar} from '../charts_svg/common';
import {ChartLegend} from '../charts_svg/legend';
import './proportion_bar.scss';

// One slice of a proportion bar. `weight` decides the slice width (the bar is
// "pie-like": each slice is its share of the summed weights); the legend lists
// every segment regardless of weight.
export interface ProportionBarSegment {
  readonly label: string;
  // Non-negative magnitude that sets the slice width. Segments with weight 0
  // are dropped from the bar but still shown in the legend.
  readonly weight: number;
  // Slice/swatch colour. Falls back to the theme chart palette by index.
  readonly color?: string;
  // Optional value rendered after the label in the legend (e.g. "+1.2 MiB").
  readonly value?: m.Children;
  // Optional CSS colour for the legend value text.
  readonly valueColor?: string;
}

export interface ProportionBarAttrs {
  readonly segments: readonly ProportionBarSegment[];
  // Show the swatch legend (default true).
  readonly showLegend?: boolean;
  readonly className?: string;
}

// A single horizontal stacked bar — a bar-shaped pie chart — that shows each
// segment as its proportion of the total, with a swatch legend. Styling and
// the legend are shared with the other charts_svg widgets.
export class ProportionBar implements m.ClassComponent<ProportionBarAttrs> {
  view({attrs}: m.Vnode<ProportionBarAttrs>) {
    const {segments} = attrs;
    const total = segments.reduce((sum, s) => sum + Math.max(0, s.weight), 0);
    const showLegend = attrs.showLegend ?? true;

    const legend =
      showLegend &&
      m(
        ChartLegend,
        segments.map((s, i) =>
          m(ChartLegend.Entry, {
            name: s.label,
            value: s.value,
            valueColor: s.valueColor,
            swatch: s.color ?? chartColorVar(i),
          }),
        ),
      );

    return m(
      '.pf-chart-svg',
      {
        className: classNames(
          'pf-chart-svg--proportion',
          `pf-chart-svg--legend-bottom`,
          attrs.className,
        ),
      },
      m(
        '.pf-chart-svg__proportion-bar',
        total > 0 &&
          segments.map(
            (s, i) =>
              s.weight > 0 &&
              m('.pf-chart-svg__proportion-segment', {
                style: {
                  width: `${(s.weight / total) * 100}%`,
                  background: s.color ?? chartColorVar(i),
                },
              }),
          ),
      ),
      legend,
    );
  }
}

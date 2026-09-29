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

import './dual_metric_bar.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import type {HTMLAttrs} from './common';

export interface DualMetricAxisAttrs extends HTMLAttrs {
  readonly topMax: number;
  readonly topUnit: string;
  readonly bottomMax: number;
  readonly bottomUnit: string;
}

const TICKS = [0, 0.25, 0.5, 0.75, 1] as const;

export class DualMetricAxis implements m.ClassComponent<DualMetricAxisAttrs> {
  view({attrs}: m.CVnode<DualMetricAxisAttrs>): m.Children {
    const {topMax, topUnit, bottomMax, bottomUnit, className, ...rest} = attrs;
    return m(
      '.pf-dual-metric-axis',
      {...rest, className: classNames(className)},
      m(
        '.pf-dual-metric-axis__row',
        TICKS.map((f) =>
          m(
            'span.pf-dual-metric-axis__tick',
            {style: {left: `${f * 100}%`}},
            `${Math.round(f * topMax)}${f === 1 ? ` ${topUnit}` : ''}`,
          ),
        ),
      ),
      m(
        '.pf-dual-metric-axis__row',
        TICKS.map((f) =>
          m(
            'span.pf-dual-metric-axis__tick.pf-dual-metric-axis__tick--bottom',
            {style: {left: `${f * 100}%`}},
            `${Math.round(f * bottomMax)}${f === 1 ? ` ${bottomUnit}` : ''}`,
          ),
        ),
      ),
    );
  }
}

export interface DualMetricBarAttrs extends HTMLAttrs {
  // Top metric (e.g. CPU ms in 100 ms bucket)
  readonly topValue: number;
  readonly topMax: number;
  readonly topColor: string;
  readonly topZeroWidth?: boolean;
  // Bottom metric (e.g. total RSS MB, anon RSS MB, swap MB)
  readonly bottomSecondaryValue?: number | null;
  readonly bottomPrimaryValue?: number | null;
  readonly bottomOverflowValue?: number | null;
  readonly bottomMax: number;
}

export class DualMetricBar implements m.ClassComponent<DualMetricBarAttrs> {
  view({attrs}: m.CVnode<DualMetricBarAttrs>): m.Children {
    const {
      topValue,
      topMax,
      topColor,
      topZeroWidth = false,
      bottomSecondaryValue,
      bottomPrimaryValue,
      bottomOverflowValue,
      bottomMax,
      className,
      ...rest
    } = attrs;

    const topPct = topZeroWidth
      ? 0
      : Math.min(100, Math.max(0, (topValue / Math.max(1, topMax)) * 100));
    const isOver = !topZeroWidth && topValue > topMax;

    const secVal = bottomSecondaryValue ?? 0;
    const priVal = bottomPrimaryValue ?? 0;
    const ovfVal = bottomOverflowValue ?? 0;
    const safeBottomMax = Math.max(1, bottomMax);

    const secPct =
      secVal > 0 ? Math.min(100, (secVal / safeBottomMax) * 100) : 0;
    const priPct =
      priVal > 0 ? Math.min(secPct, (priVal / safeBottomMax) * 100) : 0;
    const ovfPct =
      ovfVal > 0 ? Math.min(100 - secPct, (ovfVal / safeBottomMax) * 100) : 0;

    return m(
      '.pf-dual-metric-bar',
      {...rest, className: classNames(className)},
      m('.pf-dual-metric-bar__fill', {
        className: classNames(isOver && 'pf-dual-metric-bar__fill--over'),
        style: {
          width: `${topPct}%`,
          background: topColor,
        },
      }),
      m('.pf-dual-metric-bar__secondary', {
        style: {width: `${secPct}%`},
      }),
      m('.pf-dual-metric-bar__primary', {
        style: {width: `${priPct}%`},
      }),
      m('.pf-dual-metric-bar__overflow', {
        style: {
          left: `${secPct}%`,
          width: `${ovfPct}%`,
        },
      }),
    );
  }
}

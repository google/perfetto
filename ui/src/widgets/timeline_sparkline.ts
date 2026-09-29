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

import './timeline_sparkline.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import type {HTMLAttrs} from './common';

export interface SparklineBandSeries {
  readonly key: string;
  readonly color: string;
  readonly values: ReadonlyArray<number>;
}

export interface TimelineSparklineAttrs extends HTMLAttrs {
  readonly numFrames: number;
  readonly currentFrame: number;
  readonly originFrame?: number;
  readonly bands: ReadonlyArray<SparklineBandSeries>;
  readonly lineValues?: ReadonlyArray<number>;
  readonly onSelectFrame?: (frame: number) => void;
}

const VIEW_WIDTH = 1000;
const VIEW_HEIGHT = 38;

export class TimelineSparkline implements m.ClassComponent<TimelineSparklineAttrs> {
  private isDragging = false;

  view({attrs}: m.CVnode<TimelineSparklineAttrs>): m.Children {
    const {
      numFrames,
      currentFrame,
      originFrame,
      bands,
      lineValues,
      onSelectFrame,
      className,
      ...rest
    } = attrs;

    const n = Math.max(1, numFrames);
    const denom = Math.max(1, n - 1);

    let maxStack = 1;
    for (let i = 0; i < n; i++) {
      let sum = 0;
      for (const band of bands) {
        sum += band.values[i] ?? 0;
      }
      if (sum > maxStack) {
        maxStack = sum;
      }
    }

    const base = new Array<number>(n).fill(0);
    const bandPaths: Array<{key: string; d: string; color: string}> = [];

    for (const band of bands) {
      const top = base.map((b, i) => b + (band.values[i] ?? 0));
      let d = `M0,${VIEW_HEIGHT}`;
      for (let i = 0; i < n; i++) {
        const x = (i / denom) * VIEW_WIDTH;
        const y = VIEW_HEIGHT - (top[i] / maxStack) * VIEW_HEIGHT;
        d += `L${x.toFixed(1)},${y.toFixed(1)}`;
      }
      for (let i = n - 1; i >= 0; i--) {
        const x = (i / denom) * VIEW_WIDTH;
        const y = VIEW_HEIGHT - (base[i] / maxStack) * VIEW_HEIGHT;
        d += `L${x.toFixed(1)},${y.toFixed(1)}`;
      }
      d += 'Z';
      bandPaths.push({key: band.key, d, color: band.color});
      for (let i = 0; i < n; i++) {
        base[i] = top[i];
      }
    }

    let linePath = '';
    if (lineValues !== undefined && lineValues.length > 0) {
      let maxLine = 1;
      for (const v of lineValues) {
        if (v > maxLine) {
          maxLine = v;
        }
      }
      for (let i = 0; i < n; i++) {
        const v = lineValues[i] ?? 0;
        const x = (i / denom) * VIEW_WIDTH;
        const y = VIEW_HEIGHT - 2 - (v / maxLine) * (VIEW_HEIGHT - 4);
        linePath += `${i === 0 ? 'M' : 'L'}${x.toFixed(1)},${y.toFixed(1)}`;
      }
    }

    const cursorX =
      (Math.max(0, Math.min(n - 1, currentFrame)) / denom) * VIEW_WIDTH;
    const originX =
      originFrame !== undefined
        ? (Math.max(0, Math.min(n - 1, originFrame)) / denom) * VIEW_WIDTH
        : undefined;

    const pickFrame = (e: MouseEvent, el: Element) => {
      if (onSelectFrame === undefined) return;
      const rect = el.getBoundingClientRect();
      if (rect.width <= 0) return;
      const frac = Math.max(
        0,
        Math.min(1, (e.clientX - rect.left) / rect.width),
      );
      onSelectFrame(Math.round(frac * denom));
    };

    return m(
      '.pf-timeline-sparkline',
      {
        ...rest,
        className: classNames(className),
        onmousedown: (e: MouseEvent) => {
          this.isDragging = true;
          pickFrame(e, e.currentTarget as Element);
        },
        onmousemove: (e: MouseEvent) => {
          if (!this.isDragging) return;
          if ((e.buttons & 1) === 0) {
            this.isDragging = false;
            return;
          }
          pickFrame(e, e.currentTarget as Element);
        },
        onmouseup: () => {
          this.isDragging = false;
        },
        onmouseleave: () => {
          this.isDragging = false;
        },
      },
      m(
        'svg',
        {
          viewBox: `0 0 ${VIEW_WIDTH} ${VIEW_HEIGHT}`,
          preserveAspectRatio: 'none',
        },
        bandPaths.map((bp) =>
          m('path', {
            key: bp.key,
            d: bp.d,
            fill: bp.color,
            opacity: '0.8',
          }),
        ),
        linePath !== '' &&
          m('path.pf-timeline-sparkline__line', {
            'd': linePath,
            'fill': 'none',
            'stroke-width': '1.5',
            'vector-effect': 'non-scaling-stroke',
          }),
        originX !== undefined &&
          m('line.pf-timeline-sparkline__origin', {
            'x1': originX.toFixed(1),
            'x2': originX.toFixed(1),
            'y1': '0',
            'y2': `${VIEW_HEIGHT}`,
            'stroke-dasharray': '3 3',
            'stroke-width': '1',
            'vector-effect': 'non-scaling-stroke',
          }),
        m('line.pf-timeline-sparkline__cursor', {
          'x1': cursorX.toFixed(1),
          'x2': cursorX.toFixed(1),
          'y1': '0',
          'y2': `${VIEW_HEIGHT}`,
          'stroke-width': '2',
          'vector-effect': 'non-scaling-stroke',
        }),
      ),
    );
  }
}

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
import {ChartType} from '../nodes/visualisation_node';
import {CHART_TYPES, ChartTypeDefinition} from '../nodes/chart_type_registry';

// SVG preview renderers for each chart type. Each returns a small schematic
// SVG that gives users a visual sense of what the chart looks like.
const CHART_PREVIEW_RENDERERS: Record<ChartType, () => m.Children> = {
  bar: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'currentColor'},
      m('rect', {x: 6, y: 28, width: 10, height: 16, rx: 1, opacity: 0.5}),
      m('rect', {x: 20, y: 12, width: 10, height: 32, rx: 1, opacity: 0.7}),
      m('rect', {x: 34, y: 20, width: 10, height: 24, rx: 1, opacity: 0.85}),
      m('rect', {x: 48, y: 6, width: 10, height: 38, rx: 1}),
    ),

  histogram: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'currentColor'},
      m('rect', {x: 4, y: 30, width: 8, height: 14, opacity: 0.4}),
      m('rect', {x: 12, y: 22, width: 8, height: 22, opacity: 0.55}),
      m('rect', {x: 20, y: 10, width: 8, height: 34, opacity: 0.75}),
      m('rect', {x: 28, y: 6, width: 8, height: 38, opacity: 0.9}),
      m('rect', {x: 36, y: 14, width: 8, height: 30, opacity: 0.7}),
      m('rect', {x: 44, y: 26, width: 8, height: 18, opacity: 0.5}),
      m('rect', {x: 52, y: 34, width: 8, height: 10, opacity: 0.35}),
    ),

  line: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'none', stroke: 'currentColor'},
      m('polyline', {
        'points': '6,38 16,28 26,32 36,16 46,20 58,8',
        'stroke-width': '2.5',
        'stroke-linecap': 'round',
        'stroke-linejoin': 'round',
      }),
      m('circle', {
        cx: 6,
        cy: 38,
        r: 2.5,
        fill: 'currentColor',
        stroke: 'none',
      }),
      m('circle', {
        cx: 26,
        cy: 32,
        r: 2.5,
        fill: 'currentColor',
        stroke: 'none',
      }),
      m('circle', {
        cx: 46,
        cy: 20,
        r: 2.5,
        fill: 'currentColor',
        stroke: 'none',
      }),
      m('circle', {
        cx: 58,
        cy: 8,
        r: 2.5,
        fill: 'currentColor',
        stroke: 'none',
      }),
    ),

  scatter: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'currentColor'},
      m('circle', {cx: 10, cy: 34, r: 3.5, opacity: 0.7}),
      m('circle', {cx: 18, cy: 26, r: 2.5, opacity: 0.6}),
      m('circle', {cx: 28, cy: 18, r: 4, opacity: 0.8}),
      m('circle', {cx: 36, cy: 30, r: 3, opacity: 0.65}),
      m('circle', {cx: 44, cy: 12, r: 3.5, opacity: 0.75}),
      m('circle', {cx: 52, cy: 22, r: 2.5, opacity: 0.85}),
      m('circle', {cx: 22, cy: 38, r: 2, opacity: 0.5}),
      m('circle', {cx: 48, cy: 36, r: 2, opacity: 0.55}),
    ),

  pie: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      // Larger slice (~60%)
      m('path', {
        d: 'M32,24 L32,8 A16,16 0 1,1 18.1,36.1 Z',
        fill: 'currentColor',
        opacity: 0.8,
      }),
      // Smaller slice (~40%)
      m('path', {
        d: 'M32,24 L18.1,36.1 A16,16 0 0,1 32,8 Z',
        fill: 'currentColor',
        opacity: 0.45,
      }),
    ),

  treemap: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'currentColor'},
      m('rect', {x: 4, y: 4, width: 34, height: 24, rx: 2, opacity: 0.8}),
      m('rect', {x: 40, y: 4, width: 20, height: 24, rx: 2, opacity: 0.55}),
      m('rect', {x: 4, y: 30, width: 20, height: 14, rx: 2, opacity: 0.65}),
      m('rect', {x: 26, y: 30, width: 16, height: 14, rx: 2, opacity: 0.4}),
      m('rect', {x: 44, y: 30, width: 16, height: 14, rx: 2, opacity: 0.5}),
    ),

  boxplot: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', stroke: 'currentColor', fill: 'currentColor'},
      // Whisker lines
      m('line', {'x1': 20, 'y1': 6, 'x2': 20, 'y2': 14, 'stroke-width': 1.5}),
      m('line', {'x1': 20, 'y1': 34, 'x2': 20, 'y2': 42, 'stroke-width': 1.5}),
      // Caps
      m('line', {'x1': 15, 'y1': 6, 'x2': 25, 'y2': 6, 'stroke-width': 1.5}),
      m('line', {'x1': 15, 'y1': 42, 'x2': 25, 'y2': 42, 'stroke-width': 1.5}),
      // Box
      m('rect', {
        'x': 13,
        'y': 14,
        'width': 14,
        'height': 20,
        'rx': 1,
        'fill': 'none',
        'stroke-width': 1.5,
      }),
      // Median line
      m('line', {'x1': 13, 'y1': 22, 'x2': 27, 'y2': 22, 'stroke-width': 2}),
      // Second boxplot (shorter)
      m('line', {'x1': 44, 'y1': 12, 'x2': 44, 'y2': 18, 'stroke-width': 1.5}),
      m('line', {'x1': 44, 'y1': 36, 'x2': 44, 'y2': 42, 'stroke-width': 1.5}),
      m('line', {'x1': 39, 'y1': 12, 'x2': 49, 'y2': 12, 'stroke-width': 1.5}),
      m('line', {'x1': 39, 'y1': 42, 'x2': 49, 'y2': 42, 'stroke-width': 1.5}),
      m('rect', {
        'x': 37,
        'y': 18,
        'width': 14,
        'height': 18,
        'rx': 1,
        'fill': 'none',
        'stroke-width': 1.5,
      }),
      m('line', {'x1': 37, 'y1': 28, 'x2': 51, 'y2': 28, 'stroke-width': 2}),
    ),

  heatmap: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'currentColor'},
      // 4x3 grid of cells with varying opacity
      m('rect', {x: 4, y: 4, width: 13, height: 12, rx: 1, opacity: 0.9}),
      m('rect', {x: 19, y: 4, width: 13, height: 12, rx: 1, opacity: 0.4}),
      m('rect', {x: 34, y: 4, width: 13, height: 12, rx: 1, opacity: 0.7}),
      m('rect', {x: 49, y: 4, width: 13, height: 12, rx: 1, opacity: 0.3}),
      m('rect', {x: 4, y: 18, width: 13, height: 12, rx: 1, opacity: 0.5}),
      m('rect', {x: 19, y: 18, width: 13, height: 12, rx: 1, opacity: 0.8}),
      m('rect', {x: 34, y: 18, width: 13, height: 12, rx: 1, opacity: 0.35}),
      m('rect', {x: 49, y: 18, width: 13, height: 12, rx: 1, opacity: 0.65}),
      m('rect', {x: 4, y: 32, width: 13, height: 12, rx: 1, opacity: 0.25}),
      m('rect', {x: 19, y: 32, width: 13, height: 12, rx: 1, opacity: 0.6}),
      m('rect', {x: 34, y: 32, width: 13, height: 12, rx: 1, opacity: 0.85}),
      m('rect', {x: 49, y: 32, width: 13, height: 12, rx: 1, opacity: 0.45}),
    ),

  cdf: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'none', stroke: 'currentColor'},
      m('polyline', {
        'points': '6,42 14,40 22,36 28,28 34,18 40,12 48,9 56,8',
        'stroke-width': '2.5',
        'stroke-linecap': 'round',
        'stroke-linejoin': 'round',
      }),
      // Dashed 50% line
      m('line', {
        'x1': 4,
        'y1': 24,
        'x2': 60,
        'y2': 24,
        'stroke-width': 1,
        'stroke-dasharray': '3,3',
        'opacity': 0.4,
      }),
    ),

  scorecard: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'currentColor'},
      m(
        'text',
        {
          'x': 32,
          'y': 28,
          'text-anchor': 'middle',
          'dominant-baseline': 'central',
          'font-size': '18',
          'font-weight': 'bold',
        },
        '42',
      ),
      m(
        'text',
        {
          'x': 32,
          'y': 41,
          'text-anchor': 'middle',
          'font-size': '7',
          'opacity': 0.5,
        },
        'value',
      ),
    ),
};

/**
 * Renders a grid of chart type cards. Each card fires `onSelect` when clicked
 * and has the `pf-dismiss-popup-group` class so it auto-closes a parent Popup.
 */
export function renderChartTypePickerGrid(
  onSelect: (type: ChartType) => void,
): m.Children {
  return m(
    '.pf-chart-type-picker',
    CHART_TYPES.map((def) => renderChartTypeCard(def, onSelect)),
  );
}

function renderChartTypeCard(
  def: ChartTypeDefinition,
  onSelect: (type: ChartType) => void,
): m.Children {
  const description = def.description;

  return m(
    'button.pf-chart-type-picker__card.pf-dismiss-popup-group',
    {
      key: def.type,
      title: description,
      onclick: () => onSelect(def.type),
    },
    [
      m('.pf-chart-type-picker__preview', CHART_PREVIEW_RENDERERS[def.type]()),
      m('.pf-chart-type-picker__label', def.label),
    ],
  );
}

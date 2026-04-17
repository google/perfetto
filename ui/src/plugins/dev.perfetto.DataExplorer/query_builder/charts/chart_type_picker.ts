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
import {classNames} from '../../../../base/classnames';
import {ChartType} from '../nodes/visualisation_node';
import {CHART_TYPES, ChartTypeDefinition} from '../nodes/chart_type_registry';

// Chart preview palette — references CSS variables from theme_provider.scss
// so thumbnails match the actual chart colours (and adapt to dark mode).
// SVG presentation attributes don't resolve var(), so we apply colours via
// inline style objects.
// Picked for visual contrast; C2/C3 skip indices intentionally.
const C1 = 'var(--pf-chart-color-1)';
const C2 = 'var(--pf-chart-color-3)';
const C3 = 'var(--pf-chart-color-4)';

// Shorthand style helpers — keeps the SVG hyperscript readable.
function fill(
  c: string,
  o?: number,
): {style: {fill: string; opacity?: number}} {
  return o !== undefined ? {style: {fill: c, opacity: o}} : {style: {fill: c}};
}
function stroke(
  c: string,
  o?: number,
): {style: {stroke: string; opacity?: number}} {
  return o !== undefined
    ? {style: {stroke: c, opacity: o}}
    : {style: {stroke: c}};
}

// SVG preview renderers for each chart type. Each returns a small schematic
// SVG that gives users a visual sense of what the chart looks like.
const CHART_PREVIEW_RENDERERS: Record<ChartType, () => m.Children> = {
  bar: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      m('rect', {x: 6, y: 28, width: 10, height: 16, rx: 1, ...fill(C2)}),
      m('rect', {x: 20, y: 12, width: 10, height: 32, rx: 1, ...fill(C1)}),
      m('rect', {x: 34, y: 20, width: 10, height: 24, rx: 1, ...fill(C3)}),
      m('rect', {x: 48, y: 6, width: 10, height: 38, rx: 1, ...fill(C1)}),
    ),

  histogram: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      m('rect', {x: 4, y: 30, width: 8, height: 14, ...fill(C1, 0.45)}),
      m('rect', {x: 12, y: 22, width: 8, height: 22, ...fill(C1, 0.6)}),
      m('rect', {x: 20, y: 10, width: 8, height: 34, ...fill(C1, 0.75)}),
      m('rect', {x: 28, y: 6, width: 8, height: 38, ...fill(C1, 0.9)}),
      m('rect', {x: 36, y: 14, width: 8, height: 30, ...fill(C1, 0.75)}),
      m('rect', {x: 44, y: 26, width: 8, height: 18, ...fill(C1, 0.55)}),
      m('rect', {x: 52, y: 34, width: 8, height: 10, ...fill(C1, 0.4)}),
    ),

  line: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'none'},
      m('polyline', {
        'points': '6,38 16,28 26,32 36,16 46,20 58,8',
        'stroke-width': '2.5',
        'stroke-linecap': 'round',
        'stroke-linejoin': 'round',
        ...stroke(C1),
      }),
      m('polyline', {
        'points': '6,42 16,36 26,38 36,30 46,28 58,22',
        'stroke-width': '2',
        'stroke-linecap': 'round',
        'stroke-linejoin': 'round',
        ...stroke(C2, 0.7),
      }),
    ),

  scatter: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      m('circle', {cx: 10, cy: 34, r: 3.5, ...fill(C1)}),
      m('circle', {cx: 18, cy: 26, r: 2.5, ...fill(C2)}),
      m('circle', {cx: 28, cy: 18, r: 4, ...fill(C3)}),
      m('circle', {cx: 36, cy: 30, r: 3, ...fill(C1)}),
      m('circle', {cx: 44, cy: 12, r: 3.5, ...fill(C2)}),
      m('circle', {cx: 52, cy: 22, r: 2.5, ...fill(C3)}),
      m('circle', {cx: 22, cy: 38, r: 2, ...fill(C2)}),
      m('circle', {cx: 48, cy: 36, r: 2, ...fill(C1)}),
    ),

  pie: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      m('path', {d: 'M32,24 L32,8 A16,16 0 0,1 48,24 Z', ...fill(C1)}),
      m('path', {
        d: 'M32,24 L48,24 A16,16 0 0,1 27.1,38.6 Z',
        ...fill(C2),
      }),
      m('path', {d: 'M32,24 L27.1,38.6 A16,16 0 0,1 32,8 Z', ...fill(C3)}),
    ),

  treemap: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      m('rect', {x: 4, y: 4, width: 34, height: 24, rx: 2, ...fill(C1)}),
      m('rect', {x: 40, y: 4, width: 20, height: 24, rx: 2, ...fill(C2)}),
      m('rect', {x: 4, y: 30, width: 20, height: 14, rx: 2, ...fill(C3)}),
      m('rect', {
        x: 26,
        y: 30,
        width: 16,
        height: 14,
        rx: 2,
        ...fill(C1, 0.55),
      }),
      m('rect', {
        x: 44,
        y: 30,
        width: 16,
        height: 14,
        rx: 2,
        ...fill(C2, 0.6),
      }),
    ),

  boxplot: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      // First boxplot
      m('line', {
        'x1': 20,
        'y1': 6,
        'x2': 20,
        'y2': 14,
        'stroke-width': 1.5,
        ...stroke(C1),
      }),
      m('line', {
        'x1': 20,
        'y1': 34,
        'x2': 20,
        'y2': 42,
        'stroke-width': 1.5,
        ...stroke(C1),
      }),
      m('line', {
        'x1': 15,
        'y1': 6,
        'x2': 25,
        'y2': 6,
        'stroke-width': 1.5,
        ...stroke(C1),
      }),
      m('line', {
        'x1': 15,
        'y1': 42,
        'x2': 25,
        'y2': 42,
        'stroke-width': 1.5,
        ...stroke(C1),
      }),
      m('rect', {
        'x': 13,
        'y': 14,
        'width': 14,
        'height': 20,
        'rx': 1,
        'fill': 'none',
        'stroke-width': 1.5,
        ...stroke(C1),
      }),
      m('line', {
        'x1': 13,
        'y1': 22,
        'x2': 27,
        'y2': 22,
        'stroke-width': 2,
        ...stroke(C1),
      }),
      // Second boxplot
      m('line', {
        'x1': 44,
        'y1': 12,
        'x2': 44,
        'y2': 18,
        'stroke-width': 1.5,
        ...stroke(C2),
      }),
      m('line', {
        'x1': 44,
        'y1': 36,
        'x2': 44,
        'y2': 42,
        'stroke-width': 1.5,
        ...stroke(C2),
      }),
      m('line', {
        'x1': 39,
        'y1': 12,
        'x2': 49,
        'y2': 12,
        'stroke-width': 1.5,
        ...stroke(C2),
      }),
      m('line', {
        'x1': 39,
        'y1': 42,
        'x2': 49,
        'y2': 42,
        'stroke-width': 1.5,
        ...stroke(C2),
      }),
      m('rect', {
        'x': 37,
        'y': 18,
        'width': 14,
        'height': 18,
        'rx': 1,
        'fill': 'none',
        'stroke-width': 1.5,
        ...stroke(C2),
      }),
      m('line', {
        'x1': 37,
        'y1': 28,
        'x2': 51,
        'y2': 28,
        'stroke-width': 2,
        ...stroke(C2),
      }),
    ),

  heatmap: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      m('rect', {x: 4, y: 4, width: 13, height: 12, rx: 1, ...fill(C1)}),
      m('rect', {x: 19, y: 4, width: 13, height: 12, rx: 1, ...fill(C2, 0.5)}),
      m('rect', {x: 34, y: 4, width: 13, height: 12, rx: 1, ...fill(C3)}),
      m('rect', {x: 49, y: 4, width: 13, height: 12, rx: 1, ...fill(C1, 0.35)}),
      m('rect', {x: 4, y: 18, width: 13, height: 12, rx: 1, ...fill(C2, 0.6)}),
      m('rect', {x: 19, y: 18, width: 13, height: 12, rx: 1, ...fill(C1)}),
      m('rect', {x: 34, y: 18, width: 13, height: 12, rx: 1, ...fill(C3, 0.4)}),
      m('rect', {x: 49, y: 18, width: 13, height: 12, rx: 1, ...fill(C2)}),
      m('rect', {x: 4, y: 32, width: 13, height: 12, rx: 1, ...fill(C3, 0.3)}),
      m('rect', {x: 19, y: 32, width: 13, height: 12, rx: 1, ...fill(C1, 0.7)}),
      m('rect', {x: 34, y: 32, width: 13, height: 12, rx: 1, ...fill(C2)}),
      m('rect', {
        x: 49,
        y: 32,
        width: 13,
        height: 12,
        rx: 1,
        ...fill(C3, 0.55),
      }),
    ),

  cdf: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48', fill: 'none'},
      m('polyline', {
        'points': '6,42 14,40 22,36 28,28 34,18 40,12 48,9 56,8',
        'stroke-width': '2.5',
        'stroke-linecap': 'round',
        'stroke-linejoin': 'round',
        ...stroke(C1),
      }),
      m('line', {
        'x1': 4,
        'y1': 24,
        'x2': 60,
        'y2': 24,
        'stroke-width': 1,
        'stroke-dasharray': '3,3',
        ...stroke(C3, 0.5),
      }),
    ),

  scorecard: () =>
    m(
      'svg',
      {viewBox: '0 0 64 48'},
      m(
        'text',
        {
          'x': 32,
          'y': 28,
          'text-anchor': 'middle',
          'dominant-baseline': 'central',
          'font-size': '18',
          'font-weight': 'bold',
          ...fill(C1),
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
          ...fill(C2, 0.7),
        },
        'value',
      ),
    ),
};

/**
 * Renders a grid of chart type cards. Each card fires `onSelect` when clicked.
 * When `selectedType` is provided, the matching card is visually highlighted.
 */
export function renderChartTypePickerGrid(
  onSelect: (type: ChartType) => void,
  selectedType?: ChartType,
): m.Children {
  return m(
    '.pf-chart-type-picker',
    CHART_TYPES.map((def) => renderChartTypeCard(def, onSelect, selectedType)),
  );
}

function renderChartTypeCard(
  def: ChartTypeDefinition,
  onSelect: (type: ChartType) => void,
  selectedType?: ChartType,
): m.Children {
  const description = def.description;
  const isSelected = selectedType === def.type;

  return m(
    'button.pf-chart-type-picker__card',
    {
      key: def.type,
      className: classNames(isSelected && 'pf-selected'),
      title: description,
      onclick: () => onSelect(def.type),
    },
    [
      m('.pf-chart-type-picker__preview', CHART_PREVIEW_RENDERERS[def.type]()),
      m('.pf-chart-type-picker__label', def.label),
    ],
  );
}

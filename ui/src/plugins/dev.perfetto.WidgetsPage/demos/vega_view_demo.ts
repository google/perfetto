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
import {VegaView} from '../../../components/widgets/vega_view';
import {EnumOption, renderWidgetShowcase} from '../widgets_page_utils';

const DATA_ENGLISH_LETTER_FREQUENCY = {
  table: [
    {category: 'a', amount: 8.167},
    {category: 'b', amount: 1.492},
    {category: 'c', amount: 2.782},
    {category: 'd', amount: 4.253},
    {category: 'e', amount: 12.7},
    {category: 'f', amount: 2.228},
    {category: 'g', amount: 2.015},
    {category: 'h', amount: 6.094},
    {category: 'i', amount: 6.966},
    {category: 'j', amount: 0.253},
    {category: 'k', amount: 1.772},
    {category: 'l', amount: 4.025},
    {category: 'm', amount: 2.406},
    {category: 'n', amount: 6.749},
    {category: 'o', amount: 7.507},
    {category: 'p', amount: 1.929},
    {category: 'q', amount: 0.095},
    {category: 'r', amount: 5.987},
    {category: 's', amount: 6.327},
    {category: 't', amount: 9.056},
    {category: 'u', amount: 2.758},
    {category: 'v', amount: 0.978},
    {category: 'w', amount: 2.36},
    {category: 'x', amount: 0.25},
    {category: 'y', amount: 1.974},
    {category: 'z', amount: 0.074},
  ],
};

const DATA_POLISH_LETTER_FREQUENCY = {
  table: [
    {category: 'a', amount: 8.965},
    {category: 'b', amount: 1.482},
    {category: 'c', amount: 3.988},
    {category: 'd', amount: 3.293},
    {category: 'e', amount: 7.921},
    {category: 'f', amount: 0.312},
    {category: 'g', amount: 1.377},
    {category: 'h', amount: 1.072},
    {category: 'i', amount: 8.286},
    {category: 'j', amount: 2.343},
    {category: 'k', amount: 3.411},
    {category: 'l', amount: 2.136},
    {category: 'm', amount: 2.911},
    {category: 'n', amount: 5.6},
    {category: 'o', amount: 7.59},
    {category: 'p', amount: 3.101},
    {category: 'q', amount: 0.003},
    {category: 'r', amount: 4.571},
    {category: 's', amount: 4.263},
    {category: 't', amount: 3.966},
    {category: 'u', amount: 2.347},
    {category: 'v', amount: 0.034},
    {category: 'w', amount: 4.549},
    {category: 'x', amount: 0.019},
    {category: 'y', amount: 3.857},
    {category: 'z', amount: 5.62},
  ],
};

const DATA_EMPTY = {};

const SPEC_BAR_CHART = `
{
  "$schema": "https://vega.github.io/schema/vega/v5.json",
  "description": "A basic bar chart example, with value labels shown upon mouse hover.",
  "width": 400,
  "height": 200,
  "padding": 5,

  "data": [
    {
      "name": "table"
    }
  ],

  "signals": [
    {
      "name": "tooltip",
      "value": {},
      "on": [
        {"events": "rect:mouseover", "update": "datum"},
        {"events": "rect:mouseout",  "update": "{}"}
      ]
    }
  ],

  "scales": [
    {
      "name": "xscale",
      "type": "band",
      "domain": {"data": "table", "field": "category"},
      "range": "width",
      "padding": 0.05,
      "round": true
    },
    {
      "name": "yscale",
      "domain": {"data": "table", "field": "amount"},
      "nice": true,
      "range": "height"
    }
  ],

  "axes": [
    { "orient": "bottom", "scale": "xscale" },
    { "orient": "left", "scale": "yscale" }
  ],

  "marks": [
    {
      "type": "rect",
      "from": {"data":"table"},
      "encode": {
        "enter": {
          "x": {"scale": "xscale", "field": "category"},
          "width": {"scale": "xscale", "band": 1},
          "y": {"scale": "yscale", "field": "amount"},
          "y2": {"scale": "yscale", "value": 0}
        },
        "update": {
          "fill": {"value": "steelblue"}
        },
        "hover": {
          "fill": {"value": "red"}
        }
      }
    },
    {
      "type": "text",
      "encode": {
        "enter": {
          "align": {"value": "center"},
          "baseline": {"value": "bottom"},
          "fill": {"value": "#333"}
        },
        "update": {
          "x": {"scale": "xscale", "signal": "tooltip.category", "band": 0.5},
          "y": {"scale": "yscale", "signal": "tooltip.amount", "offset": -2},
          "text": {"signal": "tooltip.amount"},
          "fillOpacity": [
            {"test": "datum === tooltip", "value": 0},
            {"value": 1}
          ]
        }
      }
    }
  ]
}
`;

const SPEC_BAR_CHART_LITE = `
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "description": "A simple bar chart with embedded data.",
  "data": {
    "name": "table"
  },
  "mark": "bar",
  "encoding": {
    "x": {"field": "category", "type": "nominal", "axis": {"labelAngle": 0}},
    "y": {"field": "amount", "type": "quantitative"}
  }
}
`;

const SPEC_BROKEN = `{
  "description": 123
}
`;

enum SpecExample {
  BarChart = 'Barchart',
  BarChartLite = 'Barchart (Lite)',
  Broken = 'Broken',
}

enum DataExample {
  English = 'English',
  Polish = 'Polish',
  Empty = 'Empty',
}

function getExampleSpec(example: SpecExample): string {
  switch (example) {
    case SpecExample.BarChart:
      return SPEC_BAR_CHART;
    case SpecExample.BarChartLite:
      return SPEC_BAR_CHART_LITE;
    case SpecExample.Broken:
      return SPEC_BROKEN;
    default:
      const exhaustiveCheck: never = example;
      throw new Error(`Unhandled case: ${exhaustiveCheck}`);
  }
}

function getExampleData(example: DataExample) {
  switch (example) {
    case DataExample.English:
      return DATA_ENGLISH_LETTER_FREQUENCY;
    case DataExample.Polish:
      return DATA_POLISH_LETTER_FREQUENCY;
    case DataExample.Empty:
      return DATA_EMPTY;
    default:
      const exhaustiveCheck: never = example;
      throw new Error(`Unhandled case: ${exhaustiveCheck}`);
  }
}

export function renderVegaView(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'VegaView'),
      m(
        'p',
        'A data visualization component powered by Vega-Lite for creating interactive charts and graphs from data.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opt) =>
        m(VegaView, {
          spec: getExampleSpec(opt.exampleSpec),
          data: getExampleData(opt.exampleData),
        }),
      initialOpts: {
        exampleSpec: new EnumOption(
          SpecExample.BarChart,
          Object.values(SpecExample),
        ),
        exampleData: new EnumOption(
          DataExample.English,
          Object.values(DataExample),
        ),
      },
    }),
  ];
}

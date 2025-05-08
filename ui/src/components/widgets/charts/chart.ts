// Copyright (C) 2024 The Android Open Source Project
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
import {Row} from '../../../trace_processor/query_result';
import {Histogram} from './histogram';
import {Item, SignalValue} from 'vega';
import {VegaLiteAggregationOps, VegaLiteFieldType} from '../vega_view';
import {BarChart} from './bar_chart';

export interface ChartAttrs {
  readonly chartType: ChartType;
  readonly description?: string;
  readonly title?: string;
  data?: Row[];
  columns: string[];
  onPointSelection?: (item: Item) => void;
  onIntervalSelection?: (value: SignalValue) => void;
  isLoading?: boolean;
  specProps?: {
    xField?: string;
    xType?: VegaLiteFieldType;
    xAggregationOp?: VegaLiteAggregationOps;
    yField?: string;
    yType?: VegaLiteFieldType;
    yAggregationOp?: VegaLiteAggregationOps;
  };
}

// Holds the various chart types and human readable string
export enum ChartType {
  BAR_CHART = 'bar chart',
  HISTOGRAM = 'histogram',
}

export interface ChartData {
  readonly rows: Row[];
  readonly error?: string;
}

export function toTitleCase(s: string): string {
  const words = s.split(/\s/);

  for (let i = 0; i < words.length; ++i) {
    words[i] = words[i][0].toUpperCase() + words[i].substring(1);
  }

  return words.join(' ');
}

// Takes a chart option and config and map
// to the corresponding chart class component.
export function renderChart(chart: ChartAttrs) {
  switch (chart.chartType) {
    case ChartType.BAR_CHART:
      return m(BarChart, chart);
    case ChartType.HISTOGRAM:
      return m(Histogram, chart);
    default:
      return;
  }
}

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

export enum ChartType {
  Table = 'table',
  Histogram = 'histogram',
  Cdf = 'cdf',
  Scatter = 'scatter',
  Line = 'line',
  Bar = 'bar',
  Boxplot = 'boxplot',
  Heatmap = 'heatmap',
  Donut = 'donut',
  Violin = 'violin',
}

export enum AggregationFunction {
  Sum = 'sum',
  Avg = 'avg',
  Count = 'count',
  Min = 'min',
  Max = 'max',
}

export enum LayoutMode {
  Grouped = 'grouped',
  Stacked = 'stacked',
}

export enum SortDirection {
  Asc = 'asc',
  Desc = 'desc',
}

export enum SortBy {
  X = 'x',
  Y = 'y',
}

export enum FilterOp {
  Eq = '=',
  NotEq = '!=',
  Lt = '<',
  Lte = '<=',
  Gt = '>',
  Gte = '>=',
  In = 'in',
  NotIn = 'not in',
  Glob = 'glob',
}

export type Filter = {
  col: string;
  op: FilterOp;
  val: string | number | boolean | string[] | number[] | null;
};

/**
 * Atomic unit of filters that are added/removed together.
 *
 * Range selections like "duration between 100-500" create a group with two
 * filters (>= and <=) that should stay together.
 */
export type FilterGroup = {
  id: string;
  filters: Filter[];
  label?: string;
};

/**
 * Filter change notification includes source ID so charts can decide
 * whether to update themselves (depends on "update source chart" setting).
 */
export interface FilterNotification {
  filters: Filter[];
  sourceChartId: string;
}

export type Aggregation = {
  fn: AggregationFunction;
  field: string;
  groupBy: string[];
};

export type Row = Record<string, string | number | boolean | null | undefined>;

export type Sort = {
  by: SortBy;
  direction: SortDirection;
};

export type ChartSpec =
  | {
      type: ChartType.Bar;
      x: string;
      y: string;
      aggregation: AggregationFunction;
      groupBy?: string;
      mode?: LayoutMode;
      sort?: Sort;
    }
  | {type: ChartType.Histogram; x: string; bins?: number}
  | {type: ChartType.Cdf; x: string; colorBy?: string}
  | {
      type: ChartType.Scatter;
      x: string;
      y: string;
      colorBy?: string;
      showCorrelation?: boolean;
    }
  | {type: ChartType.Boxplot; x: string; y: string}
  | {
      type: ChartType.Heatmap;
      x: string;
      y: string;
      value: string;
      aggregation: AggregationFunction;
    }
  | {
      type: ChartType.Line;
      x: string;
      y: string;
      aggregation: AggregationFunction;
      colorBy?: string;
      sort?: Sort;
    }
  | {
      type: ChartType.Donut;
      category: string;
      value: string;
      aggregation: AggregationFunction;
    }
  | {type: ChartType.Violin; x: string; y: string};

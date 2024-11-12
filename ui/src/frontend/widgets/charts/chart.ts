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
import {Engine} from '../../../trace_processor/engine';
import {Filter, TableColumn, TableColumnSet} from '../sql/table/column';
import {Histogram} from './histogram/histogram';

export interface VegaLiteChartSpec {
  $schema: string;
  width: string | number;
  mark:
    | 'area'
    | 'bar'
    | 'circle'
    | 'line'
    | 'point'
    | 'rect'
    | 'rule'
    | 'square'
    | 'text'
    | 'tick'
    | 'geoshape'
    | 'boxplot'
    | 'errorband'
    | 'errorbar';
  data: {values?: string | Row[]};

  encoding: {
    x: {[key: string]: unknown};
    y: {[key: string]: unknown};
  };
}

// Holds the various chart types and human readable string
export enum ChartOption {
  HISTOGRAM = 'histogram',
}

export interface ChartConfig {
  readonly engine: Engine;
  readonly columnTitle: string; // Human readable column name (ex: Duration)
  readonly sqlColumn: string[]; // SQL column name (ex: dur)
  readonly filters?: Filter[]; // Filters applied to SQL table
  readonly tableDisplay?: string; // Human readable table name (ex: slices)
  readonly query: string; // SQL query for the underlying data
  readonly aggregationType?: 'nominal' | 'quantitative'; // Aggregation type.
}

export interface ChartData {
  readonly rows: Row[];
  readonly error?: string;
}

export interface ChartState {
  readonly engine: Engine;
  readonly query: string;
  readonly columns: TableColumn[] | TableColumnSet[] | string[];
  data?: ChartData;
  spec?: VegaLiteChartSpec;
  loadData(): Promise<void>;
  isLoading(): boolean;
}

export function toTitleCase(s: string): string {
  const words = s.split(/\s/);

  for (let i = 0; i < words.length; ++i) {
    words[i] = words[i][0].toUpperCase() + words[i].substring(1);
  }

  return words.join(' ');
}

// renderChartComponent will take a chart option and config and map
// to the corresponding chart class component.
export function renderChartComponent(option: ChartOption, config: ChartConfig) {
  switch (option) {
    case ChartOption.HISTOGRAM:
      return m(Histogram, config);
    default:
      return;
  }
}

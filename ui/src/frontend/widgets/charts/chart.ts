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
import {Row} from '../../../trace_processor/query_result';
import {Engine} from '../../../trace_processor/engine';
import {TableColumn, TableColumnSet} from '../sql/table/column';

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

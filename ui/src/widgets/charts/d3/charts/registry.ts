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

import {ChartType} from '../data/types';
import {ChartRenderer} from './base_renderer';
import {BarRenderer} from './bar';
import {HistogramRenderer} from './histogram';
import {CDFRenderer} from './cdf';
import {ScatterRenderer} from './scatter';
import {BoxplotRenderer} from './boxplot';
import {HeatmapRenderer} from './heatmap';
import {LineRenderer} from './line';
import {DonutChartRenderer} from './donut';
import {ViolinRenderer} from './violin';

// Factory functions to create new renderer instances per chart
// This prevents callback collision when multiple charts share the same renderer type
export const RENDERERS: Record<
  Exclude<ChartType, ChartType.Table>,
  () => ChartRenderer
> = {
  [ChartType.Bar]: () => new BarRenderer(),
  [ChartType.Histogram]: () => new HistogramRenderer(),
  [ChartType.Cdf]: () => new CDFRenderer(),
  [ChartType.Scatter]: () => new ScatterRenderer(),
  [ChartType.Boxplot]: () => new BoxplotRenderer(),
  [ChartType.Heatmap]: () => new HeatmapRenderer(),
  [ChartType.Line]: () => new LineRenderer(),
  [ChartType.Donut]: () => new DonutChartRenderer(),
  [ChartType.Violin]: () => new ViolinRenderer(),
};

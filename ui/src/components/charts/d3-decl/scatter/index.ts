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

/**
 * Scatter plot chart component - unified export.
 *
 * Clean, production-ready scatter plot implementation following all
 * architecture principles with zero anti-patterns.
 *
 * Features:
 * - Always controlled (no internal state)
 * - Complete filter array paradigm
 * - BrushHandler2D for interactions (no race conditions)
 * - 2D rectangular brush selection
 * - Category-based coloring
 * - Optional correlation line and statistics
 * - Hover tooltips with point details
 * - Click empty space to clear, drag to brush filter
 *
 * @example
 * ```typescript
 * import {ScatterPlot} from './scatter';
 *
 * class Dashboard {
 *   private filters: Filter[] = [];
 *
 *   view() {
 *     return m(ScatterPlot, {
 *       data: scatterLoader.use({filters: this.filters}),
 *       filters: this.filters,
 *       xColumn: 'duration',
 *       yColumn: 'cpu_time',
 *       onFiltersChanged: (filters) => { this.filters = filters; },
 *       showCorrelation: true,
 *     });
 *   }
 * }
 * ```
 */

// Component export
export {ScatterPlot} from './scatter';

// Type exports
export type {
  ScatterPoint,
  CorrelationStats,
  ScatterData,
  ScatterPlotAttrs,
} from './scatter';

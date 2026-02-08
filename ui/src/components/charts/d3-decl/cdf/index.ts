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
 * CDF (Cumulative Distribution Function) chart component - unified export.
 *
 * Clean, production-ready CDF implementation following all
 * architecture principles with zero anti-patterns.
 *
 * Features:
 * - Always controlled (no internal state)
 * - Complete filter array paradigm
 * - BrushHandler1D for interactions (no race conditions)
 * - Multi-line support for distribution comparison
 * - Crosshair with probability tooltips
 * - Optional percentile markers (P50, P90, P95, P99)
 * - Click empty space to clear, drag to brush filter
 *
 * @example
 * ```typescript
 * import {CDFChart} from './cdf';
 *
 * class Dashboard {
 *   private filters: Filter[] = [];
 *
 *   view() {
 *     return m(CDFChart, {
 *       data: cdfLoader.use({filters: this.filters}),
 *       filters: this.filters,
 *       column: 'duration',
 *       onFiltersChanged: (filters) => { this.filters = filters; },
 *       showPercentiles: true,
 *     });
 *   }
 * }
 * ```
 */

// Component export
export {CDFChart} from './cdf';

// Type exports
export type {CDFPoint, CDFLine, CDFData, CDFChartAttrs} from './cdf';

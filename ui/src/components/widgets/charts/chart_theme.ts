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
 * Chart theme utilities for reading Perfetto theme colors from CSS variables.
 */

/**
 * Theme colors for charts and visualizations.
 */
export interface ChartThemeColors {
  readonly textColor: string;
  readonly borderColor: string;
  readonly backgroundColor: string;
  readonly accentColor: string;
  readonly chartColors: readonly string[];
}

/**
 * Returns the current theme colors by reading CSS variables from the given element.
 */
export function getChartThemeColors(element: Element): ChartThemeColors {
  const style = getComputedStyle(element);

  const chartColors: string[] = [];
  for (let i = 1; i <= 8; i++) {
    chartColors.push(style.getPropertyValue(`--pf-chart-color-${i}`).trim());
  }

  return {
    textColor: style.getPropertyValue('--pf-color-text').trim(),
    borderColor: style.getPropertyValue('--pf-color-border').trim(),
    backgroundColor: style.getPropertyValue('--pf-color-background').trim(),
    accentColor: style.getPropertyValue('--pf-color-accent').trim(),
    chartColors,
  };
}

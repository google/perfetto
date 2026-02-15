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
 * Chart theme utilities for reading Perfetto theme colors.
 *
 * DESIGN NOTES:
 *
 * 1. Colors are read from CSS variables defined in theme_provider.scss, NOT
 *    hardcoded. This ensures charts automatically adapt when theme colors
 *    are updated in SCSS.
 *
 * 2. We read from the `.pf-theme-provider` element (not document.documentElement)
 *    because that's where theme classes (.pf-theme-provider--light/--dark) are
 *    applied and where CSS variables are scoped.
 *
 * 3. ECharts doesn't automatically pick up CSS variable changes, so chart
 *    components must call getChartThemeColors() when building options and
 *    rebuild when the theme changes (see EChartView.onThemeChange).
 *
 * 4. Chart options that set sub-objects (like axisLabel: {fontSize: 10})
 *    override theme values entirely - ECharts doesn't deep merge. Therefore,
 *    chart_option_builder.ts explicitly includes theme colors in axis options.
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
 * Returns true if dark theme is currently active.
 */
export function isDarkTheme(): boolean {
  const themeProvider = document.querySelector('.pf-theme-provider');
  return themeProvider?.classList.contains('pf-theme-provider--dark') ?? false;
}

/**
 * Returns the current theme colors by reading CSS variables from the
 * theme provider element. Colors are defined in theme_provider.scss.
 */
export function getChartThemeColors(): ChartThemeColors {
  const themeProvider = document.querySelector('.pf-theme-provider');
  if (themeProvider === null) {
    throw new Error('Theme provider element not found');
  }
  const style = getComputedStyle(themeProvider);

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

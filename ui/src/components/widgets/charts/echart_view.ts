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
 * ECharts integration for Perfetto UI.
 *
 * THEME HANDLING:
 *
 * ECharts themes are registered at initialization time by reading CSS variables
 * from the theme provider (see chart_theme.ts). When the user switches themes:
 *
 * 1. A MutationObserver detects the class change on .pf-theme-provider
 * 2. onThemeChange() re-registers ECharts themes with fresh CSS variable values
 * 3. The chart is disposed and re-initialized with the new theme
 * 4. m.redraw() triggers parent components to rebuild options with new colors
 *
 * This approach ensures charts respond to theme changes without page reload.
 */

import m from 'mithril';
import * as echarts from 'echarts/core';
import {
  BarChart as EBarChart,
  LineChart as ELineChart,
  PieChart as EPieChart,
  ScatterChart as EScatterChart,
  TreemapChart as ETreemapChart,
} from 'echarts/charts';
import {
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DataZoomComponent,
  BrushComponent,
  ToolboxComponent,
} from 'echarts/components';
import {CanvasRenderer} from 'echarts/renderers';
import type {EChartsType} from 'echarts/core';
import {classNames} from '../../../base/classnames';
import {SimpleResizeObserver} from '../../../base/resize_observer';
import {Spinner} from '../../../widgets/spinner';
import {
  isDarkTheme,
  getChartThemeColors,
  type ChartThemeColors,
} from './chart_theme';

// Re-export for backward compatibility
export {getChartThemeColors as getPerfettoThemeColors};
export type {ChartThemeColors as ThemeColors};

let echartsInitialized = false;

function ensureEChartsSetup(): void {
  if (echartsInitialized) return;
  echartsInitialized = true;
  echarts.use([
    EBarChart,
    ELineChart,
    EPieChart,
    EScatterChart,
    ETreemapChart,
    GridComponent,
    TooltipComponent,
    LegendComponent,
    DataZoomComponent,
    BrushComponent,
    ToolboxComponent,
    CanvasRenderer,
  ]);
  registerPerfettoThemes();
}

/**
 * Returns the ECharts theme name based on current theme.
 */
function getCurrentThemeName(): 'perfetto-light' | 'perfetto-dark' {
  return isDarkTheme() ? 'perfetto-dark' : 'perfetto-light';
}

/**
 * Builds an ECharts theme object by reading CSS variables.
 */
function buildEChartsTheme(): Record<string, unknown> {
  const theme = getChartThemeColors();

  return {
    color: theme.chartColors,
    backgroundColor: 'transparent',
    textStyle: {
      color: theme.textColor,
      fontFamily: 'inherit',
    },
    title: {
      textStyle: {
        color: theme.textColor,
      },
    },
    legend: {
      textStyle: {
        color: theme.textColor,
      },
    },
    tooltip: {
      backgroundColor: theme.backgroundColor,
      borderColor: theme.borderColor,
      textStyle: {
        color: theme.textColor,
      },
    },
    axisPointer: {
      lineStyle: {
        color: theme.borderColor,
      },
      crossStyle: {
        color: theme.borderColor,
      },
    },
    xAxis: {
      axisLine: {
        lineStyle: {
          color: theme.borderColor,
        },
      },
      axisTick: {
        lineStyle: {
          color: theme.borderColor,
        },
      },
      axisLabel: {
        color: theme.textColor,
      },
      splitLine: {
        lineStyle: {
          color: theme.borderColor,
        },
      },
      nameTextStyle: {
        color: theme.textColor,
      },
    },
    yAxis: {
      axisLine: {
        lineStyle: {
          color: theme.borderColor,
        },
      },
      axisTick: {
        lineStyle: {
          color: theme.borderColor,
        },
      },
      axisLabel: {
        color: theme.textColor,
      },
      splitLine: {
        lineStyle: {
          color: theme.borderColor,
        },
      },
      nameTextStyle: {
        color: theme.textColor,
      },
    },
  };
}

/**
 * Registers both light and dark Perfetto themes with ECharts.
 * Called once during ECharts initialization.
 */
function registerPerfettoThemes(): void {
  // Register themes with current CSS variable values.
  // Note: Theme registration happens once at init time. For dynamic theme
  // switching, we re-initialize the chart instance with the new theme name.
  const theme = buildEChartsTheme();
  echarts.registerTheme('perfetto-light', theme);
  echarts.registerTheme('perfetto-dark', theme);
}

// Global set to track all mounted EChartView instances
const mountedCharts = new Set<EChartView>();

// Single MutationObserver for all charts
let themeObserver: MutationObserver | undefined;

/**
 * Starts observing theme provider class changes to detect theme switches.
 * Only creates the observer when the first chart mounts.
 */
function startThemeObserver(): void {
  if (themeObserver !== undefined) return;

  const themeProvider = document.querySelector('.pf-theme-provider');
  if (themeProvider === null) return;

  themeObserver = new MutationObserver((mutations) => {
    for (const mutation of mutations) {
      if (
        mutation.type === 'attributes' &&
        mutation.attributeName === 'class'
      ) {
        const newTheme = getCurrentThemeName();
        // Notify all mounted charts
        for (const chart of mountedCharts) {
          chart.onThemeChange(newTheme);
        }
        break;
      }
    }
  });

  themeObserver.observe(themeProvider, {
    attributes: true,
    attributeFilter: ['class'],
  });
}

/**
 * Stops the theme observer when no charts are mounted.
 */
function stopThemeObserver(): void {
  if (themeObserver !== undefined && mountedCharts.size === 0) {
    themeObserver.disconnect();
    themeObserver = undefined;
  }
}

/**
 * Typed params for the ECharts `brushEnd` event.
 * Used by chart brush handlers to extract selected ranges.
 */
export interface EChartBrushEndParams {
  readonly areas?: ReadonlyArray<{
    readonly coordRange?: [number, number];
  }>;
}

/**
 * Typed params for ECharts click/interaction events.
 */
export interface EChartClickParams {
  readonly name?: string;
  readonly seriesName?: string;
  readonly dataIndex?: number;
  readonly value?: unknown;
  readonly data?: unknown;
  readonly marker?: string;
  readonly color?: string;
  readonly percent?: number;
}

/**
 * Event handler binding for an ECharts instance.
 * Handlers are wrapped by EChartView to call `m.redraw()` automatically
 * after each invocation, so callers do not need to trigger redraws.
 */
export interface EChartEventHandler {
  readonly eventName: string;
  readonly handler: (...args: unknown[]) => void;
}

export interface EChartViewAttrs {
  /**
   * ECharts option to render. When undefined, a loading spinner is shown.
   */
  readonly option: echarts.EChartsCoreOption | undefined;

  /**
   * Height of the chart in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Show empty state instead of loading. Defaults to false.
   */
  readonly empty?: boolean;

  /**
   * Event handlers to attach to the ECharts instance.
   */
  readonly eventHandlers?: ReadonlyArray<EChartEventHandler>;

  /**
   * Brush type to activate immediately. When set, brush mode is enabled
   * automatically without requiring user to click a toolbox button.
   */
  readonly activeBrushType?: 'rect' | 'lineX' | 'lineY';
}

const DEFAULT_HEIGHT = 200;

export class EChartView implements m.ClassComponent<EChartViewAttrs> {
  private chart?: EChartsType;
  private container?: HTMLElement;
  private resizeObs?: Disposable;
  private prevHandlers: ReadonlyArray<EChartEventHandler> = [];
  private prevOptionJson?: string;
  private currentTheme: 'perfetto-light' | 'perfetto-dark' = 'perfetto-light';

  oncreate({dom, attrs}: m.CVnodeDOM<EChartViewAttrs>) {
    ensureEChartsSetup();
    this.currentTheme = getCurrentThemeName();

    const container = dom.querySelector(
      '.pf-echart-view__canvas',
    ) as HTMLElement | null;
    if (container === null) return;
    this.container = container;

    // Only init ECharts when we have an option to render (the canvas
    // is display:none during loading, so init would get 0×0 dimensions).
    if (attrs.option !== undefined) {
      this.initChart(attrs);
    }

    // Defer resize to the next frame so that a layout change caused by
    // chart.resize() doesn't re-trigger the observer in the same frame.
    this.resizeObs = new SimpleResizeObserver(dom, () => {
      requestAnimationFrame(() => this.chart?.resize());
    });

    // Register for theme changes
    mountedCharts.add(this);
    startThemeObserver();
  }

  onupdate({attrs}: m.CVnodeDOM<EChartViewAttrs>) {
    if (attrs.option === undefined) return;

    // Lazy init: first option arrived after a loading state.
    if (this.chart === undefined) {
      this.initChart(attrs);
      return;
    }

    const optionJson = JSON.stringify(attrs.option);
    if (optionJson !== this.prevOptionJson) {
      this.prevOptionJson = optionJson;
      this.chart.setOption(attrs.option, {notMerge: true});
      // The canvas may have been display:none (loading state) since the
      // last option, so ECharts' cached dimensions could be stale.
      this.chart.resize();
      this.activateBrush(attrs.activeBrushType);
    }
    this.syncHandlers(attrs.eventHandlers ?? []);
  }

  private initChart(attrs: EChartViewAttrs): void {
    if (this.container === undefined || attrs.option === undefined) return;
    this.chart = echarts.init(this.container, this.currentTheme);
    this.chart.setOption(attrs.option);
    this.prevOptionJson = JSON.stringify(attrs.option);
    this.syncHandlers(attrs.eventHandlers ?? []);
    this.activateBrush(attrs.activeBrushType);
  }

  private activateBrush(brushType: string | undefined): void {
    if (this.chart === undefined || brushType === undefined) return;
    this.chart.dispatchAction({
      type: 'takeGlobalCursor',
      key: 'brush',
      brushOption: {
        brushType,
        brushMode: 'single',
      },
    });
  }

  onremove() {
    mountedCharts.delete(this);
    stopThemeObserver();

    if (this.resizeObs) {
      this.resizeObs[Symbol.dispose]();
      this.resizeObs = undefined;
    }
    this.detachAllHandlers();
    if (this.chart) {
      this.chart.dispose();
      this.chart = undefined;
    }
  }

  /**
   * Called when the document theme changes.
   * Re-registers ECharts themes with new CSS values and reinitializes charts.
   */
  onThemeChange(newTheme: 'perfetto-light' | 'perfetto-dark'): void {
    if (this.currentTheme === newTheme) return;
    this.currentTheme = newTheme;

    // Re-register themes with updated CSS variable values
    const theme = buildEChartsTheme();
    echarts.registerTheme('perfetto-light', theme);
    echarts.registerTheme('perfetto-dark', theme);

    // Re-initialize chart with new theme
    if (this.chart !== undefined && this.container !== undefined) {
      const currentOption = this.chart.getOption();
      this.chart.dispose();
      this.chart = echarts.init(this.container, newTheme);
      this.chart.setOption(currentOption, {notMerge: true});
      this.syncHandlers(this.prevHandlers);
      this.chart.resize();
      m.redraw();
    }
  }

  view({attrs}: m.Vnode<EChartViewAttrs>) {
    const height = attrs.height ?? DEFAULT_HEIGHT;
    const isLoading = attrs.option === undefined && !attrs.empty;
    const isEmpty = attrs.empty === true;

    return m(
      '.pf-echart-view',
      {
        className: classNames(
          attrs.fillParent && 'pf-echart-view--fill-parent',
          attrs.className,
        ),
        // When fillParent is set, let the CSS class control height (100%)
        // instead of setting an explicit pixel height via inline style.
        style: attrs.fillParent ? undefined : {height: `${height}px`},
      },
      [
        m('.pf-echart-view__canvas', {
          className: classNames(
            (isLoading || isEmpty) && 'pf-echart-view__canvas--hidden',
          ),
        }),
        isLoading && m('.pf-echart-view__loading', m(Spinner)),
        isEmpty && m('.pf-echart-view__empty', 'No data to display'),
      ],
    );
  }

  private syncHandlers(handlers: ReadonlyArray<EChartEventHandler>): void {
    this.detachAllHandlers();
    const wrapped: EChartEventHandler[] = [];
    for (const h of handlers) {
      // Wrap each handler to trigger a Mithril redraw after it executes,
      // so chart event handlers don't need to call m.redraw() manually.
      const wrappedHandler = (...args: unknown[]) => {
        h.handler(...args);
        m.redraw();
      };
      this.chart?.on(h.eventName, wrappedHandler);
      wrapped.push({eventName: h.eventName, handler: wrappedHandler});
    }
    this.prevHandlers = wrapped;
  }

  private detachAllHandlers(): void {
    for (const h of this.prevHandlers) {
      this.chart?.off(h.eventName, h.handler);
    }
    this.prevHandlers = [];
  }
}

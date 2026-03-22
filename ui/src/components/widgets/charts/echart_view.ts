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
 * Theme colors are read from CSS variables and used to build an ECharts theme
 * object at chart initialization time. When the user switches themes, the
 * component detects the color change and reinitializes the chart with the
 * new theme.
 *
 * This approach keeps chart options pure (no embedded theme colors) and
 * leverages ECharts' native theme system.
 */

import m from 'mithril';
import * as echarts from 'echarts/core';
import {
  BarChart as EBarChart,
  BoxplotChart as EBoxplotChart,
  HeatmapChart as EHeatmapChart,
  LineChart as ELineChart,
  PieChart as EPieChart,
  ScatterChart as EScatterChart,
  TreemapChart as ETreemapChart,
  SankeyChart as ESankeyChart,
  GaugeChart as EGaugeChart,
} from 'echarts/charts';
import {
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DataZoomComponent,
  BrushComponent,
  ToolboxComponent,
  VisualMapComponent,
  MarkAreaComponent,
} from 'echarts/components';
import {CanvasRenderer} from 'echarts/renderers';
import type {EChartsType} from 'echarts/core';
import {assertIsInstance} from '../../../base/assert';
import {classNames} from '../../../base/classnames';
import {SimpleResizeObserver} from '../../../base/resize_observer';
import {Spinner} from '../../../widgets/spinner';
import {type ChartThemeColors, getChartThemeColors} from './chart_theme';

// Re-export for backward compatibility
export type {ChartThemeColors as ThemeColors};

let echartsInitialized = false;

function ensureEChartsSetup(): void {
  if (echartsInitialized) return;
  echartsInitialized = true;
  echarts.use([
    EBarChart,
    EBoxplotChart,
    EHeatmapChart,
    ELineChart,
    EPieChart,
    EScatterChart,
    ETreemapChart,
    ESankeyChart,
    EGaugeChart,
    GridComponent,
    TooltipComponent,
    LegendComponent,
    DataZoomComponent,
    BrushComponent,
    ToolboxComponent,
    VisualMapComponent,
    MarkAreaComponent,
    CanvasRenderer,
  ]);
}

/**
 * Build an ECharts theme object from Perfetto theme colors.
 */
function buildEChartsTheme(colors: ChartThemeColors): Record<string, unknown> {
  return {
    color: [...colors.chartColors],
    backgroundColor: 'transparent',
    textStyle: {
      color: colors.textColor,
    },
    title: {
      textStyle: {color: colors.textColor},
      subtextStyle: {color: colors.textColor},
    },
    legend: {
      textStyle: {color: colors.textColor},
    },
    tooltip: {
      backgroundColor: colors.backgroundColor,
      borderColor: colors.borderColor,
      textStyle: {color: colors.textColor},
    },
    categoryAxis: {
      axisLine: {lineStyle: {color: colors.borderColor}},
      axisTick: {lineStyle: {color: colors.borderColor}},
      axisLabel: {color: colors.textColor},
      splitLine: {lineStyle: {color: colors.borderColor}},
      nameTextStyle: {color: colors.textColor},
    },
    valueAxis: {
      axisLine: {lineStyle: {color: colors.borderColor}},
      axisTick: {lineStyle: {color: colors.borderColor}},
      axisLabel: {color: colors.textColor},
      splitLine: {lineStyle: {color: colors.borderColor}},
      nameTextStyle: {color: colors.textColor},
    },
    logAxis: {
      axisLine: {lineStyle: {color: colors.borderColor}},
      axisTick: {lineStyle: {color: colors.borderColor}},
      axisLabel: {color: colors.textColor},
      splitLine: {lineStyle: {color: colors.borderColor}},
      nameTextStyle: {color: colors.textColor},
    },
    visualMap: {
      textStyle: {color: colors.textColor},
      inRange: {
        color: [colors.chartColors[0] + '22', colors.chartColors[0]],
      },
    },
  };
}

/**
 * Compute a simple hash of theme colors to detect changes.
 */
function themeHash(colors: ChartThemeColors): string {
  return `${colors.textColor}|${colors.borderColor}|${colors.backgroundColor}|${colors.chartColors.join(',')}`;
}

/**
 * Typed params for the ECharts `brushEnd` event.
 * Used by chart brush handlers to extract selected ranges.
 *
 * coordRange is [min, max] for 1-D brushes (lineX / lineY) and
 * [[xMin, xMax], [yMin, yMax]] for 2-D rect brushes.
 */
export interface EChartBrushEndParams {
  readonly areas?: ReadonlyArray<{
    readonly coordRange?:
      | [number, number]
      | [[number, number], [number, number]];
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
   * Optional callback to transform the option with theme colors.
   * Called after theme colors are read from the DOM, before the option
   * is applied to the ECharts instance. Useful for series-level color
   * overrides (e.g. gauge axis tracks) that the ECharts theme system
   * does not cover.
   */
  readonly resolveOption?: (
    option: echarts.EChartsCoreOption,
    colors: ChartThemeColors,
  ) => echarts.EChartsCoreOption;

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

// Shared theme name for all EChartView instances
const THEME_NAME = 'perfetto';

export class EChartView implements m.ClassComponent<EChartViewAttrs> {
  private chart?: EChartsType;
  private resizeObs?: Disposable;
  private prevHandlers: ReadonlyArray<EChartEventHandler> = [];
  private prevOptionJson?: string;
  private prevThemeHash?: string;

  oncreate({dom, attrs}: m.CVnodeDOM<EChartViewAttrs>) {
    ensureEChartsSetup();

    // Only init ECharts when we have an option to render (the canvas
    // is display:none during loading, so init would get 0×0 dimensions).
    if (attrs.option !== undefined) {
      this.initChart(attrs, dom);
    }

    // Defer resize to the next frame so that a layout change caused by
    // chart.resize() doesn't re-trigger the observer in the same frame.
    this.resizeObs = new SimpleResizeObserver(dom, () => {
      requestAnimationFrame(() => this.chart?.resize());
    });
  }

  onupdate({dom, attrs}: m.CVnodeDOM<EChartViewAttrs>) {
    if (attrs.option === undefined) return;

    // Lazy init: first option arrived after a loading state.
    if (this.chart === undefined) {
      this.initChart(attrs, dom);
      return;
    }

    // Check if theme changed - if so, reinitialize the chart
    const colors = getChartThemeColors(dom);
    const currentThemeHash = themeHash(colors);

    if (currentThemeHash !== this.prevThemeHash) {
      // Theme changed - dispose and reinit with new theme
      this.disposeChart();
      this.initChart(attrs, dom);
      return;
    }

    // Check if option changed
    const resolvedOption = this.applyResolveOption(attrs.option, attrs, colors);
    const optionJson = JSON.stringify(resolvedOption);
    if (optionJson !== this.prevOptionJson) {
      this.prevOptionJson = optionJson;
      this.chart.setOption(resolvedOption, {notMerge: true});
      // The canvas may have been display:none (loading state) since the
      // last option, so ECharts' cached dimensions could be stale.
      this.chart.resize();
      this.activateBrush(attrs.activeBrushType);
    }
    this.syncHandlers(attrs.eventHandlers ?? []);
  }

  private initChart(attrs: EChartViewAttrs, dom: Element): void {
    if (attrs.option === undefined) return;

    const container = dom.querySelector('.pf-echart-view__canvas');
    assertIsInstance(container, HTMLElement);

    // Read theme colors and register/update the ECharts theme
    const colors = getChartThemeColors(container);
    const theme = buildEChartsTheme(colors);
    echarts.registerTheme(THEME_NAME, theme);

    const resolvedOption = this.applyResolveOption(attrs.option, attrs, colors);

    // Initialize chart with the theme
    this.chart = echarts.init(container, THEME_NAME);
    this.chart.setOption(resolvedOption);

    this.prevOptionJson = JSON.stringify(resolvedOption);
    this.prevThemeHash = themeHash(colors);
    this.syncHandlers(attrs.eventHandlers ?? []);
    this.activateBrush(attrs.activeBrushType);
  }

  private applyResolveOption(
    option: echarts.EChartsCoreOption,
    attrs: EChartViewAttrs,
    colors: ChartThemeColors,
  ): echarts.EChartsCoreOption {
    return attrs.resolveOption ? attrs.resolveOption(option, colors) : option;
  }

  private disposeChart(): void {
    this.detachAllHandlers();
    if (this.chart) {
      this.chart.dispose();
      this.chart = undefined;
    }
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
    if (this.resizeObs) {
      this.resizeObs[Symbol.dispose]();
      this.resizeObs = undefined;
    }
    this.disposeChart();
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
      m('.pf-echart-view__canvas', {
        className: classNames(
          (isLoading || isEmpty) && 'pf-echart-view__canvas--hidden',
        ),
      }),
      isLoading && m('.pf-echart-view__loading', m(Spinner)),
      isEmpty && m('.pf-echart-view__empty', 'No data to display'),
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

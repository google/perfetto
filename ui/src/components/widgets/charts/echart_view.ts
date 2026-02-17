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
 * Theme colors are read from CSS variables and used to register an ECharts
 * theme at init time. When the theme changes (e.g., light/dark mode toggle),
 * the chart is disposed and re-initialized with the new theme. This approach
 * avoids mutating options (which would drop formatter functions) and ensures
 * charts automatically respond to theme changes.
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
import {getChartThemeColors, type ChartThemeColors} from './chart_theme';

const PERFETTO_THEME_NAME = 'perfetto';

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

/**
 * Build an ECharts theme object from CSS variable colors.
 * This sets default styling for axes, text, legends, tooltips, etc.
 */
function buildEChartsTheme(colors: ChartThemeColors): Record<string, unknown> {
  const {textColor, borderColor, backgroundColor, chartColors} = colors;
  return {
    color: chartColors,
    backgroundColor: 'transparent',
    textStyle: {
      color: textColor,
      fontFamily: 'inherit',
    },
    title: {textStyle: {color: textColor}},
    legend: {textStyle: {color: textColor}},
    tooltip: {
      backgroundColor,
      borderColor,
      textStyle: {color: textColor},
    },
    axisPointer: {
      lineStyle: {color: borderColor},
      crossStyle: {color: borderColor},
    },
    categoryAxis: {
      axisLabel: {color: textColor},
      nameTextStyle: {color: textColor},
      axisLine: {lineStyle: {color: borderColor}},
      axisTick: {lineStyle: {color: borderColor}},
      splitLine: {lineStyle: {color: borderColor}},
    },
    valueAxis: {
      axisLabel: {color: textColor},
      nameTextStyle: {color: textColor},
      axisLine: {lineStyle: {color: borderColor}},
      axisTick: {lineStyle: {color: borderColor}},
      splitLine: {lineStyle: {color: borderColor}},
    },
    logAxis: {
      axisLabel: {color: textColor},
      nameTextStyle: {color: textColor},
      axisLine: {lineStyle: {color: borderColor}},
      axisTick: {lineStyle: {color: borderColor}},
      splitLine: {lineStyle: {color: borderColor}},
    },
  };
}

export class EChartView implements m.ClassComponent<EChartViewAttrs> {
  private chart?: EChartsType;
  private container?: HTMLElement;
  private resizeObs?: Disposable;
  private prevHandlers: ReadonlyArray<EChartEventHandler> = [];
  private prevOptionJson?: string;
  private prevThemeJson?: string;

  oncreate({dom, attrs}: m.CVnodeDOM<EChartViewAttrs>) {
    ensureEChartsSetup();

    const container = dom.querySelector(
      '.pf-echart-view__canvas',
    ) as HTMLElement | null;
    if (container === null) return;
    this.container = container;

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

  onupdate({attrs, dom}: m.CVnodeDOM<EChartViewAttrs>) {
    if (attrs.option === undefined) return;

    // Read theme colors from DOM
    const themeColors = getChartThemeColors(dom);
    const themeJson = JSON.stringify(themeColors);

    // If theme changed, we need to re-init the chart with the new theme
    if (this.chart !== undefined && themeJson !== this.prevThemeJson) {
      this.chart.dispose();
      this.chart = undefined;
    }

    // Lazy init: first option arrived after a loading state, or theme changed.
    if (this.chart === undefined) {
      this.initChart(attrs, dom, themeColors);
      return;
    }

    // Update option (just stringify option, not themed version since theme is in ECharts)
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

  private initChart(
    attrs: EChartViewAttrs,
    dom: Element,
    themeColors?: ChartThemeColors,
  ): void {
    if (this.container === undefined || attrs.option === undefined) return;

    // Get theme colors if not provided
    const colors = themeColors ?? getChartThemeColors(dom);
    const themeJson = JSON.stringify(colors);

    // Register theme with ECharts (re-registering is safe, it just overwrites)
    echarts.registerTheme(PERFETTO_THEME_NAME, buildEChartsTheme(colors));

    this.chart = echarts.init(this.container, PERFETTO_THEME_NAME);
    this.chart.setOption(attrs.option);
    this.prevOptionJson = JSON.stringify(attrs.option);
    this.prevThemeJson = themeJson;
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

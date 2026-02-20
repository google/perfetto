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
 * All theme-sensitive values (axis colors, series colors, tooltip colors) are
 * embedded directly in the ECharts option object by reading CSS variables at
 * option-build time (see chart_option_builder.ts and chart_theme.ts).
 *
 * When the user switches themes, Mithril redraws the parent component, which
 * rebuilds the option with the latest CSS variable values. EChartView detects
 * the option change and calls setOption() to update the chart — no dispose or
 * re-initialization needed.
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
} from 'echarts/charts';
import {
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DataZoomComponent,
  BrushComponent,
  ToolboxComponent,
  VisualMapComponent,
} from 'echarts/components';
import {CanvasRenderer} from 'echarts/renderers';
import type {EChartsType} from 'echarts/core';
import {assertExists} from '../../../base/assert';
import {classNames} from '../../../base/classnames';
import {SimpleResizeObserver} from '../../../base/resize_observer';
import {Spinner} from '../../../widgets/spinner';
import {type ChartThemeColors, getChartThemeColors} from './chart_theme';

// Re-export for backward compatibility
export {getChartThemeColors as getPerfettoThemeColors};
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
    GridComponent,
    TooltipComponent,
    LegendComponent,
    DataZoomComponent,
    BrushComponent,
    ToolboxComponent,
    VisualMapComponent,
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

export class EChartView implements m.ClassComponent<EChartViewAttrs> {
  private chart?: EChartsType;
  private container?: HTMLElement;
  private resizeObs?: Disposable;
  private prevHandlers: ReadonlyArray<EChartEventHandler> = [];
  private prevOptionJson?: string;

  oncreate({dom, attrs}: m.CVnodeDOM<EChartViewAttrs>) {
    ensureEChartsSetup();

    this.container = assertExists(
      dom.querySelector('.pf-echart-view__canvas') as HTMLElement | null,
    );

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
    this.chart = echarts.init(this.container);
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

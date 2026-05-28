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

import m from 'mithril';
import {SimpleResizeObserver} from '../../../base/resize_observer';
import {Spinner} from '../../../widgets/spinner';
import {assertIsInstance} from '../../../base/assert';

export interface SvgChartFrameAttrs {
  /** Show a spinner instead of the chart. */
  readonly isLoading: boolean;
  /** Show the empty-state placeholder instead of the chart. */
  readonly isEmpty: boolean;
  /**
   * Imperative draw callback. Called with the measured container size on
   * mount, on every attrs change, and on resize. Return the SVG vnodes to
   * render inside the container.
   */
  readonly renderChart: (width: number, height: number) => m.Children;
}

/**
 * The chart-area primitive. Renders a single `.pf-chart-svg__container`
 * element and:
 *  - measures it with a ResizeObserver,
 *  - imperatively re-renders the caller's SVG into it on mount, attrs
 *    change, and resize (so the chart is drawn with real pixel dimensions
 *    on the very first paint),
 *  - swaps in a spinner or empty-state placeholder when asked.
 *
 * Outer layout (fixed height, legend positioning, etc.) is the caller's
 * responsibility — drop this next to a legend, tooltip, or whatever else
 * as normal Mithril siblings.
 */
export class SvgChartFrame implements m.ClassComponent<SvgChartFrameAttrs> {
  private resizeObs?: Disposable;
  private container?: HTMLElement;
  private currentAttrs?: SvgChartFrameAttrs;
  private prevW = 0;
  private prevH = 0;

  oncreate({dom, attrs}: m.CVnodeDOM<SvgChartFrameAttrs>) {
    this.container = assertIsInstance(dom, HTMLElement);
    this.currentAttrs = attrs;
    const {width, height} = this.container.getBoundingClientRect();
    this.prevW = width;
    this.prevH = height;
    this.draw(attrs, width, height);
    this.resizeObs = new SimpleResizeObserver(dom, () => {
      if (!this.container || !this.currentAttrs) return;
      const r = this.container.getBoundingClientRect();
      if (r.width === this.prevW && r.height === this.prevH) return;
      this.prevW = r.width;
      this.prevH = r.height;
      this.draw(this.currentAttrs, r.width, r.height);
    });
  }

  onupdate({dom, attrs}: m.CVnodeDOM<SvgChartFrameAttrs>) {
    this.container = assertIsInstance(dom, HTMLElement);
    this.currentAttrs = attrs;
    const {width, height} = this.container.getBoundingClientRect();
    this.prevW = width;
    this.prevH = height;
    this.draw(attrs, width, height);
  }

  onremove() {
    if (this.resizeObs) {
      this.resizeObs[Symbol.dispose]();
      this.resizeObs = undefined;
    }
    if (this.container) m.render(this.container, []);
  }

  private draw(attrs: SvgChartFrameAttrs, width: number, height: number) {
    if (!this.container) return;
    let inner: m.Children;
    if (attrs.isLoading) {
      inner = m('.pf-chart-svg__loading', m(Spinner));
    } else if (attrs.isEmpty) {
      inner = m('.pf-chart-svg__empty', 'No data to display');
    } else {
      inner = attrs.renderChart(width, height);
    }
    // m.render's hidden third parameter is the redraw hook used by
    // Mithril's own renderer. Wiring it here means event handlers inside
    // the imperatively-rendered SVG can trigger a global redraw, which
    // flows back through onupdate above and re-invokes renderChart.
    const renderWithRedraw = m.render as (
      el: Element,
      vnodes: m.Children,
      redraw?: () => void,
    ) => void;
    renderWithRedraw(this.container, inner, m.redraw);
  }

  view() {
    return m('.pf-chart-svg__container');
  }
}

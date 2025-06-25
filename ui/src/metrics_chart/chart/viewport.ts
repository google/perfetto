// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Selection, select, ScaleLinear, scaleLinear} from 'd3';
import {Chart} from './chart';
import {drawCanvas} from './utils';
import {IChartContainer} from './types';
import {
  CANVAS_CONTAINER_CLASS_NAME,
  CONTAINER_4_HEIGHT_CHANGE,
  X_AXIS_CONTAINER_CLASS_NAME,
} from './const';
import {XAxis} from './components';
import {zoom, ZoomTransform, zoomIdentity} from './third_party/d3-zoom/src';

const MAX_OVERSIZE_MULTIPLE = window.innerHeight * 3;

export class ViewPort {
  container: IChartContainer;
  xAxisContainer: IChartContainer | undefined;
  container4HeightChange: IChartContainer | undefined;
  canvasContainer: IChartContainer | undefined;

  chartDelegate: Chart;
  xAxis: XAxis | undefined;

  currentTransform: ZoomTransform = zoomIdentity;
  originalScale: ScaleLinear<number, number, never> | undefined;
  canvas:
    | {
        ctx: CanvasRenderingContext2D | undefined | null;
        selection: Selection<HTMLCanvasElement, unknown, null, undefined>;
      }
    | undefined;

  hasInit = false;

  offsetY = 0;

  constructor(container: IChartContainer, chartDelegate: Chart) {
    this.chartDelegate = chartDelegate;
    this.container = container;
  }

  get transform() {
    return this.currentTransform;
  }

  get maxCanvasHeight() {
    return this.chartDelegate.config.maxCanvasHeight ?? Infinity;
  }

  getFinalHeightOfContainer4HeightChange(canvasHeight: number) {
    return canvasHeight > this.maxCanvasHeight
      ? this.maxCanvasHeight
      : canvasHeight;
  }

  getFinalHeightOfCanvas(canvasHeight: number) {
    return canvasHeight > MAX_OVERSIZE_MULTIPLE
      ? MAX_OVERSIZE_MULTIPLE
      : canvasHeight;
  }

  init(canvasHeight: number) {
    this.initXAxis();
    const finalHeightOfCanvas = this.getFinalHeightOfCanvas(canvasHeight);
    const finalHeightOfContainer4HeightChange =
      this.getFinalHeightOfContainer4HeightChange(finalHeightOfCanvas);
    this.appendContainer4HeightChange(
      canvasHeight,
      finalHeightOfCanvas,
      finalHeightOfContainer4HeightChange,
    );
    this.hasInit = true;
  }

  update(canvasHeight: number) {
    const finalHeightOfCanvas = this.getFinalHeightOfCanvas(canvasHeight);
    const finalHeightOfContainer4HeightChange =
      this.getFinalHeightOfContainer4HeightChange(finalHeightOfCanvas);
    this.container4HeightChange?.selection
      .style('height', `${finalHeightOfContainer4HeightChange}px`)
      .style(
        'overflow',
        finalHeightOfContainer4HeightChange === this.maxCanvasHeight
          ? 'auto'
          : 'hidden',
      );
  }

  close() {
    this.canvas?.selection.remove();
    this.container4HeightChange?.node.remove();
    this.xAxisContainer?.selection.remove();
    this.canvasContainer?.node.remove();
  }

  createXAxisContainerEle() {
    const div = document.createElement('div');
    div.style.width = '100%';
    div.className = X_AXIS_CONTAINER_CLASS_NAME;
    this.container.node.appendChild(div);
    return div;
  }

  initXAxis() {
    this.originalScale = scaleLinear()
      .domain(
        this.chartDelegate.chartDataProvider.getDataRange(
          this.chartDelegate.config,
          this.container.node.clientWidth,
        ),
      )
      .range([0, this.container.node.clientWidth])
      .nice();
    this.xAxis = new XAxis(this.originalScale, this.chartDelegate.config);
    const div = this.createXAxisContainerEle();
    this.xAxisContainer = {
      node: div,
      selection: select(div),
    };
    this.xAxis.draw(this.xAxisContainer);
  }

  createContainer4HeightChange(
    canvasHeight: number,
    finalCanvasHeight: number,
    finalHeightOfContainer4HeightChange: number,
  ) {
    const div = document.createElement('div');
    div.style.width = '100%';
    div.style.height = `${finalHeightOfContainer4HeightChange}px`;
    div.style.overflow = 'auto';
    div.className = CONTAINER_4_HEIGHT_CHANGE;
    div.addEventListener('scroll', () => {
      const maxYDelta =
        Math.min(finalHeightOfContainer4HeightChange, window.innerHeight) - 200;
      let newOffset = div.scrollTop - maxYDelta;
      if (Math.abs(newOffset - this.offsetY) > maxYDelta) {
        newOffset = Math.max(
          0,
          Math.min(newOffset, canvasHeight - finalCanvasHeight),
        );
        if (newOffset !== this.offsetY) {
          this.offsetY = newOffset;
          this.canvas?.selection.style('top', `${this.offsetY}px`);
          this.chartDelegate.update();
        }
      }
    });
    return div;
  }

  appendContainer4HeightChange(
    canvasHeight: number,
    finalCanvasHeight: number,
    finalHeightOfContainer4HeightChange: number,
  ) {
    const container4HeightChange = this.createContainer4HeightChange(
      canvasHeight,
      finalCanvasHeight,
      finalHeightOfContainer4HeightChange,
    );
    this.container.node.appendChild(container4HeightChange);
    this.container4HeightChange = {
      node: container4HeightChange,
      selection: select(container4HeightChange),
    };
    this.appendInteractionEle(container4HeightChange, finalCanvasHeight);
  }

  createCanvasContainerEle(finalCanvasHeight: number) {
    const div = document.createElement('div');
    div.style.width = '100%';
    div.style.height = `${finalCanvasHeight}px`;
    div.style.position = 'relative';
    div.className = CANVAS_CONTAINER_CLASS_NAME;
    return div;
  }

  appendInteractionEle(parent: HTMLDivElement, finalCanvasHeight: number) {
    const interactionEle = this.createCanvasContainerEle(finalCanvasHeight);
    parent.appendChild(interactionEle);
    this.canvasContainer = {
      node: interactionEle,
      selection: select(interactionEle),
    };
    this.canvas = drawCanvas(this.canvasContainer, finalCanvasHeight);
    this.initZoom();
    this.initMouseEvent();
  }

  initZoom() {
    const handleZoom = zoom()
      .scaleExtent(
        this.chartDelegate.config.scale === false
          ? [1, 1]
          : this.chartDelegate.config.scale,
      )
      .extent([
        [0, 0],
        [
          this.canvasContainer?.node.clientWidth,
          this.canvasContainer?.node.clientHeight,
        ],
      ])
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      .on('zoom', (event: any) => {
        requestAnimationFrame(() => {
          const {transform} = event;
          this.currentTransform = transform;
          this.chartDelegate.update();
        });
      });

    this.canvas?.selection.call(handleZoom);
  }

  initMouseEvent() {
    this.canvas?.selection.on('mousemove', (e: MouseEvent) =>
      this.chartDelegate.mouseMove(e),
    );
    this.canvas?.selection.on('mousedown', (e: MouseEvent) =>
      this.chartDelegate.mouseDown(e),
    );
    this.canvas?.selection.on('mouseup', (e: MouseEvent) =>
      this.chartDelegate.mouseUp(e),
    );
    this.canvas?.selection.on('dblclick.zoom', null);
    this.canvas?.selection.on('mouseout', (e: MouseEvent) => {
      this.chartDelegate.mouseOut(e);
    });
  }
}

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

import {TNode} from './types';
import {IChartConfig} from '../config';
import {ScaleLinear} from 'd3';
import {
  TFlowEndTraceEvent,
  TFlowStartTraceEvent,
  TFlowStepTraceEvent,
  TFlowTraceEvent,
  TTraceEvent,
} from '../types';
import {
  isFlowEndTraceEvent,
  isFlowStartTraceEvent,
  isFlowStepTraceEvent,
  isFlowTraceEvent,
} from '../utils';

export class Flow {
  private from: (TFlowStartTraceEvent | TFlowStepTraceEvent)[] = [];

  private to: (TFlowEndTraceEvent | TFlowStepTraceEvent)[] = [];

  private flowStartEvents: Map<
    string,
    TFlowStartTraceEvent | TFlowStepTraceEvent
  > = new Map();

  private chartMap: Map<TFlowTraceEvent, TTraceEvent> = new Map();

  constructor(data: TTraceEvent[]) {
    this.process(data);
  }

  processStartTraceEvent(
    cur: TFlowStartTraceEvent | TFlowStepTraceEvent,
    data: TTraceEvent[],
    idx: number,
  ) {
    this.addStartEvent(cur);
    const target = this.findBindEvent(data, idx, true);
    if (target) {
      this.chartMap.set(cur, target);
    }
  }
  processEndTraceEvent(
    cur: TFlowEndTraceEvent | TFlowStepTraceEvent,
    data: TTraceEvent[],
    idx: number,
  ) {
    this.addEndEvent(cur);
    const target = this.findBindEvent(data, idx, false);
    if (target) {
      this.chartMap.set(cur, target);
    }
  }

  /**
   * 1. Traverse all trace data
   * 2. Identify all flow data, with the following strategies:
   *   a) Flow start: Store the id and event in a map; find the associated non-flow event
   *   b) Flow end: Retrieve the corresponding flow start/flow step time from the map using the id, using it as the drawing start point, and the current point as the drawing end point; find the associated non-flow event
   *   c）Flow step: Combines the logic of flow start and flow end, acting both as a drawing end point and a drawing start point; find the associated non-flow event
   * @param data trace data
   */
  process(data: TTraceEvent[]) {
    let idx = 0;
    while (idx < data.length) {
      const cur = data[idx];
      if (isFlowStartTraceEvent(cur)) {
        this.processStartTraceEvent(cur, data, idx);
        idx += 1;
        continue;
      }
      if (isFlowStepTraceEvent(cur)) {
        this.addEndEvent(cur);
        const target = this.findBindEvent(data, idx, true);
        if (target) {
          this.chartMap.set(cur, target);
        }

        this.processStartTraceEvent(cur, data, idx);

        idx += 1;
        continue;
      }
      if (isFlowEndTraceEvent(cur)) {
        this.processEndTraceEvent(cur, data, idx);
        idx += 1;
        continue;
      }
      idx += 1;
    }
  }

  findBindEvent(data: TTraceEvent[], startIdx: number, isFlowStart: boolean) {
    let idx = startIdx;
    let res: TTraceEvent | undefined = undefined;
    while (!res && idx < data.length) {
      if (!isFlowTraceEvent(data[idx])) {
        res = data[idx];
        break;
      }
      if (isFlowStart) {
        idx -= 1;
      } else {
        idx += 1;
      }
    }
    return res;
  }

  addBindTraceEvent(flowTrace: TFlowTraceEvent, otherTrace: TTraceEvent) {
    this.chartMap.set(flowTrace, otherTrace);
  }

  addStartEvent(event: TFlowStartTraceEvent | TFlowStepTraceEvent) {
    this.flowStartEvents.set(event.id, event);
  }

  addEndEvent(event: TFlowEndTraceEvent | TFlowStepTraceEvent) {
    const fromEvent = this.flowStartEvents.get(event.id);
    if (fromEvent) {
      this.from.push(fromEvent);
      this.to.push(event);
      this.flowStartEvents.delete(event.id);
    }
  }

  getHeight(node: TNode, config: IChartConfig) {
    if (node.track) {
      return (
        node.track.y +
        (node.level + 0.5) *
          ((config.node?.height ?? 0) + (config.node?.margin ?? 0))
      );
    }
    // The mark node's height is greater than that of a normal node, which introduces an additional half of the node's height.
    const markHeight = node.group.containsMarkNodes() ? (config.node?.height ?? 0) / 2 : 0;
    return (
      node.group.y +
      (node.level + 0.5) *
      ((config.node?.height ?? 0) + (config.node?.margin ?? 0)) + markHeight
    );
  }

  calIsInViewPort(
    sx: number,
    sy: number,
    fx: number,
    fy: number,
    viewPortWidth: number,
    viewPortHeight: number,
  ) {
    // sx > sy
    if (sx > viewPortWidth) {
      return false;
    }
    if (fx < 0) {
      return false;
    }

    if (sy >= fy) {
      if (fy < 0) {
        return false;
      }
      if (sy > viewPortHeight) {
        return false;
      }
      return true;
    }
    if (sy < fy) {
      if (sy < 0) {
        return false;
      }
      if (fy > viewPortHeight) {
        return false;
      }
      return true;
    }
    return true;
  }

  draw(
    context: CanvasRenderingContext2D | undefined | null,
    eventToNode: Map<TTraceEvent, TNode>,
    config: IChartConfig,
    scale: ScaleLinear<number, number, never> | undefined,
    canvasWidth: number,
    canvasHeight: number,
    offsetY: number,
  ) {
    const {basis = 0} = config;
    const len = this.from.length;
    if (len === 0 || !context || !scale) {
      return;
    }
    for (let i = 0; i < len; i++) {
      const s = this.from[i];
      const f = this.to[i];
      const sBindEvent = this.chartMap.get(s);
      const fBindEvent = this.chartMap.get(f);
      if (sBindEvent === undefined || fBindEvent === undefined) {
        return;
      }

      // Based on the associated event, locate the corresponding rendering node.
      const sBindNode = eventToNode.get(sBindEvent);
      const fBindNode = eventToNode.get(fBindEvent);
      if (sBindNode === undefined || fBindNode === undefined) {
        return;
      }

      // Calculate the coordinates of the starting and ending points for rendering.
      const sy = this.getHeight(sBindNode, config) - offsetY;
      const fy = this.getHeight(fBindNode, config) - offsetY;
      const sx = Math.floor(scale(s.ts - basis));
      const fx = Math.floor(scale(f.ts - basis));
      if (
        !this.calIsInViewPort(sx, sy, fx, fy, canvasWidth, canvasHeight) ||
        (sBindNode.track || sBindNode.group).triangle.isCollapse ||
        (fBindNode.track || fBindNode.group).triangle.isCollapse
      ) {
        continue;
      }
      if (
        (sBindNode.track || sBindNode.group).triangle.isCollapse ||
        (fBindNode.track || fBindNode.group).triangle.isCollapse
      ) {
        continue;
      }
      if (
        (sBindNode.track && sBindNode.track.group?.triangle.isCollapse) ||
        (fBindNode.track && fBindNode.track.group?.triangle.isCollapse)
      ) {
        continue;
      }
      const segment = Math.max((fx - sx) / 3, 40);
      const p = [];
      p.push({x: sx, y: sy});
      p.push({x: sx + segment, y: sy});
      p.push({x: fx - segment, y: fy});
      p.push({x: fx, y: fy});
      const arrowWidth = 6;
      context.beginPath();
      context.lineWidth = 0.5;
      context.lineCap = 'round';
      context.lineJoin = 'round';
      context.strokeStyle = 'gray';
      context.moveTo(p[0].x, p[0].y);
      context.bezierCurveTo(p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
      context.stroke();
      // draw arrow
      context.beginPath();
      context.moveTo(fx, fy);
      context.lineTo(fx - arrowWidth, fy - 3);
      context.lineTo(fx - arrowWidth, fy + 3);
      context.fillStyle = 'gray';
      context.fill();
    }
  }

  get data() {
    return {from: this.from, to: this.to};
  }
}

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

import {getFontContent, IChartConfig} from '../config';
import {transform} from './utils';
import {TNode} from '../chart/types';
import {TTraceEvent} from '../types';
import {ETraceEventPhase, isMarkTraceEvent} from '../utils';

export class ChartDataProvider {
  private data: TTraceEvent[];

  private _nodeMap: Record<string, TNode[]> = {};

  dataRange: number[];

  readonly eventToNode: Map<TTraceEvent, TNode> = new Map();

  constructor(data: TTraceEvent[]) {
    this.data = this.sortMarkData(data);
    this.dataRange = [];
  }

  get nodeMap() {
    return this._nodeMap;
  }

  recordNode(id: string, node: TNode) {
    if (id in this._nodeMap) {
      this._nodeMap[id].push(node);
    } else {
      this._nodeMap[id] = [node];
    }
  }

  private sortMarkData(data: TTraceEvent[]): TTraceEvent[] {
    const markNodes: TTraceEvent[] = [];
    const nonMarkNodes: TTraceEvent[] = [];
    data.forEach((node) => {
      if (isMarkTraceEvent(node)) {
        markNodes.push(node);
      } else {
        nonMarkNodes.push(node);
      }
    });
    return [...markNodes.sort((a, b) => a.ts - b.ts), ...nonMarkNodes];
  }

  groups(config: IChartConfig) {
    return transform(
      this.data,
      config,
      this.eventToNode,
      this.recordNode.bind(this),
    );
  }

  getDataRange(config: IChartConfig, canvasWidth: number) {
    const {basis} = config;
    const dataLeftRange =
      this.data.reduce((prev, cur) => {
        if (Number(cur.ts) < prev) {
          prev = Number(cur.ts);
        }
        return prev;
      }, Number(Infinity)) - (basis ?? 0);
    const maybeDataRightRange =
      this.data.reduce((prev, cur) => {
        if (Number(cur.ts) > prev) {
          prev = Number(cur.ts);
        }
        return prev;
      }, -Infinity) - (basis ?? 0);
    /**
     * Add some padding to the chart boundaries to prevent content from appearing too close to the edge.
     */
    const boundaryPaddingRatio = 0.05;

    const markArr = this.data.filter((val) => val.ph === ETraceEventPhase.MARK);
    if (markArr.length > 0) {
      let preRenderCanvas: HTMLCanvasElement | null =
        document.createElement('canvas');
      const ctx = preRenderCanvas.getContext('2d');
      if (!ctx) {
        return [];
      }
      ctx.font = getFontContent(config);
      const ratio = (maybeDataRightRange - dataLeftRange) / canvasWidth;

      const markLocation = markArr.reduce((accu: number[][], cur, idx) => {
        const ts = cur.ts - (basis ?? 0);
        if (idx === 0) {
          accu.push([
            ts,
            ts +
              Math.ceil(
                ctx.measureText(cur.name || '').width +
                  (config.label?.padding ?? 0) * 2,
              ) *
                ratio,
          ]);
        } else {
          const prev = accu[idx - 1];
          if (ts <= prev[1]) {
            accu.push([
              prev[1],
              prev[1] +
                Math.ceil(
                  ctx.measureText(cur.name || '').width +
                    (config.label?.padding ?? 0) * 2,
                ) *
                  ratio,
            ]);
          } else {
            accu.push([
              ts,
              ts +
                Math.ceil(
                  ctx.measureText(cur.name || '').width +
                    (config.label?.padding ?? 0) * 2,
                ) *
                  ratio,
            ]);
          }
        }
        return accu;
      }, []);
      const dataRightRange = Math.max(
        maybeDataRightRange,
        markLocation[markLocation.length - 1][1],
      );
      preRenderCanvas = null;
      const boundaryPadding =
        (dataRightRange - dataLeftRange) * boundaryPaddingRatio;
      this.dataRange = [dataLeftRange - boundaryPadding, dataRightRange];
    } else {
      const boundaryPadding =
        (maybeDataRightRange - dataLeftRange) * boundaryPaddingRatio;
      this.dataRange = [dataLeftRange - boundaryPadding, maybeDataRightRange];
    }
    return this.dataRange;
  }
}

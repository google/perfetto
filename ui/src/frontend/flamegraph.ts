// Copyright (C) 2018 The Android Open Source Project
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

import {searchSegment} from '../base/binary_search';
import {cropText} from '../common/canvas_utils';

import {CallsiteInfo} from '../common/state';

interface Node {
  width: number;
  x: number;
  nextXForChildren: number;
  size: number;
}

interface CallsiteInfoWidth {
  callsite: CallsiteInfo;
  width: number;
}

// Height of one 'row' on the flame chart including 1px of whitespace
// below the box.
const NODE_HEIGHT = 18;

export const HEAP_PROFILE_HOVERED_COLOR = 'hsl(224, 45%, 55%)';

export function findRootSize(data: CallsiteInfo[]) {
  let totalSize = 0;
  let i = 0;
  while (i < data.length && data[i].depth === 0) {
    totalSize += data[i].totalSize;
    i++;
  }
  return totalSize;
}

export interface NodeRendering {
  totalSize?: string;
  selfSize?: string;
}

export class Flamegraph {
  private nodeRendering: NodeRendering = {};
  private flamegraphData: CallsiteInfo[];
  private maxDepth = -1;
  private totalSize = -1;
  // Initialised on first draw() call
  private labelCharWidth = 0;
  private labelFontStyle = '12px Roboto Mono';
  private rolloverFontStyle = '12px Roboto Condensed';
  // Key for the map is depth followed by x coordinate - `depth;x`
  private graphData: Map<string, CallsiteInfoWidth> = new Map();
  private xStartsPerDepth: Map<number, number[]> = new Map();

  private hoveredX = -1;
  private hoveredY = -1;
  private hoveredCallsite?: CallsiteInfo;
  private clickedCallsite?: CallsiteInfo;

  private startingY = 0;

  constructor(flamegraphData: CallsiteInfo[]) {
    this.flamegraphData = flamegraphData;
    this.findMaxDepth();
  }

  private findMaxDepth() {
    this.maxDepth = Math.max(...this.flamegraphData.map(value => value.depth));
  }

  generateColor(name: string, isGreyedOut = false): string {
    if (isGreyedOut) {
      return '#d9d9d9';
    }
    if (name === 'unknown' || name === 'root') {
      return '#c0c0c0';
    }
    let x = 0;
    for (let i = 0; i < name.length; i += 1) {
      x += name.charCodeAt(i) % 64;
    }
    x = x % 360;
    return `hsl(${x}deg, 45%, 76%)`;
  }

  /**
   * Caller will have to call draw method after updating data to have updated
   * graph.
   */
  updateDataIfChanged(
      nodeRendering: NodeRendering, flamegraphData: CallsiteInfo[],
      clickedCallsite?: CallsiteInfo) {
    this.nodeRendering = nodeRendering;
    this.clickedCallsite = clickedCallsite;
    if (this.flamegraphData === flamegraphData) {
      return;
    }
    this.flamegraphData = flamegraphData;
    this.clickedCallsite = clickedCallsite;
    this.findMaxDepth();
    // Finding total size of roots.
    this.totalSize = findRootSize(flamegraphData);
  }

  draw(
      ctx: CanvasRenderingContext2D, width: number, height: number, x = 0,
      y = 0, unit = 'B') {

    if (this.flamegraphData === undefined) {
      return;
    }

    ctx.font = this.labelFontStyle;
    ctx.textBaseline = 'middle';
    if (this.labelCharWidth === 0) {
      this.labelCharWidth = ctx.measureText('_').width;
    }

    this.startingY = y;

    // For each node, we use this map to get information about it's parent
    // (total size of it, width and where it starts in graph) so we can
    // calculate it's own position in graph.
    const nodesMap = new Map<number, Node>();
    let currentY = y;
    nodesMap.set(-1, {width, nextXForChildren: x, size: this.totalSize, x});

    // Initialize data needed for click/hover behavior.
    this.graphData = new Map();
    this.xStartsPerDepth = new Map();

    // Draw root node.
    ctx.fillStyle = this.generateColor('root', false);
    ctx.fillRect(x, currentY, width, NODE_HEIGHT - 1);
    const text = cropText(
        `root: ${
            this.displaySize(
                this.totalSize, unit, unit === 'B' ? 1024 : 1000)}`,
        this.labelCharWidth,
        width - 2);
    ctx.fillStyle = 'black';
    ctx.fillText(text, x + 5, currentY + (NODE_HEIGHT - 1) / 2);
    currentY += NODE_HEIGHT;

    // Set style for borders.
    ctx.strokeStyle = 'white';
    ctx.lineWidth = 0.5;

    for (let i = 0; i < this.flamegraphData.length; i++) {
      if (currentY > height) {
        break;
      }
      const value = this.flamegraphData[i];
      const parentNode = nodesMap.get(value.parentId);
      if (parentNode === undefined) {
        continue;
      }

      const isClicked = this.clickedCallsite !== undefined;
      const isFullWidth =
          isClicked && value.depth <= this.clickedCallsite!.depth;
      const isGreyedOut =
          isClicked && value.depth < this.clickedCallsite!.depth;

      const parent = value.parentId;
      const parentSize = parent === -1 ? this.totalSize : parentNode.size;
      // Calculate node's width based on its proportion in parent.
      const width =
          (isFullWidth ? 1 : value.totalSize / parentSize) * parentNode.width;

      const currentX = parentNode.nextXForChildren;
      currentY = y + NODE_HEIGHT * (value.depth + 1);

      // Draw node.
      const name = this.getCallsiteName(value);
      ctx.fillStyle = this.generateColor(name, isGreyedOut);
      ctx.fillRect(currentX, currentY, width, NODE_HEIGHT - 1);

      // Set current node's data in map for children to use.
      nodesMap.set(value.id, {
        width,
        nextXForChildren: currentX,
        size: value.totalSize,
        x: currentX
      });
      // Update next x coordinate in parent.
      nodesMap.set(value.parentId, {
        width: parentNode.width,
        nextXForChildren: currentX + width,
        size: parentNode.size,
        x: parentNode.x
      });

      // Draw name.
      const labelPaddingPx = 5;
      const maxLabelWidth = width - labelPaddingPx * 2;
      let text = cropText(name, this.labelCharWidth, maxLabelWidth);
      // If cropped text and the original text are within 20% we keep the
      // original text and just squish it a bit.
      if (text.length * 1.2 > name.length) {
        text = name;
      }
      ctx.fillStyle = 'black';
      ctx.fillText(
          text,
          currentX + labelPaddingPx,
          currentY + (NODE_HEIGHT - 1) / 2,
          maxLabelWidth);

      // Draw border on the right of node.
      ctx.beginPath();
      ctx.moveTo(currentX + width, currentY);
      ctx.lineTo(currentX + width, currentY + NODE_HEIGHT);
      ctx.stroke();

      // Add this node for recognizing in click/hover.
      // Map graphData contains one callsite which is on that depth and X
      // start. Map xStartsPerDepth for each depth contains all X start
      // coordinates that callsites on that level have.
      this.graphData.set(
          `${value.depth};${currentX}`, {callsite: value, width});
      const xStarts = this.xStartsPerDepth.get(value.depth);
      if (xStarts === undefined) {
        this.xStartsPerDepth.set(value.depth, [currentX]);
      } else {
        xStarts.push(currentX);
      }
    }

    // Draw the tooltip.
    if (this.hoveredX > -1 && this.hoveredY > -1 && this.hoveredCallsite) {
      // Must set these before measureText below.
      ctx.font = this.rolloverFontStyle;
      ctx.textBaseline = 'top';

      // Size in px of the border around the text and the edge of the rollover
      // background.
      const paddingPx = 8;
      // Size in px of the x and y offset between the mouse and the top left
      // corner of the rollover box.
      const offsetPx = 4;

      const lines: string[] = [];
      let lineSplitter: LineSplitter;
      const nameText = this.getCallsiteName(this.hoveredCallsite);
      const nameTextSize = ctx.measureText(nameText);
      lineSplitter =
          splitIfTooBig(nameText, width - paddingPx, nameTextSize.width);
      let textWidth = lineSplitter.lineWidth;
      lines.push(...lineSplitter.lines);

      const mappingText = this.hoveredCallsite.mapping;
      lineSplitter =
          splitIfTooBig(mappingText, width, ctx.measureText(mappingText).width);
      textWidth = Math.max(textWidth, lineSplitter.lineWidth);
      lines.push(...lineSplitter.lines);

      if (this.nodeRendering.totalSize !== undefined) {
        const percentage =
            this.hoveredCallsite.totalSize / this.totalSize * 100;
        const totalSizeText = `${this.nodeRendering.totalSize}: ${
            this.displaySize(
                this.hoveredCallsite.totalSize,
                unit,
                unit === 'B' ? 1024 : 1000)} (${percentage.toFixed(2)}%)`;
        lineSplitter = splitIfTooBig(
            totalSizeText, width, ctx.measureText(totalSizeText).width);
        textWidth = Math.max(textWidth, lineSplitter.lineWidth);
        lines.push(...lineSplitter.lines);
      }

      if (this.nodeRendering.selfSize !== undefined &&
          this.hoveredCallsite.selfSize > 0) {
        const selfPercentage =
            this.hoveredCallsite.selfSize / this.totalSize * 100;
        const selfSizeText = `${this.nodeRendering.selfSize}: ${
            this.displaySize(
                this.hoveredCallsite.selfSize,
                unit,
                unit === 'B' ? 1024 : 1000)} (${selfPercentage.toFixed(2)}%)`;
        lineSplitter = splitIfTooBig(
            selfSizeText, width, ctx.measureText(selfSizeText).width);
        textWidth = Math.max(textWidth, lineSplitter.lineWidth);
        lines.push(...lineSplitter.lines);
      }

      // Compute a line height as the bounding box height + 50%:
      const heightSample = ctx.measureText(
          'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ');
      const lineHeight =
          Math.round(heightSample.actualBoundingBoxDescent * 1.5);

      const rectWidth = textWidth + 2 * paddingPx;
      const rectHeight = lineHeight * lines.length + 2 * paddingPx;

      let rectXStart = this.hoveredX + offsetPx;
      let rectYStart = this.hoveredY + offsetPx;

      if (rectXStart + rectWidth > width) {
        rectXStart = width - rectWidth;
      }

      if (rectYStart + rectHeight > height) {
        rectYStart = height - rectHeight;
      }

      ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
      ctx.fillRect(rectXStart, rectYStart, rectWidth, rectHeight);
      ctx.fillStyle = 'hsl(200, 50%, 40%)';
      ctx.textAlign = 'left';
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        ctx.fillText(
            line,
            rectXStart + paddingPx,
            rectYStart + paddingPx + i * lineHeight);
      }
    }
  }

  private getCallsiteName(value: CallsiteInfo): string {
    return value.name === undefined || value.name === '' ? 'unknown' :
                                                           value.name;
  }

  private displaySize(totalSize: number, unit: string, step = 1024): string {
    if (unit === '') return totalSize.toLocaleString();
    if (totalSize === 0) return `0 ${unit}`;
    const units = [
      ['', 1],
      ['K', step],
      ['M', Math.pow(step, 2)],
      ['G', Math.pow(step, 3)]
    ];
    let unitsIndex = Math.trunc(Math.log(totalSize) / Math.log(step));
    unitsIndex = unitsIndex > units.length - 1 ? units.length - 1 : unitsIndex;
    const result = totalSize / +units[unitsIndex][1];
    const resultString = totalSize % +units[unitsIndex][1] === 0 ?
        result.toString() :
        result.toFixed(2);
    return `${resultString} ${units[unitsIndex][0]}${unit}`;
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    this.hoveredX = x;
    this.hoveredY = y;
    this.hoveredCallsite = this.findSelectedCallsite(x, y);
    const isCallsiteSelected = this.hoveredCallsite !== undefined;
    if (!isCallsiteSelected) {
      this.onMouseOut();
    }
    return isCallsiteSelected;
  }

  onMouseOut() {
    this.hoveredX = -1;
    this.hoveredY = -1;
    this.hoveredCallsite = undefined;
  }

  onMouseClick({x, y}: {x: number, y: number}): CallsiteInfo|undefined {
    const clickedCallsite = this.findSelectedCallsite(x, y);
    // TODO(b/148596659): Allow to expand [merged] callsites. Currently,
    // this expands to the biggest of the nodes that were merged, which
    // is confusing, so we disallow clicking on them.
    if (clickedCallsite === undefined || clickedCallsite.merged) {
      return undefined;
    }
    return clickedCallsite;
  }

  private findSelectedCallsite(x: number, y: number): CallsiteInfo|undefined {
    const depth =
        Math.trunc((y - this.startingY) / NODE_HEIGHT) - 1;  // at 0 is root
    if (depth >= 0 && this.xStartsPerDepth.has(depth)) {
      const startX = this.searchSmallest(this.xStartsPerDepth.get(depth)!, x);
      const result = this.graphData.get(`${depth};${startX}`);
      if (result !== undefined) {
        const width = result.width;
        return startX + width >= x ? result.callsite : undefined;
      }
    }
    return undefined;
  }

  searchSmallest(haystack: number[], needle: number): number {
    haystack = haystack.sort((n1, n2) => n1 - n2);
    const [left, ] = searchSegment(haystack, needle);
    return left === -1 ? -1 : haystack[left];
  }

  getHeight(): number {
    return this.flamegraphData.length === 0 ? 0 :
                                              (this.maxDepth + 2) * NODE_HEIGHT;
  }
}

export interface LineSplitter {
  lineWidth: number;
  lines: string[];
}

export function splitIfTooBig(
    line: string, width: number, lineWidth: number): LineSplitter {
  if (line === '') return {lineWidth, lines: []};
  const lines: string[] = [];
  const charWidth = lineWidth / line.length;
  const maxWidth = width - 32;
  const maxLineLen = Math.trunc(maxWidth / charWidth);
  while (line.length > 0) {
    lines.push(line.slice(0, maxLineLen));
    line = line.slice(maxLineLen);
  }
  lineWidth = Math.min(maxLineLen * charWidth, lineWidth);
  return {lineWidth, lines};
}

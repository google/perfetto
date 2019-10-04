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

import {CallsiteInfo} from './globals';

interface Node {
  width: number;
  startX: number;
  size: number;
}

const NODE_HEIGHT = 10;

export class Flamegraph {
  private nodeHeight = NODE_HEIGHT;
  private flamegraphData: CallsiteInfo[];

  constructor(flamegraphData: CallsiteInfo[]) {
    this.flamegraphData = flamegraphData;
  }

  hash(s: string): number {
    let hash = 0x811c9dc5 & 0xfffffff;
    for (let i = 0; i < s.length; i++) {
      hash ^= s.charCodeAt(i);
      hash = (hash * 16777619) & 0xffffffff;
    }
    return hash & 0xff;
  }

  generateColor(name: string|undefined): string {
    if (name === undefined) {
      return 'grey';
    }
    const hue = this.hash(name);
    const saturation = 50;
    return `hsl(${hue}, ${saturation}%, 65%)`;
  }

  /**
   * Caller will have to call draw method ater updating data to have updated
   * graph.
   */
  updateData(flamegraphData: CallsiteInfo[]) {
    this.flamegraphData = flamegraphData;
  }

  draw(
      ctx: CanvasRenderingContext2D, width: number, height: number, x = 0,
      y = 0, nodeHeight: number = NODE_HEIGHT) {
    this.nodeHeight = nodeHeight;
    if (this.flamegraphData !== undefined) {
      // Finding total size of roots.
      let totalSize = 0;
      this.flamegraphData.forEach((value: CallsiteInfo) => {
        if (value.parentHash === -1) {
          totalSize += value.totalSize;
        }
      });

      // For each node, we use this map to get information about it's parent
      // (total size of it, width and where it starts in graph) so we can
      // calculate it's own position in graph.
      const node = new Map<number, Node>();
      let currentY = y;
      node.set(-1, {width, startX: x, size: totalSize});

      // Draw root node.
      ctx.fillStyle = 'grey';
      ctx.fillRect(x, currentY, width, this.nodeHeight);
      currentY += this.nodeHeight;

      for (let i = 0; i < this.flamegraphData.length; i++) {
        if (currentY > height) {
          break;
        }
        const value = this.flamegraphData[i];
        const parentNode = node.get(value.parentHash);
        if (parentNode !== undefined) {
          const parent = value.parentHash;
          const parentSize = parent === -1 ? totalSize : parentNode.size;
          // Calculate node's width based on its proportion in parent.
          const width = value.totalSize / parentSize * parentNode.width;

          // Draw node.
          const currentX = parentNode.startX;
          currentY = this.nodeHeight * (value.depth + 1);
          ctx.fillStyle = this.generateColor(value.name);
          ctx.fillRect(currentX, currentY, width, this.nodeHeight);

          // Draw border around node.
          ctx.strokeStyle = 'white';
          ctx.beginPath();
          ctx.moveTo(currentX, currentY);
          ctx.lineTo(currentX, currentY + this.nodeHeight);
          ctx.lineTo(currentX + width, currentY + this.nodeHeight);
          ctx.lineTo(currentX + width, currentY);
          ctx.moveTo(currentX, currentY);
          ctx.lineWidth = 1;
          ctx.closePath();
          ctx.stroke();

          // Set current node's data in map for children to use.
          node.set(
              value.hash, {width, startX: currentX, size: value.totalSize});
          // Update next x coordinate in parent.
          node.set(value.parentHash, {
            width: parentNode.width,
            startX: currentX + width,
            size: parentNode.size
          });
        }
      }
    }
  }
}

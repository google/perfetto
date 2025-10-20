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

import {MultiSourceNode, QueryNode} from '../query_node';
import {NodeBoxLayout} from './graph/node_box';

export function isMultiSourceNode(node: QueryNode): node is MultiSourceNode {
  return 'prevNodes' in node;
}

export function findOverlappingNode(
  dragNodeLayout: NodeBoxLayout,
  resolvedNodeLayouts: Map<QueryNode, NodeBoxLayout>,
  dragNode: QueryNode,
): QueryNode | undefined {
  for (const [node, layout] of resolvedNodeLayouts.entries()) {
    if (node !== dragNode && isOverlapping(dragNodeLayout, layout, 0)) {
      return node;
    }
  }
  return undefined;
}

export function isOverlapping(
  layout1: NodeBoxLayout,
  layout2: NodeBoxLayout,
  padding: number,
): boolean {
  const w1 = layout1.width ?? 0;
  const h1 = layout1.height ?? 0;
  const w2 = layout2.width ?? 0;
  const h2 = layout2.height ?? 0;

  return (
    layout1.x < layout2.x + w2 + padding &&
    layout1.x + w1 + padding > layout2.x &&
    layout1.y < layout2.y + h2 + padding &&
    layout1.y + h1 + padding > layout2.y
  );
}

export function isOverlappingBottomPort(
  dragNodeLayout: NodeBoxLayout,
  targetNodeLayout: NodeBoxLayout,
  padding: number,
): boolean {
  const w1 = dragNodeLayout.width ?? 0;
  const h1 = dragNodeLayout.height ?? 0;
  const w2 = targetNodeLayout.width ?? 0;
  const h2 = targetNodeLayout.height ?? 0;

  const bottomPortY = targetNodeLayout.y + h2;
  const bottomPortX = targetNodeLayout.x + w2 / 2;

  return (
    dragNodeLayout.x < bottomPortX + padding &&
    dragNodeLayout.x + w1 > bottomPortX - padding &&
    dragNodeLayout.y < bottomPortY + padding &&
    dragNodeLayout.y + h1 > bottomPortY - padding
  );
}

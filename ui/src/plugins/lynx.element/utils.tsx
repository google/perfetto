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

import ElementManager from './element_manager';
import {LynxElement, LynxElementAbbr} from './types';
import {ReactNode} from 'react';

const DENSITY_THRESHOLDD = 1500;
const DESCENDANTS_THRESHOLD = 20;
const DEPTH_THRESHOLD = 25;
const NON_RENDERING_ELEMENT_RATIO = 0.7;
const NON_RENDERING_DESCENDANTS_THRESHOLD = 10;

export function isDeeplyNestedElement(element: LynxElement): boolean {
  if (element.depth < DEPTH_THRESHOLD || element.children.length > 0) {
    return false;
  }
  // we need the screen size info
  const screenSize = ElementManager.getScreenSize();
  if (
    Math.abs(screenSize.screenWidth) <= 1e-5 ||
    Math.abs(screenSize.screenHeight) <= 1e-5
  ) {
    return false;
  }

  // find the top level parent node, the node area must big enough
  let item = element;
  for (let i = 0; i < element.depth; i++) {
    if (!item.parent) {
      break;
    }

    item = item.parent;
    const areaRatio =
      (item.width * item.height) /
      (screenSize.screenHeight * screenSize.screenWidth);

    if (areaRatio > 0 && item.descendantCount > DESCENDANTS_THRESHOLD) {
      const nodeDensity = item.descendantCount / areaRatio;
      if (nodeDensity >= DENSITY_THRESHOLDD) {
        element.deeplyNested = true;
        return true;
      }
    }
  }

  return false;
}

export function isTwoElementOverlap(
  first: LynxElement,
  second: LynxElement,
): boolean {
  if (first === second) return true;

  // if 'first' and 'second' do not overlap, then 'first' may be left, top, right, bottom of 'second'
  return !(
    first.left + first.width < second.left ||
    first.top + first.height < second.top ||
    first.left > second.left + second.width ||
    first.top > second.top + second.height
  );
}

export function zeroSizeElement(current: LynxElement): boolean {
  return Math.abs(current.width) <= 1e-5 && Math.abs(current.height) <= 1e-5;
}

export function isWrappElement(tag: string): boolean {
  return tag === 'view' || tag === 'wrapper' || tag === 'component';
}

export function reConstructElementTree(
  current: LynxElementAbbr,
  parent?: LynxElement,
): LynxElement {
  const newElement: LynxElement = {
    width: current.w,
    height: current.h,
    left: current.l,
    top: current.t,
    name: current.n,
    id: current.i,
    class: current.cl,
    inlineStyle: current.in,
    attributes: current.at,

    children: [],
    descendantCount: 1,
    wrapDescendantCount: isWrappElement(current.n) ? 1 : 0,
    overNoRenderingRatio: 0,
    parent,
    depth: parent ? parent.depth + 1 : 0,
    lynxLeft: current.l + (parent?.left ?? 0),
    lynxTop: current.t + (parent?.top ?? 0),
    deeplyNested: false,
    invisible: false,
    hasExcessiveNonRenderingElements: false,
  };

  if (current.c && current.c.length > 0) {
    for (const child of current.c) {
      const childElement = reConstructElementTree(child, newElement);
      newElement.children.push(childElement);
      newElement.descendantCount += childElement.descendantCount;
      newElement.wrapDescendantCount += childElement.wrapDescendantCount;
    }
    newElement.overNoRenderingRatio = parseFloat(
      (newElement.wrapDescendantCount / newElement.descendantCount).toFixed(2),
    );
  }

  return newElement;
}

export function findDeeplyNestedNodesRecursively(
  root: LynxElement,
  current: LynxElement,
): LynxElement[] {
  const nodesList: LynxElement[] = [];

  if (isDeeplyNestedElement(current)) {
    nodesList.push(current);
  }

  for (let i = 0; i < current.children.length; i++) {
    const res = findDeeplyNestedNodesRecursively(root, current.children[i]);
    nodesList.push(...res);
  }
  nodesList.sort((a, b) => b.depth - a.depth);
  return nodesList;
}

export function findInvisibleNodesRecursively(
  root: LynxElement,
  current: LynxElement,
): LynxElement[] {
  const nodesList: LynxElement[] = [];
  if (
    current.name !== 'wrapper' &&
    (!isTwoElementOverlap(root, current) || zeroSizeElement(current)) &&
    current.descendantCount > DESCENDANTS_THRESHOLD
  ) {
    current.invisible = true;
    nodesList.push(current);
    return nodesList;
  }

  for (let i = 0; i < current.children.length; i++) {
    const res = findInvisibleNodesRecursively(root, current.children[i]);
    nodesList.push(...res);
  }
  nodesList.sort((a, b) => b.descendantCount - a.descendantCount);
  return nodesList;
}

export function findNonRenderingNodesRecursively(
  root: LynxElement,
  current: LynxElement,
): LynxElement[] {
  const nodesList: LynxElement[] = [];
  for (let i = 0; i < current.children.length; i++) {
    const res = findNonRenderingNodesRecursively(root, current.children[i]);
    nodesList.push(...res);
  }
  const nonRenderingElementRatio =
    current.wrapDescendantCount / current.descendantCount;
  if (
    nonRenderingElementRatio >= NON_RENDERING_ELEMENT_RATIO &&
    current.descendantCount >= NON_RENDERING_DESCENDANTS_THRESHOLD
  ) {
    current.hasExcessiveNonRenderingElements = true;
    // If the children contain an excessive number of non-rendering elements, then it is likely that the element is influenced by its children. So skip adding the element to nodeList.
    const childHasExcessiveNonRenderingElements = current.children.some(
      (item) => item.hasExcessiveNonRenderingElements,
    );
    if (!childHasExcessiveNonRenderingElements) {
      nodesList.push(current);
    }
  }

  nodesList.sort((a, b) => b.overNoRenderingRatio - a.overNoRenderingRatio);
  return nodesList;
}

export function constructElementDetail(current: LynxElement): string {
  return `<${current.name}${getIdDetail(current)}${getAttributesDetail(current)} />`;
}

export function constructElementDetailWithinDepth(
  current: LynxElement,
  depth: number,
): ReactNode {
  const MAX_DEPTH = 2;
  const idDetail = getIdDetail(current);
  const attributeDetail = getAttributesDetail(current);
  const currentElementDetail = `<${current.name}${idDetail}${attributeDetail}>`;
  const currentElementLine = (
    <div style={{paddingLeft: `${depth * 20}px`}}>{currentElementDetail}</div>
  );
  if (
    idDetail ||
    attributeDetail ||
    depth === MAX_DEPTH ||
    current.children.length <= 0
  ) {
    return currentElementLine;
  }
  return (
    <>
      {currentElementLine}
      {constructElementDetailWithinDepth(current.children[0], depth + 1)}
    </>
  );
}

function getIdDetail(current: LynxElement) {
  if (current.class && current.class.length > 0) {
    let res = ' class="';
    for (let i = 0; i < current.class.length; i++) {
      if (i > 0) res += ' ';
      res += current.class[i];
    }
    res += '"';
    return res;
  }
  return '';
}

function getAttributesDetail(current: LynxElement) {
  if (current.attributes && Object.keys(current.attributes).length > 0) {
    let res = '';
    Object.entries(current.attributes).forEach(([key, value]) => {
      res += ` ${key}="${value}"`;
    });
    return res;
  }
  return '';
}

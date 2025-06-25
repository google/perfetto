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

import {LynxElement} from './types';
import {ReactNode} from 'react';

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

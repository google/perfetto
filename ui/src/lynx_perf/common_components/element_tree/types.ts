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

export interface LynxElement {
  width: number;
  height: number;
  left: number;
  top: number;
  name: string;
  id: number;
  class?: string[];
  inlineStyle?: Record<string, string>;
  attributes?: Record<string, string>;
  children: LynxElement[];

  descendantCount: number;
  wrapDescendantCount: number;
  overNoRenderingRatio: number;
  parent?: LynxElement;
  depth: number;
  lynxLeft: number;
  lynxTop: number;

  // extension
  rootElement?: LynxElement;
  invisible: boolean;
  deeplyNested: boolean;
  hasExcessiveNonRenderingElements: boolean;
}

export interface ElementTreeViewState {
  currentSelectedElement: LynxElement | undefined;
  treeHeight: number;
  treeWidth: number;
}

export interface ElementTreeViewProps {
  selectedElement: LynxElement | undefined;
  rootElement: LynxElement | undefined;
  closeDialog: () => void;
}

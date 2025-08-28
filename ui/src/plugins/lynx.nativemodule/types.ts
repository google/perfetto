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

import {BaseSlice} from '../../lynx_perf/types';

export interface NativeModuleItem extends BaseSlice {
  name: string;
  depth: number;
  flowId: number;
  sections: NativeModuleSection[] | undefined;
  invokeEndTs: number;
  callbackStartTs: number;
}

export interface NativeModuleSection {
  beginTs: number;
  endTs: number;
  name: string;
  thread: string | Record<string, number>;
  description: string;
}

export interface DepthRange {
  leftTs: number;
  rightTs: number;
}

export const SECTION_COLOR = [
  '#005bb5', // Sky Blue Selected
  '#e65c00', // Orange Selected
  '#00cc60', // Spring Green Selected
  '#e60072', // Rose Red Selected
  '#6600cc', // Purple Selected
];

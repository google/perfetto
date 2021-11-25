// Copyright (C) 2021 The Android Open Source Project
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

export interface SliceLayoutBase {
  padding: number;     // top/bottom pixel padding between slices and track.
  rowSpacing: number;  // Spacing between rows.
  minDepth: number;    // Minimum depth a slice can be (normally zero)
  // Maximum depth a slice can be plus 1 (a half open range with minDepth).
  // We have a optimization for when maxDepth - minDepth == 1 so it is useful
  // to set this correctly:
  maxDepth: number;
}

export const SLICE_LAYOUT_BASE_DEFAULTS: SliceLayoutBase = Object.freeze({
  padding: 3,
  rowSpacing: 0,
  minDepth: 0,
  // A realistic bound to avoid tracks with unlimited height. If somebody wants
  // extremely deep tracks they need to change this explicitly.
  maxDepth: 128,
});

export interface SliceLayoutFixed extends SliceLayoutBase {
  heightMode: 'FIXED';
  fixedHeight: number;  // Outer height of the track.
}

export const SLICE_LAYOUT_FIXED_DEFAULTS: SliceLayoutFixed = Object.freeze({
  ...SLICE_LAYOUT_BASE_DEFAULTS,
  heightMode: 'FIXED',
  fixedHeight: 30,
});

export interface SliceLayoutFitContent extends SliceLayoutBase {
  heightMode: 'FIT_CONTENT';
  sliceHeight: number;  // Only when heightMode = 'FIT_CONTENT'.
}

export const SLICE_LAYOUT_FIT_CONTENT_DEFAULTS: SliceLayoutFitContent =
    Object.freeze({
      ...SLICE_LAYOUT_BASE_DEFAULTS,
      heightMode: 'FIT_CONTENT',
      sliceHeight: 18,
    });

export interface SliceLayoutFlat extends SliceLayoutBase {
  heightMode: 'FIXED';
  fixedHeight: number;  // Outer height of the track.
  minDepth: 0;
  maxDepth: 1;
}

export const SLICE_LAYOUT_FLAT_DEFAULTS: SliceLayoutFlat = Object.freeze({
  ...SLICE_LAYOUT_BASE_DEFAULTS,
  minDepth: 0,
  maxDepth: 1,
  heightMode: 'FIXED',
  fixedHeight: 30,
});

export type SliceLayout =
    SliceLayoutFixed|SliceLayoutFitContent|SliceLayoutFlat;

export const DEFAULT_SLICE_LAYOUT: SliceLayout =
    SLICE_LAYOUT_FIT_CONTENT_DEFAULTS;

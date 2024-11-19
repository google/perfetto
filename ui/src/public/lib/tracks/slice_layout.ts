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
  readonly padding: number; // vertical pixel padding between slices and track.
  readonly rowSpacing: number; // Spacing between rows.

  // A *guess* at the depth
  readonly depthGuess?: number;

  // True iff the track is flat (all slices have the same depth
  // we have an optimisation for this).
  readonly isFlat?: boolean;

  readonly titleSizePx?: number;
  readonly subtitleSizePx?: number;
}

export const SLICE_LAYOUT_BASE_DEFAULTS: SliceLayoutBase = Object.freeze({
  padding: 3,
  rowSpacing: 0,
});

export interface SliceLayoutFixed extends SliceLayoutBase {
  readonly heightMode: 'FIXED';
  readonly fixedHeight: number; // Outer height of the track.
}

export const SLICE_LAYOUT_FIXED_DEFAULTS: SliceLayoutFixed = Object.freeze({
  ...SLICE_LAYOUT_BASE_DEFAULTS,
  heightMode: 'FIXED',
  fixedHeight: 30,
});

export interface SliceLayoutFitContent extends SliceLayoutBase {
  readonly heightMode: 'FIT_CONTENT';
  readonly sliceHeight: number; // Only when heightMode = 'FIT_CONTENT'.
}

export const SLICE_LAYOUT_FIT_CONTENT_DEFAULTS: SliceLayoutFitContent =
  Object.freeze({
    ...SLICE_LAYOUT_BASE_DEFAULTS,
    heightMode: 'FIT_CONTENT',
    sliceHeight: 18,
  });

export interface SliceLayoutFlat extends SliceLayoutBase {
  readonly heightMode: 'FIXED';
  readonly fixedHeight: number; // Outer height of the track.
  readonly depthGuess: 0;
  readonly isFlat: true;
}

export const SLICE_LAYOUT_FLAT_DEFAULTS: SliceLayoutFlat = Object.freeze({
  ...SLICE_LAYOUT_BASE_DEFAULTS,
  depthGuess: 0,
  isFlat: true,
  heightMode: 'FIXED',
  fixedHeight: 18,
  titleSizePx: 10,
  padding: 3,
});

export type SliceLayout =
  | SliceLayoutFixed
  | SliceLayoutFitContent
  | SliceLayoutFlat;

export const DEFAULT_SLICE_LAYOUT: SliceLayout =
  SLICE_LAYOUT_FIT_CONTENT_DEFAULTS;

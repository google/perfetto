// Copyright (C) 2026 The Android Open Source Project
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

export interface DataBounds {
  readonly xMin: number;
  readonly xMax: number;
  readonly yMin: number;
  readonly yMax: number;
}

export interface ViewRange {
  readonly x0: number;
  readonly x1: number;
  readonly y0: number;
  readonly y1: number;
}

export interface PointColumns {
  readonly x: Float64Array;
  readonly y: Float64Array;
  readonly row: Float64Array;
  readonly color: Float32Array;
  readonly weight?: Float32Array;
}

export interface PointSet {
  readonly id: number;
  readonly count: number;
  readonly bounds: DataBounds;
  readonly x: Float64Array;
  readonly y: Float64Array;
  readonly row: Float64Array;
  readonly color: Float32Array;
  readonly weight?: Float32Array;
}

export const NULL_CATEGORY = -1;
export const TOP_N_CATEGORIES = 20;
export const NUMERIC_LUT_SIZE = 256;

export type ColorMode =
  | {readonly kind: 'none'}
  | {
      readonly kind: 'numeric';
      readonly column: string;
      readonly min: number;
      readonly max: number;
    }
  | {
      readonly kind: 'categorical';
      readonly column: string;
      // Top-N category values; index i is the colour index i.
      readonly categories: ReadonlyArray<string>;
      // Row count for each entry in `categories` (same order).
      readonly counts: ReadonlyArray<number>;
      readonly hasOther: boolean;
      // Rows bucketed into "Other" / with a NULL colour value.
      readonly otherCount: number;
      readonly nullCount: number;
    };

export interface PointStyle {
  readonly sizePx: number;
  readonly opacity: number;
}

export interface ColorEncoding {
  readonly kind: 'uniform' | 'numeric' | 'categorical';
  readonly lut: Uint8Array;
  readonly nullColor: Uint8Array;
  readonly min: number;
  readonly max: number;
}

export type ColumnKind = 'numeric' | 'string' | 'other';

export interface ColumnInfo {
  readonly name: string;
  readonly kind: ColumnKind;
}

export type SourceSpec =
  | {
      readonly kind: 'table';
      readonly table: string;
      readonly module?: string;
    }
  | {readonly kind: 'query'; readonly sql: string};

export interface PlotSpec {
  readonly x: string;
  readonly y: string;
  readonly color?: string;
}

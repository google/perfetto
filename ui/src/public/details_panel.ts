// Copyright (C) 2024 The Android Open Source Project
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

import m from 'mithril';
import {LegacySelection, Selection} from './selection';

export interface LegacyDetailsPanel {
  render(selection: LegacySelection): m.Children;
  isLoading?(): boolean;
}

export interface DetailsPanel {
  render(selection: Selection): m.Children;
  isLoading?(): boolean;
}

export interface TrackSelectionDetailsPanel {
  render(id: number): m.Children;
  isLoading?(): boolean;
}

// TODO(primiano): rationalize this GenericSliceDetailsTabConfig. it should be
// probably moved to a public/lib/ next.
export interface ColumnConfig {
  readonly displayName?: string;
}

export type Columns = {
  readonly [columnName: string]: ColumnConfig;
};

export interface GenericSliceDetailsTabConfigBase {
  readonly sqlTableName: string;
  readonly title: string;
  // All columns are rendered if |columns| is undefined.
  readonly columns?: Columns;
}

export type GenericSliceDetailsTabConfig = GenericSliceDetailsTabConfigBase & {
  readonly id: number;
};

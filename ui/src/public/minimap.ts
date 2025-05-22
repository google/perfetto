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

import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {duration, time} from '../base/time';

export interface MinimapCell {
  readonly ts: time;
  readonly dur: duration;
  readonly load: number;
}

export type MinimapRow = MinimapCell[];

export interface MinimapContentProvider {
  readonly priority: number;
  getData(
    timeSpan: HighPrecisionTimeSpan,
    resolution: duration,
  ): Promise<MinimapRow[]>;
}

export interface MinimapManager {
  registerContentProvider(x: MinimapContentProvider): void;
}

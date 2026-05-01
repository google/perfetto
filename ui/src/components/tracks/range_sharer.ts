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

import {Trace} from '../../public/trace';
import {ChartHeightSize, YMode} from './counter_track';

interface RangeSharingParams {
  yRangeSharingKey?: string;
  yMode: YMode;
  yDisplay: string;
  chartHeightSize: ChartHeightSize;
}

export class RangeSharer {
  private static traceToRangeSharer = new WeakMap<Trace, RangeSharer>();

  private tagToRange: Map<string, [number, number]>;

  constructor() {
    this.tagToRange = new Map();
  }

  static getRangeSharer(trace: Trace): RangeSharer {
    let sharer = RangeSharer.traceToRangeSharer.get(trace);
    if (sharer === undefined) {
      sharer = new RangeSharer();
      RangeSharer.traceToRangeSharer.set(trace, sharer);
    }
    return sharer;
  }

  share(
    config: RangeSharingParams,
    [min, max]: [number, number],
  ): [number, number] {
    const key = config.yRangeSharingKey;
    if (!key) {
      return [min, max];
    }

    const tag = `${key}-${config.yMode}-${config.yDisplay}-${config.chartHeightSize}`;
    const cachedRange = this.tagToRange.get(tag);
    if (cachedRange === undefined) {
      this.tagToRange.set(tag, [min, max]);
      return [min, max];
    }

    cachedRange[0] = Math.min(min, cachedRange[0]);
    cachedRange[1] = Math.max(max, cachedRange[1]);

    return [cachedRange[0], cachedRange[1]];
  }
}

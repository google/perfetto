// Copyright (C) 2023 The Android Open Source Project
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

import {BigintMath} from '../base/bigint_math';
import {duration, Time, time, TimeSpan} from '../base/time';
export {Store} from '../base/store';
import {raf} from '../core/raf_scheduler';

type FetchTimeline<Data> = (
  start: time,
  end: time,
  resolution: duration,
) => Promise<Data>;

// This helper provides the logic to call |doFetch()| only when more
// data is needed as the visible window is panned and zoomed about, and
// includes an FSM to ensure doFetch is not re-entered.
export class TimelineFetcher<Data> implements Disposable {
  private doFetch: FetchTimeline<Data>;

  private data_?: Data;

  // Timespan and resolution of the latest *request*. data_ may cover
  // a different time window.
  private latestTimespan: TimeSpan;
  private latestResolution: duration;

  constructor(doFetch: FetchTimeline<Data>) {
    this.doFetch = doFetch;
    this.latestTimespan = TimeSpan.ZERO;
    this.latestResolution = 0n;
  }

  async requestData(timespan: TimeSpan, resolution: duration): Promise<void> {
    if (this.shouldLoadNewData(timespan, resolution)) {
      // Over request data, one page worth to the left and right.
      const padded = timespan.pad(timespan.duration);
      const start = padded.start;
      const end = padded.end;

      // Quantize up and down to the bounds of |resolution|.
      const startQ = Time.fromRaw(BigintMath.quantFloor(start, resolution));
      const endQ = Time.fromRaw(BigintMath.quantCeil(end, resolution));

      this.latestTimespan = new TimeSpan(startQ, endQ);
      this.latestResolution = resolution;
      await this.loadData();
    }
  }

  get data(): Data | undefined {
    return this.data_;
  }

  invalidate() {
    this.data_ = undefined;
  }

  [Symbol.dispose]() {
    this.data_ = undefined;
  }

  private shouldLoadNewData(timespan: TimeSpan, resolution: duration): boolean {
    if (this.data_ === undefined) {
      return true;
    }

    if (timespan.start < this.latestTimespan.start) {
      return true;
    }

    if (timespan.end > this.latestTimespan.end) {
      return true;
    }

    if (resolution !== this.latestResolution) {
      return true;
    }

    return false;
  }

  private async loadData(): Promise<void> {
    const {start, end} = this.latestTimespan;
    const resolution = this.latestResolution;
    this.data_ = await this.doFetch(start, end, resolution);
    raf.scheduleRedraw();
  }
}

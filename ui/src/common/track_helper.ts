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

import m from 'mithril';

import {Disposable} from '../base/disposable';
import {duration, Time, time, TimeSpan} from '../base/time';
import {raf} from '../core/raf_scheduler';
import {globals} from '../frontend/globals';
import {PanelSize} from '../frontend/panel';
import {SliceRect, Track, TrackContext} from '../public';

export {Store} from '../frontend/store';
export {EngineProxy} from '../trace_processor/engine';
export {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../trace_processor/query_result';

type FetchTimeline<Data> = (start: time, end: time, resolution: duration) =>
    Promise<Data>;

// This helper provides the logic to call |doFetch()| only when more
// data is needed as the visible window is panned and zoomed about, and
// includes an FSM to ensure doFetch is not re-entered.
class TimelineFetcher<Data> implements Disposable {
  private requestingData = false;
  private queuedRequest = false;
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

  requestDataForCurrentTime(): void {
    const currentTimeSpan = globals.timeline.visibleTimeSpan;
    const currentResolution = globals.getCurResolution();
    this.requestData(currentTimeSpan, currentResolution);
  }

  requestData(timespan: TimeSpan, resolution: duration): void {
    if (this.shouldLoadNewData(timespan, resolution)) {
      // Over request data, one page worth to the left and right.
      const start = Time.sub(timespan.start, timespan.duration);
      const end = Time.add(timespan.end, timespan.duration);
      this.latestTimespan = new TimeSpan(start, end);
      this.latestResolution = resolution;
      this.loadData();
    }
  }

  get data(): Data|undefined {
    return this.data_;
  }

  dispose() {
    this.queuedRequest = false;
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

  private loadData(): void {
    if (this.requestingData) {
      this.queuedRequest = true;
      return;
    }
    const {start, end} = this.latestTimespan;
    const resolution = this.latestResolution;
    this.doFetch(start, end, resolution).then((data) => {
      this.requestingData = false;
      this.data_ = data;
      if (this.queuedRequest) {
        this.queuedRequest = false;
        this.loadData();
      } else {
        raf.scheduleRedraw();
      }
    });
    this.requestingData = true;
  }
}

// A helper class which provides a base track implementation for tracks which
// load their content asynchronously from the trace.
//
// Tracks extending this base class need only define |renderCanvas()| and
// |onBoundsChange()|. This helper provides sensible default implementations for
// all the |Track| interface methods which subclasses may also choose to
// override if necessary.
//
// This helper provides the logic to call |onBoundsChange()| only when more data
// is needed as the visible window is panned and zoomed about, and includes an
// FSM to ensure onBoundsChange is not re-entered, and that the track doesn't
// render stale data.
//
// Note: This class is deprecated and should not be used for new tracks. Use
// |BaseSliceTrack| instead.
export abstract class TrackHelperLEGACY<Data> implements Track {
  private timelineFetcher: TimelineFetcher<Data>;

  constructor() {
    this.timelineFetcher =
        new TimelineFetcher<Data>(this.onBoundsChange.bind(this));
  }

  onCreate(_ctx: TrackContext): void {}

  onDestroy(): void {
    this.timelineFetcher.dispose();
  }

  get data(): Data|undefined {
    return this.timelineFetcher.data;
  }

  // Returns a place where a given slice should be drawn. Should be implemented
  // only for track types that support slices e.g. chrome_slice, async_slices
  // tStart - slice start time in seconds, tEnd - slice end time in seconds,
  // depth - slice depth
  getSliceRect(_tStart: time, _tEnd: time, _depth: number): SliceRect
      |undefined {
    return undefined;
  }

  abstract getHeight(): number;

  getTrackShellButtons(): m.Children {
    return [];
  }

  onMouseMove(_position: {x: number; y: number;}): void {}

  onMouseClick(_position: {x: number; y: number;}): boolean {
    return false;
  }

  onMouseOut(): void {}

  onFullRedraw(): void {}

  abstract onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data>;

  abstract renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize): void;

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void {
    this.timelineFetcher.requestDataForCurrentTime();
    this.renderCanvas(ctx, size);
  }
}

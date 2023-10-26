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

import {duration, Span, Time, time} from '../base/time';
import {raf} from '../core/raf_scheduler';
import {globals} from '../frontend/globals';
import {PxSpan, TimeScale} from '../frontend/time_scale';
import {SliceRect} from '../frontend/track';
import {Track, TrackContext} from '../public';

import {TrackData} from './track_data';

export {Store} from '../frontend/store';
export {EngineProxy} from './engine';
export {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from './query_result';

// This shim track provides the base for async style tracks implementing the new
// plugin track interface.
// This provides the logic to perform data reloads at appropriate times as the
// window is panned and zoomed about.
// The extending class need only define renderCanvas() and onBoundsChange().
export abstract class BasicAsyncTrack<Data> implements Track {
  private requestingData = false;
  private queuedRequest = false;
  private currentState?: TrackData;
  protected data?: Data;

  onCreate(_ctx: TrackContext): void {}

  onDestroy(): void {
    this.queuedRequest = false;
    this.currentState = undefined;
    this.data = undefined;
  }

  // Returns a place where a given slice should be drawn. Should be implemented
  // only for track types that support slices e.g. chrome_slice, async_slices
  // tStart - slice start time in seconds, tEnd - slice end time in seconds,
  // depth - slice depth
  getSliceRect(
      _visibleTimeScale: TimeScale, _visibleWindow: Span<time, duration>,
      _windowSpan: PxSpan, _tStart: time, _tEnd: time,
      _depth: number): SliceRect|undefined {
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

  abstract renderCanvas(ctx: CanvasRenderingContext2D): void;

  render(ctx: CanvasRenderingContext2D): void {
    if (this.shouldLoadNewData()) {
      this.loadData();
    }

    this.renderCanvas(ctx);
  }

  private loadData(): void {
    if (this.requestingData) {
      this.queuedRequest = true;
      return;
    }

    const ts = globals.frontendLocalState.visibleTimeSpan;
    const resolution = globals.getCurResolution();

    const start = Time.sub(ts.start, ts.duration);
    const end = Time.add(ts.end, ts.duration);

    this.currentState = {
      start,
      end,
      resolution,
      length: 0,
    };

    this.onBoundsChange(start, end, resolution).then((data) => {
      this.requestingData = false;
      this.data = data;

      if (this.queuedRequest) {
        this.queuedRequest = false;
        this.loadData();
      } else {
        raf.scheduleRedraw();
      }
    });

    this.requestingData = true;
  }

  private shouldLoadNewData(): boolean {
    if (!this.currentState) {
      return true;
    }

    const ts = globals.frontendLocalState.visibleTimeSpan;
    if (ts.start < this.currentState.start) {
      return true;
    }

    if (ts.end > this.currentState.end) {
      return true;
    }

    if (globals.getCurResolution() !== this.currentState.resolution) {
      return true;
    }

    return false;
  }
}

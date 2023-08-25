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

import {assertExists} from '../base/logging';
import {EngineProxy} from '../common/engine';
import {duration, Span, Time, time} from '../common/time';
import {TrackData} from '../common/track_data';
import {globals} from '../frontend/globals';
import {PxSpan, TimeScale} from '../frontend/time_scale';
import {NewTrackArgs, SliceRect} from '../frontend/track';
import {TrackButtonAttrs} from '../frontend/track_panel';
import {TrackLike} from '../public';

export {EngineProxy} from '../common/engine';
export {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../common/query_result';
export {Store} from '../frontend/store';

// This is an adapter to convert old style controller based tracks to new style
// tracks.
export class TrackWithControllerAdapter<Config, Data> implements TrackLike {
  private track: TrackAdapter<Config, Data>;
  private controller: TrackControllerAdapter<Config, Data>;
  private requestingData = false;
  private queuedRequest = false;
  private currentState?: TrackData;

  constructor(
      engine: EngineProxy, id: string, config: Config,
      Track: TrackAdapterClass<Config, Data>,
      Controller: TrackControllerAdapterClass<Config, Data>) {
    const args: NewTrackArgs = {
      trackId: id,
      engine,
    };
    this.track = new Track(args);
    this.track.setConfig(config);
    this.controller = new Controller(config, engine);
  }

  onDestroy(): void {
    this.queuedRequest = false;
    this.currentState = undefined;
    this.track.onDestroy();
  }

  getSliceRect(
      visibleTimeScale: TimeScale, visibleWindow: Span<time, bigint>,
      windowSpan: PxSpan, tStart: time, tEnd: time, depth: number): SliceRect
      |undefined {
    return this.track.getSliceRect(
        visibleTimeScale, visibleWindow, windowSpan, tStart, tEnd, depth);
  }

  getHeight(): number {
    return this.track.getHeight();
  }

  getTrackShellButtons(): m.Vnode<TrackButtonAttrs, {}>[] {
    return this.track.getTrackShellButtons();
  }

  getContextMenu(): m.Vnode<any, {}>|null {
    return this.track.getContextMenu();
  }

  onMouseMove(position: {x: number; y: number;}): void {
    this.track.onMouseMove(position);
  }

  onMouseClick(position: {x: number; y: number;}): boolean {
    return this.track.onMouseClick(position);
  }

  onMouseOut(): void {
    this.track.onMouseOut();
  }

  onFullRedraw(): void {
    this.track.onFullRedraw();
  }

  render(ctx: CanvasRenderingContext2D): void {
    if (this.shouldLoadNewData()) {
      this.loadData(ctx);
    }

    this.track.renderCanvas(ctx);
  }

  private loadData(ctx: CanvasRenderingContext2D): void {
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

    this.controller.onBoundsChange(start, end, resolution).then((data) => {
      this.requestingData = false;
      this.track.setData(data);

      if (this.queuedRequest) {
        this.queuedRequest = false;
        this.loadData(ctx);
      } else {
        this.track.renderCanvas(ctx);
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

// Extend from this class instead of `Track` to use existing track
// implementations with `TrackWithControllerAdapter`.
export abstract class TrackAdapter<Config, Data> {
  private _config?: Config;
  private _data?: Data;

  get config(): Config {
    return assertExists(this._config);
  }

  setConfig(config: Config) {
    this._config = config;
  }

  data(): undefined|Data {
    return this._data;
  }

  setData(data: Data) {
    this._data = data;
  }

  constructor(_args: NewTrackArgs) {}

  abstract renderCanvas(ctx: CanvasRenderingContext2D): void;

  getSliceRect(
      _visibleTimeScale: TimeScale, _visibleWindow: Span<time, bigint>,
      _windowSpan: PxSpan, _tStart: time, _tEnd: time,
      _depth: number): SliceRect|undefined {
    return undefined;
  }

  getHeight(): number {
    return 40;
  }

  getTrackShellButtons(): Array<m.Vnode<TrackButtonAttrs>> {
    return [];
  }

  getContextMenu(): m.Vnode<any>|null {
    return null;
  }

  onMouseMove(_position: {x: number, y: number}) {}

  // Returns whether the mouse click has selected something.
  // Used to prevent further propagation if necessary.
  onMouseClick(_position: {x: number, y: number}): boolean {
    return false;
  }

  onMouseOut(): void {}

  onFullRedraw(): void {}

  onDestroy(): void {
    // Drop this potentially large object
    this._data = undefined;
  }
}

type TrackAdapterClass<Config, Data> = {
  new (args: NewTrackArgs): TrackAdapter<Config, Data>
}

// Extend from this class instead of `TrackController` to use existing track
// controller implementations with `TrackWithControllerAdapter`.
export abstract class TrackControllerAdapter<Config, Data> {
  constructor(protected config: Config, private engine: EngineProxy) {}

  protected async query(query: string) {
    const result = await this.engine.query(query);
    return result;
  }

  abstract onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data>;
}

type TrackControllerAdapterClass<Config, Data> = {
  new (config: Config, engine: EngineProxy):
      TrackControllerAdapter<Config, Data>
}

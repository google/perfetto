// Copyright (C) 2018 The Android Open Source Project
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
import {duration, Span, time} from '../base/time';
import {EngineProxy} from '../common/engine';
import {Track, TrackContext} from '../public';

import {globals} from './globals';
import {PxSpan, TimeScale} from './time_scale';

// Args passed to the track constructors when creating a new track.
export interface NewTrackArgs {
  trackKey: string;
  engine: EngineProxy;
}

// This interface forces track implementations to have some static properties.
// Typescript does not have abstract static members, which is why this needs to
// be in a separate interface.
export interface TrackCreator {
  // Store the kind explicitly as a string as opposed to using class.kind in
  // case we ever minify our code.
  readonly kind: string;

  // We need the |create| method because the stored value in the registry can be
  // an abstract class, and we cannot call 'new' on an abstract class.
  create(args: NewTrackArgs): TrackBase;
}

export interface SliceRect {
  left: number;
  width: number;
  top: number;
  height: number;
  visible: boolean;
}

// The abstract class that needs to be implemented by all tracks.
export abstract class TrackBase<Config = {}> implements Track {
  protected readonly trackKey: string;
  protected readonly engine: EngineProxy;
  private _config?: Config;

  get config(): Config {
    return assertExists(this._config);
  }

  set config(x: Config) {
    this._config = x;
  }

  constructor(args: NewTrackArgs) {
    this.trackKey = args.trackKey;
    this.engine = args.engine;
  }

  onCreate(_ctx: TrackContext) {}

  // Last call the track will receive. Called just before the last reference to
  // this object is removed.
  onDestroy() {}

  protected abstract renderCanvas(ctx: CanvasRenderingContext2D): void;

  getHeight(): number {
    return 40;
  }

  getTrackShellButtons(): m.Children {
    return [];
  }

  onMouseMove(_position: {x: number, y: number}) {}

  // Returns whether the mouse click has selected something.
  // Used to prevent further propagation if necessary.
  onMouseClick(_position: {x: number, y: number}): boolean {
    return false;
  }

  onMouseOut(): void {}

  onFullRedraw(): void {}

  render(ctx: CanvasRenderingContext2D) {
    globals.frontendLocalState.addVisibleTrack(this.trackKey);
    this.renderCanvas(ctx);
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
}

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

import {time} from '../base/time';
import {SliceRect, Track} from '../public';
import {EngineProxy} from '../trace_processor/engine';

import {PanelSize} from './panel';
// Args passed to the track constructors when creating a new track.
export interface NewTrackArgs {
  trackKey: string;
  engine: EngineProxy;
}

// The abstract class that needs to be implemented by all tracks.
export abstract class TrackBase implements Track {
  protected readonly trackKey: string;
  protected readonly engine: EngineProxy;

  constructor(args: NewTrackArgs) {
    this.trackKey = args.trackKey;
    this.engine = args.engine;
  }

  protected abstract renderCanvas(
      ctx: CanvasRenderingContext2D, size: PanelSize): void;

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

  render(ctx: CanvasRenderingContext2D, size: PanelSize) {
    this.renderCanvas(ctx, size);
  }

  // Returns a place where a given slice should be drawn. Should be implemented
  // only for track types that support slices e.g. chrome_slice, async_slices
  // tStart - slice start time in seconds, tEnd - slice end time in seconds,
  // depth - slice depth
  getSliceRect(_tStart: time, _tEnd: time, _depth: number): SliceRect
      |undefined {
    return undefined;
  }
}

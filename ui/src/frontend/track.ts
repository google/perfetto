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
import {TrackState} from '../common/state';
import {TrackData} from '../common/track_data';
import {Track} from '../public';

import {checkerboard} from './checkerboard';
import {globals} from './globals';
import {PxSpan, TimeScale} from './time_scale';
import {TrackButtonAttrs} from './track_panel';

// Args passed to the track constructors when creating a new track.
export interface NewTrackArgs {
  trackId: string;
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
export abstract class TrackBase<Config = {}, Data extends TrackData = TrackData>
    implements Track {
  // The UI-generated track ID (not to be confused with the SQL track.id).
  protected readonly trackId: string;
  protected readonly engine: EngineProxy;

  // When true this is a new controller-less track type.
  // TODO(hjd): eventually all tracks will be controller-less and this
  // should be removed then.
  protected frontendOnly = false;

  // Caches the last state.track[this.trackId]. This is to deal with track
  // deletion, see comments in trackState() below.
  private lastTrackState: TrackState;

  constructor(args: NewTrackArgs) {
    this.trackId = args.trackId;
    this.engine = args.engine;
    this.lastTrackState = assertExists(globals.state.tracks[this.trackId]);
  }

  onCreate() {}

  // Last call the track will receive. Called just before the last reference to
  // this object is removed.
  onDestroy() {}

  protected abstract renderCanvas(ctx: CanvasRenderingContext2D): void;

  protected get trackState(): TrackState {
    // We can end up in a state where a Track is still in the mithril renderer
    // tree but its corresponding state has been deleted. This can happen in the
    // interval of time between a track being removed from the state and the
    // next animation frame that would remove the Track object. If a mouse event
    // is dispatched in the meanwhile (or a promise is resolved), we need to be
    // able to access the state. Hence the caching logic here.
    const trackState = globals.state.tracks[this.trackId];
    if (trackState === undefined) {
      return this.lastTrackState;
    }
    this.lastTrackState = trackState;
    return trackState;
  }

  get config(): Config {
    return this.trackState.config as Config;
  }

  data(): Data|undefined {
    if (this.frontendOnly) {
      return undefined;
    }
    return globals.trackDataStore.get(this.trackId) as Data;
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

  render(ctx: CanvasRenderingContext2D) {
    globals.frontendLocalState.addVisibleTrack(this.trackState.id);
    if (this.data() === undefined && !this.frontendOnly) {
      const {visibleWindowTime, visibleTimeScale} = globals.frontendLocalState;
      const startPx =
          Math.floor(visibleTimeScale.hpTimeToPx(visibleWindowTime.start));
      const endPx =
          Math.ceil(visibleTimeScale.hpTimeToPx(visibleWindowTime.end));
      checkerboard(ctx, this.getHeight(), startPx, endPx);
    } else {
      this.renderCanvas(ctx);
    }
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

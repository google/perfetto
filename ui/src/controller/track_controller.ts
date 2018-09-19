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

import {assertExists} from '../base/logging';
import {clearTrackDataRequest} from '../common/actions';
import {Registry} from '../common/registry';
import {TrackState} from '../common/state';

import {Controller} from './controller';
import {ControllerFactory} from './controller';
import {Engine} from './engine';
import {globals} from './globals';

// TrackController is a base class overridden by track implementations (e.g.,
// sched slices, nestable slices, counters).
export abstract class TrackController<Config = {}> extends Controller<'main'> {
  readonly trackId: string;
  readonly engine: Engine;

  constructor(args: TrackControllerArgs) {
    super('main');
    this.trackId = args.trackId;
    this.engine = args.engine;
  }

  // Must be overridden by the track implementation. Is invoked when the track
  // frontend runs out of cached data. The derived track controller is expected
  // to publish new track data in response to this call.
  abstract onBoundsChange(start: number, end: number, resolution: number): void;

  get trackState(): TrackState {
    return assertExists(globals.state.tracks[this.trackId]);
  }

  get config(): Config {
    return this.trackState.config as Config;
  }

  run() {
    const dataReq = this.trackState.dataReq;
    if (dataReq === undefined) return;
    globals.dispatch(clearTrackDataRequest(this.trackId));
    this.onBoundsChange(dataReq.start, dataReq.end, dataReq.resolution);
  }
}

export interface TrackControllerArgs {
  trackId: string;
  engine: Engine;
}

export interface TrackControllerFactory extends
    ControllerFactory<TrackControllerArgs> {
  kind: string;
}

export const trackControllerRegistry = new Registry<TrackControllerFactory>();

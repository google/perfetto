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

import {TrackState} from '../common/state';
import {Engine} from './engine';

export interface PublishFn { (data: {}): void; }

/**
 * This interface forces track implementations to have two static properties:
 * kind and a create function.
 */
export interface TrackControllerCreator {
  // Store the kind explicitly as a string as opposed to using class.name in
  // case we ever minify our code.
  readonly kind: string;

  create(config: TrackState, engine: Engine, publish: PublishFn):
      TrackController;
}

export abstract class TrackController {
  // TODO(hjd): Maybe this should be optional?
  abstract onBoundsChange(start: number, end: number): void;
}

// Re-export these so track implementors don't have to import from several
// files.
export {
  TrackState,
  Engine,
};

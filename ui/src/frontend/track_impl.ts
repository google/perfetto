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

import {TimeScale} from './time_scale';
import {VirtualCanvasContext} from './virtual_canvas_context';

/**
 * This interface forces track implementations to have two static properties:
 * type and a create function.
 *
 * Typescript does not have abstract static members, which is why this needs to
 * be in a seperate interface. We need the |create| method because the stored
 * value in the registry is an abstract class, and we cannot call 'new'
 * on an abstract class.
 */
export interface TrackCreator {
  // Store the type explicitly as a string as opposed to using class.name in
  // case we ever minify our code.
  readonly type: string;

  create(TrackState: TrackState): TrackImpl;
}

// TODO(dproy): TrackImpl is not a great name. We can change this to something
// better as we figure out the rest of the pieces of track rendering
// architecture.
/**
 * The abstract class that needs to be implemented by all tracks.
 */
export abstract class TrackImpl {
  constructor(protected trackState: TrackState) {}
  abstract draw(
      vCtx: VirtualCanvasContext, width: number, timeScale: TimeScale): void;
}

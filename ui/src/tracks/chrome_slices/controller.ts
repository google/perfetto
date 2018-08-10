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

import {assertExists} from '../../base/logging';
import {rawQueryResultIter} from '../../common/protos';
import {
  Engine,
  PublishFn,
  TrackController,
  TrackState
} from '../../controller/track_controller';
import {
  trackControllerRegistry
} from '../../controller/track_controller_registry';

import {ChromeSlice, TRACK_KIND} from './common';

// Need this because some values in query result is string|number.
function convertToNumber(x: string|number): number {
  // tslint:disable-next-line:ban Temporary. parseInt banned by style guide.
  return typeof x === 'number' ? x : parseInt(x, 10);
}

// TODO(hjd): Too much bolierplate here. Prehaps TrackController/Track
// should be an interface and we provide a TrackControllerBase/TrackBase
// you can inherit from which does the basic things.
class ChromeSliceTrackController extends TrackController {
  static readonly kind = TRACK_KIND;

  static create(config: TrackState, engine: Engine, publish: PublishFn):
      TrackController {
    return new ChromeSliceTrackController(
        engine,
        // TODO: Remove assertExists once we have typecheked kind specific
        // state.
        assertExists(config.utid),
        publish);
  }

  // TODO: This publish function should be better typed to only accept
  // CpuSliceTrackData. Perhaps we can do PublishFn<T>.
  private publish: PublishFn;

  constructor(
      private engine: Engine, private utid: number, publish: PublishFn) {
    super();
    this.publish = publish;
    this.init();
  }

  async init() {
    const query =
        `select ts, dur, name, cat, depth from slices where utid = ${
                                                                     this.utid
                                                                   };`;
    const rawResult = await this.engine.rawQuery({'sqlQuery': query});
    // TODO(dproy): Remove.
    const result = [...rawQueryResultIter(rawResult)];
    const slices: ChromeSlice[] = [];

    // TODO: We need better time origin handling.
    if (result.length === 0) return;
    const firstTimestamp = convertToNumber(result[0].ts);

    for (const row of result) {
      const start = convertToNumber(row.ts) - firstTimestamp;
      const end = start + convertToNumber(row.dur);
      slices.push({
        start,
        end,
        title: (row.name as string),
        category: (row.cat as string),
        depth: convertToNumber(row.depth)
      });
    }

    this.publish({slices});
  }

  onBoundsChange(_start: number, _end: number): void {
    // TODO: Implement.
  }
}

trackControllerRegistry.register(ChromeSliceTrackController);

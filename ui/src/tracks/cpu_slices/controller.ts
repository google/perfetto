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

import {TRACK_KIND} from './common';

// Need this because some values in query result is string|number.
function convertToNumber(x: string|number): number {
  // tslint:disable-next-line:ban Temporary. parseInt banned by style guide.
  return typeof x === 'number' ? x : parseInt(x, 10);
}

// TODO(hjd): Too much bolierplate here. Prehaps TrackController/Track
// should be an interface and we provide a TrackControllerBase/TrackBase
// you can inherit from which does the basic things.
class CpuSliceTrackController extends TrackController {
  static readonly kind = TRACK_KIND;

  static create(config: TrackState, engine: Engine, publish: PublishFn):
      TrackController {
    return new CpuSliceTrackController(config.cpu, engine, publish);
  }

  private cpu: number;
  private engine: Engine;
  // TODO: This publish function should be better typed to only accept
  // CpuSliceTrackData. Perhaps we can do PublishFn<T>.
  private publish: PublishFn;

  constructor(cpu: number, engine: Engine, publish: PublishFn) {
    super();
    this.cpu = cpu;
    this.engine = engine;
    this.publish = publish;
    this.init();
  }

  async init() {
    const query = `select * from sched where cpu = ${this.cpu} limit 1000;`;
    const rawResult = await this.engine.rawQuery({'sqlQuery': query});
    // TODO(hjd): Remove.
    const result = [...rawQueryResultIter(rawResult)];
    const slices = [];

    // Hacking time for now. http://bit.ly/2LNElLB
    // TODO: We're currently not setting the maxVisible window anywhere. Should
    // there even be a max visible window? Should it be the job of track
    // controllers to tell us what the max visible window is?
    if (result.length === 0) return;
    const firstTimestamp = convertToNumber(result[0].ts);

    for (const row of result) {
      const start = convertToNumber(row.ts) - firstTimestamp;
      const end = start + convertToNumber(row.dur);
      slices.push({start, end, title: 'Placeholder'});
    }
    this.publish({slices});
  }

  onBoundsChange(_start: number, _end: number): void {
    // TODO: Implement.
  }
}

trackControllerRegistry.register(CpuSliceTrackController);

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
  private publish: PublishFn;

  constructor(cpu: number, engine: Engine, publish: PublishFn) {
    super();
    this.cpu = cpu;
    this.engine = engine;
    this.publish = publish;
    this.init();
  }

  async init() {
    const query = `select * from sched where cpu = ${this.cpu};`;
    const rawResult = await this.engine.rawQuery({'sqlQuery': query});
    // TODO(hjd): Remove.
    const result = [...rawQueryResultIter(rawResult)];
    this.publish(result);
  }

  onBoundsChange(_start: number, _end: number): void {
    // TODO: Implement.
  }
}

trackControllerRegistry.register(CpuSliceTrackController);

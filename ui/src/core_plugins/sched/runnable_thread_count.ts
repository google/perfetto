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

import {
  BaseCounterTrack,
  CounterOptions,
} from '../../frontend/base_counter_track';
import {NewTrackArgs} from '../../frontend/track';

export class RunnableThreadCountTrack extends BaseCounterTrack {
  constructor(args: NewTrackArgs) {
    super(args);
  }

  protected getDefaultCounterOptions(): CounterOptions {
    const options = super.getDefaultCounterOptions();
    options.yRangeRounding = 'strict';
    options.yRange = 'viewport';
    return options;
  }

  async onInit() {
    await this.engine.query(
      `INCLUDE PERFETTO MODULE sched.thread_level_parallelism`,
    );
  }

  getSqlSource() {
    return `
      select
        ts,
        runnable_thread_count as value
      from sched_runnable_thread_count
    `;
  }
}

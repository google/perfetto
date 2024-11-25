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
} from '../../components/tracks/base_counter_track';
import {Trace} from '../../public/trace';

abstract class ThreadCountTrack extends BaseCounterTrack {
  constructor(trace: Trace, uri: string) {
    super(trace, uri);
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
}

export class RunnableThreadCountTrack extends ThreadCountTrack {
  getSqlSource() {
    return `
      select
        ts,
        runnable_thread_count as value
      from sched_runnable_thread_count
    `;
  }
}

export class UninterruptibleSleepThreadCountTrack extends ThreadCountTrack {
  getSqlSource() {
    return `
      select
        ts,
        uninterruptible_sleep_thread_count as value
      from sched_uninterruptible_sleep_thread_count
    `;
  }
}

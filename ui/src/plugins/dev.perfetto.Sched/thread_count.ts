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

import {CounterTrack} from '../../components/tracks/counter_track';
import {Trace} from '../../public/trace';

async function threadLevelParallelismInit(trace: Trace): Promise<void> {
  await trace.engine.query(
    `INCLUDE PERFETTO MODULE sched.thread_level_parallelism`,
  );
}

export class RunnableThreadCountTrack extends CounterTrack {
  constructor(trace: Trace, uri: string) {
    super({
      trace,
      uri,
      sqlSource: `select ts, runnable_thread_count as value from sched_runnable_thread_count`,
      yRangeRounding: 'strict',
      yRange: 'viewport',
      onInit: () => threadLevelParallelismInit(trace),
    });
  }
}

export class UninterruptibleSleepThreadCountTrack extends CounterTrack {
  constructor(trace: Trace, uri: string) {
    super({
      trace,
      uri,
      sqlSource: `select ts, uninterruptible_sleep_thread_count as value from sched_uninterruptible_sleep_thread_count`,
      yRangeRounding: 'strict',
      yRange: 'viewport',
      onInit: () => threadLevelParallelismInit(trace),
    });
  }
}

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

import {sqliteString} from '../../base/string_utils';
import {CounterTrack} from '../../components/tracks/counter_track';
import {Trace} from '../../public/trace';

export enum CPUType {
  Big = 'big',
  Mid = 'mid',
  Little = 'little',
}

export class ActiveCPUCountTrack extends CounterTrack {
  constructor(trackUri: string, trace: Trace, cpuType?: CPUType) {
    const sourceTable =
      cpuType === undefined
        ? 'sched_active_cpu_count'
        : `_active_cpu_count_for_cluster_type(${sqliteString(cpuType)})`;
    super({
      trace,
      uri: trackUri,
      sqlSource: `select ts, active_cpu_count as value from ${sourceTable}`,
      yRangeRounding: 'strict',
      yRange: 'viewport',
      onInit: async () => {
        await trace.engine.query(`
          INCLUDE PERFETTO MODULE sched.thread_level_parallelism;
          INCLUDE PERFETTO MODULE android.cpu.cluster_type;
        `);
      },
    });
  }
}

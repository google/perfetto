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

import m from 'mithril';
import {Icons} from '../../base/semantic_icons';
import {sqliteString} from '../../base/string_utils';
import {
  BaseCounterTrack,
  CounterOptions,
} from '../../frontend/base_counter_track';
import {TrackContext} from '../../public/track';
import {Button} from '../../widgets/button';
import {Trace} from '../../public/trace';

export enum CPUType {
  Big = 'big',
  Mid = 'mid',
  Little = 'little',
}

export class ActiveCPUCountTrack extends BaseCounterTrack {
  private readonly cpuType?: CPUType;

  constructor(ctx: TrackContext, trace: Trace, cpuType?: CPUType) {
    super({
      trace,
      uri: ctx.trackUri,
    });
    this.cpuType = cpuType;
  }

  getTrackShellButtons(): m.Children {
    return m(Button, {
      onclick: () => {
        this.trace.workspace.findTrackByUri(this.uri)?.remove();
      },
      icon: Icons.Close,
      title: 'Close',
      compact: true,
    });
  }

  protected getDefaultCounterOptions(): CounterOptions {
    const options = super.getDefaultCounterOptions();
    options.yRangeRounding = 'strict';
    options.yRange = 'viewport';
    return options;
  }

  async onInit() {
    await this.engine.query(`
      INCLUDE PERFETTO MODULE sched.thread_level_parallelism;
      INCLUDE PERFETTO MODULE android.cpu.cluster_type;
    `);
  }

  getSqlSource() {
    const sourceTable =
      this.cpuType === undefined
        ? 'sched_active_cpu_count'
        : `_active_cpu_count_for_cluster_type(${sqliteString(this.cpuType)})`;
    return `
      select
        ts,
        active_cpu_count as value
      from ${sourceTable}
    `;
  }
}

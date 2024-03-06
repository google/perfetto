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

import {NullDisposable} from '../../base/disposable';
import {sqliteString} from '../../base/string_utils';
import {uuidv4} from '../../base/uuid';
import {Actions} from '../../common/actions';
import {SCROLLING_TRACK_GROUP} from '../../common/state';
import {
  BaseCounterTrack,
  RenderOptions,
} from '../../frontend/base_counter_track';
import {CloseTrackButton} from '../../frontend/close_track_button';
import {globals} from '../../frontend/globals';
import {EngineProxy, PrimaryTrackSortKey, TrackContext} from '../../public';

export function addActiveCPUCountTrack(cpuType?: string) {
  const cpuTypeName = cpuType === undefined ? '' : ` ${cpuType} `;

  const key = uuidv4();

  globals.dispatchMultiple([
    Actions.addTrack({
      key,
      uri: ActiveCPUCountTrack.kind,
      name: `Active ${cpuTypeName}CPU count`,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
      params: {
        cpuType,
      },
    }),
    Actions.toggleTrackPinned({trackKey: key}),
  ]);
}

export interface ActiveCPUCountTrackConfig {
  cpuType?: string;
}

export class ActiveCPUCountTrack extends BaseCounterTrack {
  private config: ActiveCPUCountTrackConfig;

  static readonly kind = 'dev.perfetto.Sched.ActiveCPUCount';

  constructor(ctx: TrackContext, engine: EngineProxy) {
    super({
      engine,
      trackKey: ctx.trackKey,
    });

    // TODO(stevegolton): Validate params before type asserting.
    // TODO(stevegolton): Avoid just pushing this config up for some base
    // class to use. Be more explicit.
    this.config = ctx.params as ActiveCPUCountTrackConfig;
  }

  getTrackShellButtons(): m.Children {
    return [m(CloseTrackButton, {
      trackKey: this.trackKey,
    })];
  }

  protected getRenderOptions(): RenderOptions {
    return {
      yBoundaries: 'strict',
      yRange: 'viewport',
    };
  }

  async onInit() {
    await this.engine.query(
      `INCLUDE PERFETTO MODULE sched.thread_level_parallelism`);
    return new NullDisposable();
  }

  getSqlSource() {
    const sourceTable = this.config!.cpuType === undefined ?
      'sched_active_cpu_count' :
      `sched_active_cpu_count_for_core_type(${
        sqliteString(this.config!.cpuType)})`;
    return `
    select
      ts,
      active_cpu_count as value
    from ${sourceTable}
    `;
  }
}

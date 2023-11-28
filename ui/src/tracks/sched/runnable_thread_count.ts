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
import {uuidv4} from '../../base/uuid';
import {Actions} from '../../common/actions';
import {SCROLLING_TRACK_GROUP} from '../../common/state';
import {
  BaseCounterTrack,
  RenderOptions,
} from '../../frontend/base_counter_track';
import {CloseTrackButton} from '../../frontend/close_track_button';
import {globals} from '../../frontend/globals';
import {NewTrackArgs} from '../../frontend/track';
import {PrimaryTrackSortKey} from '../../public';

export function addRunnableThreadCountTrack() {
  const key = uuidv4();
  globals.dispatchMultiple([
    Actions.addTrack({
      key,
      uri: RunnableThreadCountTrack.kind,
      name: `Runnable thread count`,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
    }),
    Actions.toggleTrackPinned({trackKey: key}),
  ]);
}

export class RunnableThreadCountTrack extends BaseCounterTrack {
  static readonly kind = 'dev.perfetto.Sched.RunnableThreadCount';

  constructor(args: NewTrackArgs) {
    super(args);
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
    return `
    select
      ts,
      runnable_thread_count as value
    from sched_runnable_thread_count
    `;
  }
}

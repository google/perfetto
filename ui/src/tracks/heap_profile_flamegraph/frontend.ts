// Copyright (C) 2019 The Android Open Source Project
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

import {TrackState} from '../../common/state';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {Flamegraph} from '../../frontend/flamegraph';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {trackRegistry} from '../../frontend/track_registry';

import {
  Config,
  Data,
  HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND,
  HeapProfileFlamegraphKey
} from './common';

const MARGIN = 10;

export class HeapProfileFlamegraphTrack extends Track<Config, Data> {
  static readonly kind = HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND;
  private flamegraph: Flamegraph;

  static create(trackState: TrackState): HeapProfileFlamegraphTrack {
    return new HeapProfileFlamegraphTrack(trackState);
  }

  constructor(trackState: TrackState) {
    super(trackState);
    this.flamegraph = new Flamegraph(new Array());
  }

  data() {
    return globals.trackDataStore.get(HeapProfileFlamegraphKey) as Data;
  }

  private changeFlamegraphData() {
    const data = this.data();
    if (data === undefined) {
      this.flamegraph.updateDataIfChanged(new Array());
    } else {
      this.flamegraph.updateDataIfChanged(data.flamegraph);
    }
  }

  getHeight(): number {
    const data = this.data();
    if (data === undefined) {
      return 0;
    }
    this.changeFlamegraphData();
    const height = this.flamegraph.getHeight();
    return Math.max(height + MARGIN, super.getHeight());
  }

  getWidth(): number {
    const {visibleWindowTime, timeScale} = globals.frontendLocalState;
    const startPx = Math.floor(timeScale.timeToPx(visibleWindowTime.start));
    const endPx = Math.ceil(timeScale.timeToPx(visibleWindowTime.end));
    return endPx - startPx;
  }

  renderCanvas(ctx: CanvasRenderingContext2D) {
    const data = this.data();
    if (data !== undefined && data.start === -1) {
      const {visibleWindowTime, timeScale} = globals.frontendLocalState;
      checkerboardExcept(
          ctx,
          this.getHeight(),
          timeScale.timeToPx(visibleWindowTime.start),
          timeScale.timeToPx(visibleWindowTime.end),
          timeScale.timeToPx(data.start),
          timeScale.timeToPx(data.end));
      return;
    }
    this.changeFlamegraphData();
    this.flamegraph.draw(ctx, this.getWidth(), this.getHeight());
  }

  onMouseClick({x, y}: {x: number, y: number}): boolean {
    this.flamegraph.onMouseClick({x, y});
    return true;
  }

  onMouseMove({x, y}: {x: number, y: number}): boolean {
    this.flamegraph.onMouseMove({x, y});
    return true;
  }

  onMouseOut() {
    this.flamegraph.onMouseOut();
  }
}

trackRegistry.register(HeapProfileFlamegraphTrack);

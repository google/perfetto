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

import * as m from 'mithril';

import {Actions} from '../../common/actions';
import {TrackState} from '../../common/state';
import {checkerboardExcept} from '../../frontend/checkerboard';
import {Flamegraph} from '../../frontend/flamegraph';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {TrackButton, TrackButtonAttrs} from '../../frontend/track_panel';
import {trackRegistry} from '../../frontend/track_registry';

import {
  ALLOC_SPACE_MEMORY_ALLOCATED_KEY,
  Config,
  Data,
  HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND,
  HeapProfileFlamegraphKey,
  SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY
} from './common';

const MARGIN = 10;

export class HeapProfileFlamegraphTrack extends Track<Config, Data> {
  static readonly kind = HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND;
  private flamegraph: Flamegraph;
  private currentViewingOption = SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY;

  static create(trackState: TrackState): HeapProfileFlamegraphTrack {
    return new HeapProfileFlamegraphTrack(trackState);
  }

  constructor(trackState: TrackState) {
    super(trackState);
    this.flamegraph = new Flamegraph([]);
    this.flamegraph.enableThumbnail(this.config.isMinimized);
  }

  data() {
    return globals.trackDataStore.get(HeapProfileFlamegraphKey) as Data;
  }

  private changeFlamegraphData() {
    const data = this.data();
    if (data === undefined) {
      this.flamegraph.updateDataIfChanged([]);
    } else {
      this.flamegraph.updateDataIfChanged(
          data.flamegraph, data.clickedCallsite);
      if (data.viewingOption !== undefined) {
        this.currentViewingOption = data.viewingOption;
      }
    }
  }

  getHeight(): number {
    const data = this.data();
    if (data === undefined) {
      return 0;
    }
    if (this.config.isMinimized) {
      return super.getHeight();
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
    const unit =
        this.currentViewingOption === SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY ||
            this.currentViewingOption === ALLOC_SPACE_MEMORY_ALLOCATED_KEY ?
        'B' :
        '';
    this.flamegraph.draw(ctx, this.getWidth(), this.getHeight(), 0, 0, unit);
  }

  onMouseClick({x, y}: {x: number, y: number}): boolean {
    this.config.expandedId = this.flamegraph.onMouseClick({x, y});
    globals.dispatch(Actions.updateTrackConfig(
        {id: this.trackState.id, config: this.config}));
    return true;
  }

  onMouseMove({x, y}: {x: number, y: number}): boolean {
    this.flamegraph.onMouseMove({x, y});
    return true;
  }

  onMouseOut() {
    this.flamegraph.onMouseOut();
  }

  getTrackShellButtons(): Array<m.Vnode<TrackButtonAttrs>> {
    const buttons: Array<m.Vnode<TrackButtonAttrs>> = [];
    buttons.push(
        // Minimize button
        m(TrackButton, {
          action: () => {
            const newIsMinimized = !this.config.isMinimized;
            this.config.isMinimized = newIsMinimized;
            Actions.updateTrackConfig(
                {id: this.trackState.id, config: this.config});
            this.flamegraph.enableThumbnail(newIsMinimized);
            globals.rafScheduler.scheduleFullRedraw();
          },
          i: this.config.isMinimized ? 'expand_more' : 'expand_less',
          tooltip: this.config.isMinimized ? 'Maximize' : 'Minimize',
          selected: this.config.isMinimized,
        }));
    return buttons;
  }
}

trackRegistry.register(HeapProfileFlamegraphTrack);

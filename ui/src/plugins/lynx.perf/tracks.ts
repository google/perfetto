// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {duration, Time, time} from '../../base/time';
import {TrackRenderContext} from '../../public/track';
import {makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';
import {AppImpl} from '../../core/app_impl';
import {IssueRank, IssueSummary} from '../../lynx_perf/types';
import {CHEVRON_WIDTH_PX, LynxBaseTrack} from '../../lynx_perf/lynx_base_track';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from '../../lynx_perf/constants';

const MINOR_PERFORMANCE_ISSUE_COLOR = makeColorScheme(
  new HSLColor([45, 100, 51]),
);
const MODERATE_PERFORMANCE_ISSUE_COLOR = makeColorScheme(
  new HSLColor([35, 100, 51]),
);
const CRITICAL_PERFORMANCE_ISSUE_COLOR = makeColorScheme(
  new HSLColor([4, 90, 58]),
);

export class LynxPerfTrack extends LynxBaseTrack<IssueSummary[]> {
  getHeight(): number {
    return (
      SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.sliceHeight +
      2 * SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding
    );
  }

  async onBoundsChange(
    start: time,
    end: time,
    _resolution: duration,
  ): Promise<IssueSummary[]> {
    const issues = lynxPerfGlobals.state.issues.filter(
      (item) => item.ts >= start && item.ts <= end,
    );
    return issues;
  }

  render(ctx: TrackRenderContext): void {
    const data = this.fetcher.data;
    if (data === undefined) return;
    for (let i = 0; i < data.length; i++) {
      const posX = Time.fromRaw(BigInt(data[i].ts));
      const highlighted = data[i].id === this.hoveredSlice?.id;
      let colorSchema = MINOR_PERFORMANCE_ISSUE_COLOR;
      if (data[i].issueRank === IssueRank.MODERATE) {
        colorSchema = MODERATE_PERFORMANCE_ISSUE_COLOR;
      } else if (data[i].issueRank === IssueRank.CRITICAL) {
        colorSchema = CRITICAL_PERFORMANCE_ISSUE_COLOR;
      }
      ctx.ctx.fillStyle = highlighted
        ? colorSchema.variant.cssString
        : lynxPerfGlobals.shouldShowSlice(data[i].id)
          ? colorSchema.base.cssString
          : colorSchema.disabled.cssString;
      const xPx = ctx.timescale.timeToPx(posX);
      const yPx = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.padding;
      const height = SLICE_LAYOUT_FIT_CONTENT_DEFAULTS.sliceHeight;
      this.drawChevron(ctx.ctx, xPx, yPx, height);

      const selection = AppImpl.instance.trace?.selection.selection;
      const selectedId =
        selection &&
        selection.kind === 'track_event' &&
        selection.trackUri === this.uri
          ? selection.eventId
          : undefined;
      if (selectedId === undefined) {
        this.selectedSlice = undefined;
      }
      const selected = data[i].id === this.selectedSlice?.id;
      if (selected) {
        this.drawThickBorder(
          ctx,
          xPx - CHEVRON_WIDTH_PX / 2,
          yPx,
          CHEVRON_WIDTH_PX,
          height,
          colorSchema,
        );
      }
    }
  }

  changeTrackUri() {
    this.uri = (this.selectedSlice as IssueSummary)?.trackUri;
  }
}

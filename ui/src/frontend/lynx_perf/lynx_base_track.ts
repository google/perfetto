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

import m from 'mithril';
import {Duration, duration, Time, time} from '../../base/time';
import {
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
} from '../../public/track';
import {Point2D} from '../../base/geom';
import {BaseSlice} from './types';
import {Trace} from '../../public/trace';
import {TimelineFetcher} from '../../components/tracks/track_helper';
import {ColorScheme} from '../../base/color_scheme';

export const CHEVRON_WIDTH_PX = 10;

/**
 * Base track class for Lynx performance visualization
 * Provides common functionality for all Lynx-specific track implementations
 * including hover/selection handling, tooltips, and basic rendering utilities.
 */
export abstract class LynxBaseTrack<T extends BaseSlice[]>
  implements TrackRenderer
{
  /**
   * Data fetcher for timeline slices
   * Automatically handles windowing and resolution-based fetching
   */
  protected fetcher = new TimelineFetcher<T>(this.onBoundsChange.bind(this));

  // Track interaction state
  protected hoverPos?: Point2D;
  protected hoveredSlice?: BaseSlice;
  protected selectedSlice?: BaseSlice;
  protected hoverTooltip?: string;
  protected trace: Trace;
  protected uri: string;

  constructor(trace: Trace, uri: string) {
    this.trace = trace;
    this.uri = uri;
  }

  abstract render(ctx: TrackRenderContext): void;
  abstract getHeight(): number;

  // currently this method is only for plugin LYNX_ISSUES_PLUGIN_ID because it contains different track uri in one track.
  changeTrackUri(): void {}

  /**
   * Updates track data when visible window or resolution changes
   * @param ctx - Rendering context with current view parameters
   */
  async onUpdate({
    visibleWindow,
    resolution,
  }: TrackRenderContext): Promise<void> {
    await this.fetcher.requestData(visibleWindow.toTimeSpan(), resolution);
  }

  /**
   * Cleans up resources when track is destroyed
   */
  async onDestroy(): Promise<void> {
    this.fetcher[Symbol.dispose]();
  }

  /**
   * Handles mouse click events on the track
   * @param event - Mouse event details
   * @returns True if event was handled, false otherwise
   */
  onMouseClick(event: TrackMouseEvent): boolean {
    const slice = this.findSlice(event);
    this.selectedSlice = slice;
    if (slice === undefined) {
      return false;
    }
    this.changeTrackUri();
    this.trace.selection.selectTrackEvent(this.uri, slice.id);
    return true;
  }

  /**
   * Finds slice under mouse cursor
   * @param event - Mouse event details
   * @returns Slice under cursor or undefined if none found
   */
  findSlice(event: TrackMouseEvent): BaseSlice | undefined {
    const data = this.fetcher.data;
    if (data === undefined) return undefined;
    for (let i = 0; i < data.length; i++) {
      const posX = event.timescale.timeToPx(Time.fromRaw(BigInt(data[i].ts)));
      const dur = data[i].dur;
      if (dur === undefined) {
        // instant
        if (
          event.x >= posX - CHEVRON_WIDTH_PX &&
          event.x <= posX + CHEVRON_WIDTH_PX
        ) {
          return data[i];
        }
      } else {
        const durX = event.timescale.durationToPx(
          Duration.fromRaw(BigInt(dur)),
        );
        if (event.x >= posX && event.x <= posX + durX) {
          return data[i];
        }
      }
    }
    return undefined;
  }

  onMouseMove(event: TrackMouseEvent): void {
    const {x, y} = event;
    this.hoverPos = {x, y};
    this.updateHoveredSlice(this.findSlice(event));
  }

  onMouseOut(): void {
    this.updateHoveredSlice(undefined);
  }

  private updateHoveredSlice(slice?: BaseSlice): void {
    const lastHoveredSlice = this.hoveredSlice;
    this.hoveredSlice = slice;

    if (slice === lastHoveredSlice) return;

    if (this.hoveredSlice === undefined) {
      this.trace.timeline.highlightedSliceId = undefined;
    } else {
      this.trace.timeline.highlightedSliceId = this.hoveredSlice.id;
    }

    if (this.hoveredSlice === undefined) {
      this.hoverTooltip = undefined;
      this.hoverPos = undefined;
    } else {
      if (slice) {
        this.hoverTooltip = slice.description;
      }
    }
  }

  abstract onBoundsChange(
    start: time,
    end: time,
    resolution: duration,
  ): Promise<T>;

  /**
   * Renders tooltip for hovered slice
   * @returns Mithril virtual DOM element or undefined if no tooltip needed
   */
  renderTooltip(): m.Children {
    if (
      this.hoveredSlice !== undefined &&
      this.hoverTooltip &&
      this.hoverTooltip.length > 0 &&
      this.hoverPos !== undefined
    ) {
      return m('', this.hoverTooltip);
    }
    return undefined;
  }

  /**
   * Draws a chevron marker for instant events
   * @param ctx - Canvas rendering context
   * @param x - Horizontal position
   * @param y - Vertical position
   * @param h - Height of chevron
   */
  protected drawChevron(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    h: number,
  ) {
    // Draw an upward facing chevrons, in order: A, B, C, D, and back to A.
    //      . (x, y)
    //      A
    //     ###
    //    ##C##
    //   ##   ##
    //  D       B
    //
    const HALF_CHEVRON_WIDTH_PX = CHEVRON_WIDTH_PX / 2;
    const fillStyle = ctx.fillStyle;
    ctx.beginPath();
    ctx.moveTo(x, y); // A.
    ctx.lineTo(x + HALF_CHEVRON_WIDTH_PX, y + h); // B.
    ctx.lineTo(x, y + h - HALF_CHEVRON_WIDTH_PX); // C.
    ctx.lineTo(x - HALF_CHEVRON_WIDTH_PX, y + h); // D.
    ctx.lineTo(x, y); // Back to A.
    ctx.closePath();
    ctx.fill();
    ctx.fillStyle = fillStyle;
  }

  /**
   * Draws a thick border around a rectangular area
   * @param ctx - Track rendering context
   * @param x - Horizontal position
   * @param y - Vertical position
   * @param width - Border width
   * @param height - Border height
   * @param colorSchema - Color scheme to use
   */
  protected drawThickBorder(
    ctx: TrackRenderContext,
    x: number,
    y: number,
    width: number,
    height: number,
    colorSchema: ColorScheme,
  ) {
    const renderCtx = ctx.ctx;
    renderCtx.strokeStyle = colorSchema.base.setHSL({s: 100, l: 10}).cssString;
    renderCtx.beginPath();
    const THICKNESS = 3;
    renderCtx.lineWidth = THICKNESS;
    renderCtx.strokeRect(x, y - THICKNESS / 2, width, height + THICKNESS);
    renderCtx.closePath();
  }
}

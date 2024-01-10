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

import {duration, Time, time} from '../base/time';
import {Actions} from '../common/actions';
import {cropText, drawIncompleteSlice} from '../common/canvas_utils';
import {getColorForSlice} from '../common/colorizer';
import {HighPrecisionTime} from '../common/high_precision_time';
import {TrackData} from '../common/track_data';
import {TimelineFetcher} from '../common/track_helper';
import {SliceRect, Track} from '../public';

import {CROP_INCOMPLETE_SLICE_FLAG} from './base_slice_track';
import {checkerboardExcept} from './checkerboard';
import {globals} from './globals';
import {PanelSize} from './panel';

export const SLICE_TRACK_KIND = 'ChromeSliceTrack';
const SLICE_HEIGHT = 18;
const TRACK_PADDING = 2;
const CHEVRON_WIDTH_PX = 10;
const HALF_CHEVRON_WIDTH_PX = CHEVRON_WIDTH_PX / 2;
const INCOMPLETE_SLICE_WIDTH_PX = 20;

export interface SliceData extends TrackData {
  // Slices are stored in a columnar fashion.
  strings: string[];
  sliceIds: Float64Array;
  starts: BigInt64Array;
  ends: BigInt64Array;
  depths: Uint16Array;
  titles: Uint16Array;   // Index into strings.
  colors?: Uint16Array;  // Index into strings.
  isInstant: Uint16Array;
  isIncomplete: Uint16Array;
  cpuTimeRatio?: Float64Array;
}

// Track base class which handles rendering slices in a generic way.
// This is the old way of rendering slices - i.e. "track v1" format  - and
// exists as a patch to allow old tracks to be converted to controller-less
// tracks before they are ported to v2.
// Slice tracks should extend this class and implement the abstract methods,
// notably onBoundsChange().
// Note: This class is deprecated and should not be used for new tracks. Use
// |BaseSliceTrack| instead.
export abstract class SliceTrackLEGACY implements Track {
  private fetcher = new TimelineFetcher(this.onBoundsChange.bind(this));

  constructor(
      private maxDepth: number, protected trackKey: string,
      private tableName: string, private namespace?: string) {}

  async onUpdate(): Promise<void> {
    await this.fetcher.requestDataForCurrentTime();
  }

  async onDestroy(): Promise<void> {
    this.fetcher.dispose();
  }

  abstract onBoundsChange(start: time, end: time, resolution: duration):
      Promise<SliceData>;

  protected namespaceTable(tableName: string = this.tableName): string {
    if (this.namespace) {
      return this.namespace + '_' + tableName;
    } else {
      return tableName;
    }
  }

  private hoveredTitleId = -1;

  // Font used to render the slice name on the current track.
  protected getFont() {
    return '12px Roboto Condensed';
  }

  render(ctx: CanvasRenderingContext2D, size: PanelSize): void {
    // TODO: fonts and colors should come from the CSS and not hardcoded here.
    const data = this.fetcher.data;
    if (data === undefined) return;  // Can't possibly draw anything.

    const {visibleTimeSpan, visibleTimeScale} = globals.timeline;

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
        ctx,
        this.getHeight(),
        0,
        size.width,
        visibleTimeScale.timeToPx(data.start),
        visibleTimeScale.timeToPx(data.end),
    );

    ctx.textAlign = 'center';

    // measuretext is expensive so we only use it once.
    const charWidth = ctx.measureText('ACBDLqsdfg').width / 10;

    // The draw of the rect on the selected slice must happen after the other
    // drawings, otherwise it would result under another rect.
    let drawRectOnSelected = () => {};


    for (let i = 0; i < data.starts.length; i++) {
      const tStart = Time.fromRaw(data.starts[i]);
      let tEnd = Time.fromRaw(data.ends[i]);
      const depth = data.depths[i];
      const titleId = data.titles[i];
      const sliceId = data.sliceIds[i];
      const isInstant = data.isInstant[i];
      const isIncomplete = data.isIncomplete[i];
      const title = data.strings[titleId];
      const colorOverride = data.colors && data.strings[data.colors[i]];
      if (isIncomplete) {  // incomplete slice
        // TODO(stevegolton): This isn't exactly equivalent, ideally we should
        // choose tEnd once we've converted to screen space coords.
        tEnd = this.getEndTimeIfInComplete(tStart);
      }

      if (!visibleTimeSpan.intersects(tStart, tEnd)) {
        continue;
      }

      const pxEnd = size.width;
      const left = Math.max(visibleTimeScale.timeToPx(tStart), 0);
      const right = Math.min(visibleTimeScale.timeToPx(tEnd), pxEnd);

      const rect = {
        left,
        width: Math.max(right - left, 1),
        top: TRACK_PADDING + depth * SLICE_HEIGHT,
        height: SLICE_HEIGHT,
      };

      const currentSelection = globals.state.currentSelection;
      const isSelected = currentSelection &&
          currentSelection.kind === 'CHROME_SLICE' &&
          currentSelection.id !== undefined && currentSelection.id === sliceId;

      const highlighted = titleId === this.hoveredTitleId ||
          globals.state.highlightedSliceId === sliceId;

      const hasFocus = highlighted || isSelected;
      const colorScheme = getColorForSlice(title);
      const colorObj = hasFocus ? colorScheme.variant : colorScheme.base;
      const textColor =
          hasFocus ? colorScheme.textVariant : colorScheme.textBase;

      let color: string;
      if (colorOverride === undefined) {
        color = colorObj.cssString;
      } else {
        color = colorOverride;
      }
      ctx.fillStyle = color;

      // We draw instant events as upward facing chevrons starting at A:
      //     A
      //    ###
      //   ##C##
      //  ##   ##
      // D       B
      // Then B, C, D and back to A:
      if (isInstant) {
        if (isSelected) {
          drawRectOnSelected = () => {
            ctx.save();
            ctx.translate(rect.left, rect.top);

            // Draw a rectangle around the selected slice
            ctx.strokeStyle = colorObj.setHSL({s: 100, l: 10}).cssString;
            ctx.beginPath();
            ctx.lineWidth = 3;
            ctx.strokeRect(
                -HALF_CHEVRON_WIDTH_PX, 0, CHEVRON_WIDTH_PX, SLICE_HEIGHT);
            ctx.closePath();

            // Draw inner chevron as interior
            ctx.fillStyle = color;
            this.drawChevron(ctx);

            ctx.restore();
          };
        } else {
          ctx.save();
          ctx.translate(rect.left, rect.top);
          this.drawChevron(ctx);
          ctx.restore();
        }
        continue;
      }

      if (isIncomplete && rect.width > SLICE_HEIGHT / 4) {
        drawIncompleteSlice(
            ctx,
            rect.left,
            rect.top,
            rect.width,
            SLICE_HEIGHT,
            !CROP_INCOMPLETE_SLICE_FLAG.get());
      } else if (
          data.cpuTimeRatio !== undefined && data.cpuTimeRatio[i] < 1 - 1e-9) {
        // We draw two rectangles, representing the ratio between wall time and
        // time spent on cpu.
        const cpuTimeRatio = data.cpuTimeRatio![i];
        const firstPartWidth = rect.width * cpuTimeRatio;
        const secondPartWidth = rect.width * (1 - cpuTimeRatio);
        ctx.fillRect(rect.left, rect.top, rect.width, SLICE_HEIGHT);
        ctx.fillStyle = '#FFFFFF50';
        ctx.fillRect(
            rect.left + firstPartWidth,
            rect.top,
            secondPartWidth,
            SLICE_HEIGHT);
      } else {
        ctx.fillRect(rect.left, rect.top, rect.width, SLICE_HEIGHT);
      }

      // Selected case
      if (isSelected) {
        drawRectOnSelected = () => {
          ctx.strokeStyle = colorObj.setHSL({s: 100, l: 10}).cssString;
          ctx.beginPath();
          ctx.lineWidth = 3;
          ctx.strokeRect(
              rect.left, rect.top - 1.5, rect.width, SLICE_HEIGHT + 3);
          ctx.closePath();
        };
      }

      // Don't render text when we have less than 5px to play with.
      if (rect.width >= 5) {
        ctx.fillStyle = textColor.cssString;
        const displayText = cropText(title, charWidth, rect.width);
        const rectXCenter = rect.left + rect.width / 2;
        ctx.textBaseline = 'middle';
        ctx.font = this.getFont();
        ctx.fillText(displayText, rectXCenter, rect.top + SLICE_HEIGHT / 2);
      }
    }
    drawRectOnSelected();
  }

  drawChevron(ctx: CanvasRenderingContext2D) {
    // Draw a chevron at a fixed location and size. Should be used with
    // ctx.translate and ctx.scale to alter location and size.
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.lineTo(HALF_CHEVRON_WIDTH_PX, SLICE_HEIGHT);
    ctx.lineTo(0, SLICE_HEIGHT - HALF_CHEVRON_WIDTH_PX);
    ctx.lineTo(-HALF_CHEVRON_WIDTH_PX, SLICE_HEIGHT);
    ctx.lineTo(0, 0);
    ctx.fill();
  }

  getSliceIndex({x, y}: {x: number, y: number}): number|void {
    const data = this.fetcher.data;
    if (data === undefined) return;
    const {
      visibleTimeScale: timeScale,
    } = globals.timeline;
    if (y < TRACK_PADDING) return;
    const instantWidthTime = timeScale.pxDeltaToDuration(HALF_CHEVRON_WIDTH_PX);
    const t = timeScale.pxToHpTime(x);
    const depth = Math.floor((y - TRACK_PADDING) / SLICE_HEIGHT);

    for (let i = 0; i < data.starts.length; i++) {
      if (depth !== data.depths[i]) {
        continue;
      }
      const start = Time.fromRaw(data.starts[i]);
      const tStart = HighPrecisionTime.fromTime(start);
      if (data.isInstant[i]) {
        if (tStart.sub(t).abs().lt(instantWidthTime)) {
          return i;
        }
      } else {
        const end = Time.fromRaw(data.ends[i]);
        let tEnd = HighPrecisionTime.fromTime(end);
        if (data.isIncomplete[i]) {
          const endTime = this.getEndTimeIfInComplete(start);
          tEnd = HighPrecisionTime.fromTime(endTime);
        }
        if (tStart.lte(t) && t.lte(tEnd)) {
          return i;
        }
      }
    }
  }

  getEndTimeIfInComplete(start: time): time {
    const {visibleTimeScale, visibleWindowTime} = globals.timeline;

    let end = visibleWindowTime.end.toTime('ceil');
    if (CROP_INCOMPLETE_SLICE_FLAG.get()) {
      const widthTime =
          visibleTimeScale.pxDeltaToDuration(INCOMPLETE_SLICE_WIDTH_PX)
              .toTime();
      end = Time.add(start, widthTime);
    }

    return end;
  }

  onMouseMove({x, y}: {x: number, y: number}) {
    this.hoveredTitleId = -1;
    globals.dispatch(Actions.setHighlightedSliceId({sliceId: -1}));
    const sliceIndex = this.getSliceIndex({x, y});
    if (sliceIndex === undefined) return;
    const data = this.fetcher.data;
    if (data === undefined) return;
    this.hoveredTitleId = data.titles[sliceIndex];
    const sliceId = data.sliceIds[sliceIndex];
    globals.dispatch(Actions.setHighlightedSliceId({sliceId}));
  }

  onMouseOut() {
    this.hoveredTitleId = -1;
    globals.dispatch(Actions.setHighlightedSliceId({sliceId: -1}));
  }

  onMouseClick({x, y}: {x: number, y: number}): boolean {
    const sliceIndex = this.getSliceIndex({x, y});
    if (sliceIndex === undefined) return false;
    const data = this.fetcher.data;
    if (data === undefined) return false;
    const sliceId = data.sliceIds[sliceIndex];
    if (sliceId !== undefined && sliceId !== -1) {
      globals.makeSelection(Actions.selectChromeSlice({
        id: sliceId,
        trackKey: this.trackKey,
        table: this.namespace,
      }));
      return true;
    }
    return false;
  }

  getHeight() {
    return SLICE_HEIGHT * (this.maxDepth + 1) + 2 * TRACK_PADDING;
  }

  getSliceRect(tStart: time, tEnd: time, depth: number): SliceRect|undefined {
    const {
      windowSpan,
      visibleTimeScale,
      visibleTimeSpan,
    } = globals.timeline;

    const pxEnd = windowSpan.end;
    const left = Math.max(visibleTimeScale.timeToPx(tStart), 0);
    const right = Math.min(visibleTimeScale.timeToPx(tEnd), pxEnd);

    const visible = visibleTimeSpan.intersects(tStart, tEnd);

    return {
      left,
      width: Math.max(right - left, 1),
      top: TRACK_PADDING + depth * SLICE_HEIGHT,
      height: SLICE_HEIGHT,
      visible,
    };
  }
}

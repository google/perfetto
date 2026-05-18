// Copyright (C) 2024 The Android Open Source Project
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

import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {QuerySlot, SerialTaskQueue} from '../../base/query_slot';
import {ScreenshotDetailsPanel} from './screenshot_panel';
import {Spinner} from '../../widgets/spinner';
import type {
  TrackRenderer,
  TrackMouseEvent,
  TrackRenderContext,
  SnapPoint,
} from '../../public/track';
import type {TrackEventSelection} from '../../public/selection';
import type {TimeScale} from '../../base/time_scale';
import type {time} from '../../base/time';
import m from 'mithril';

class ScreenshotsTrack implements TrackRenderer {
  private sliceTrack: SliceTrack<{
    id: number;
    ts: bigint;
    dur: bigint;
    name: string;
  }>;
  private readonly queue = new SerialTaskQueue();
  private readonly metadataSlot = new QuerySlot<
    Array<{id: number; ts: bigint}>
  >(this.queue);
  private currentScreenshots?: Array<{id: number; ts: bigint}>;
  private currentScreenshotId?: number;
  private lastRenderedScreenshotId?: number;
  private imageDataCache = new Map<number, string>();
  private isLoadingImage = false;

  constructor(
    private readonly trace: Trace,
    uri: string,
  ) {
    this.sliceTrack = SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
        },
        src: 'android_screenshots',
      }),
      detailsPanel: () => {
        return new ScreenshotDetailsPanel(trace.engine);
      },
    });
  }

  render(trackCtx: TrackRenderContext): void {
    this.sliceTrack.render(trackCtx);
    this.useData(trackCtx);
  }

  private useData(trackCtx: TrackRenderContext) {
    const visibleSpan = trackCtx.visibleWindow.toTimeSpan();
    const start = visibleSpan.start;
    const end = visibleSpan.end;

    const dataset = this.sliceTrack.getDataset();
    if (dataset === undefined) return;

    const {data} = this.metadataSlot.use({
      key: {start, end},
      queryFn: async () => {
        const result = await this.trace.engine.query(`
          with all_screenshots as (
            select id, ts
            from (${dataset.query()})
          )
          select id, ts from (
            select id, ts from all_screenshots
            where ts < ${start}
            order by ts desc
            limit 1
          )
          union all
          select id, ts from all_screenshots
          where ts >= ${start} and ts <= ${end}
          order by ts
        `);
        const screenshots: Array<{id: number; ts: bigint}> = [];
        const it = result.iter({id: NUM, ts: LONG});
        for (; it.valid(); it.next()) {
          screenshots.push({id: it.id, ts: it.ts});
        }
        return screenshots;
      },
    });

    this.currentScreenshots = data;
  }

  getHeight(): number {
    return this.sliceTrack.getHeight();
  }

  getSliceVerticalBounds(depth: number) {
    return this.sliceTrack.getSliceVerticalBounds?.(depth);
  }

  getTrackShellButtons() {
    return this.sliceTrack.getTrackShellButtons?.();
  }

  onMouseClick(event: TrackMouseEvent): boolean {
    return this.sliceTrack.onMouseClick?.(event) ?? false;
  }

  onMouseOut(): void {
    this.sliceTrack.onMouseOut?.();
    this.currentScreenshotId = undefined;
  }

  onMouseMove(event: TrackMouseEvent): void {
    this.sliceTrack.onMouseMove?.(event);

    const time = event.timescale.pxToHpTime(event.x).toTime();
    const screenshot = this.currentScreenshots
      ? findMostRecentScreenshot(this.currentScreenshots, time)
      : undefined;
    if (screenshot) {
      this.currentScreenshotId = screenshot.id;
      if (!this.imageDataCache.has(screenshot.id) && !this.isLoadingImage) {
        this.loadScreenshotImage(screenshot.id);
      }
    } else {
      this.currentScreenshotId = undefined;
    }
  }

  private async loadScreenshotImage(id: number) {
    this.isLoadingImage = true;
    try {
      const result = await this.trace.engine.query(`
        select extract_arg(arg_set_id, 'screenshot.jpg_image') as image_data
        from slice
        where id = ${id}
      `);
      const row = result.firstRow({image_data: STR});
      const base64Image = row.image_data;
      this.imageDataCache.set(id, base64Image);

      // Limit cache size to 10
      if (this.imageDataCache.size > 10) {
        const firstKey = this.imageDataCache.keys().next().value;
        if (firstKey !== undefined) {
          this.imageDataCache.delete(firstKey);
        }
      }

      this.trace.raf.scheduleFullRedraw();
    } finally {
      this.isLoadingImage = false;
    }
  }

  private getCachedImage(id: number): string | undefined {
    const value = this.imageDataCache.get(id);
    if (value !== undefined) {
      this.imageDataCache.delete(id);
      this.imageDataCache.set(id, value); // Move to end
    }
    return value;
  }

  renderTooltip(): m.Children {
    // We conciously don't render the base SliceTrack tooltip, as it can
    // shift the screenshot around. The sliceTrack tooltip doesn't provide
    // any additional useful information for the screenshot track.
    if (this.currentScreenshots === undefined) {
      return [m('div', 'Loading screenshot metadata...')];
    }

    if (this.currentScreenshotId !== undefined) {
      const imageData = this.getCachedImage(this.currentScreenshotId);
      if (imageData) {
        this.lastRenderedScreenshotId = this.currentScreenshotId;
        return [
          m(
            'div',
            m('img.pf-screenshot-tooltip__img', {
              src: 'data:image/png;base64, ' + imageData,
            }),
          ),
        ];
      } else if (this.isLoadingImage) {
        const lastImageData =
          this.lastRenderedScreenshotId !== undefined
            ? this.imageDataCache.get(this.lastRenderedScreenshotId)
            : undefined;

        if (lastImageData) {
          return [
            m(
              'div',
              {style: {position: 'relative'}},
              m('img.pf-screenshot-tooltip__img', {
                src: 'data:image/png;base64, ' + lastImageData,
                style: {opacity: '0.5'},
              }),
              m(
                'div',
                {
                  style: {
                    position: 'absolute',
                    top: '50%',
                    left: '50%',
                    transform: 'translate(-50%, -50%)',
                  },
                },
                m(Spinner),
              ),
            ),
          ];
        } else {
          return [m('div', 'Loading screenshot...')];
        }
      }
    }

    return undefined;
  }

  getDataset() {
    return this.sliceTrack.getDataset?.();
  }

  getSelectionDetails(eventId: number) {
    return this.sliceTrack.getSelectionDetails?.(eventId);
  }

  detailsPanel(sel: TrackEventSelection) {
    return this.sliceTrack.detailsPanel?.(sel);
  }

  getSnapPoint(
    targetTime: time,
    thresholdPx: number,
    timescale: TimeScale,
  ): SnapPoint | undefined {
    return this.sliceTrack.getSnapPoint?.(targetTime, thresholdPx, timescale);
  }
}

export function findMostRecentScreenshot(
  screenshots: Array<{id: number; ts: bigint}>,
  ts: bigint,
): {id: number; ts: bigint} | undefined {
  // Binary search
  let l = 0;
  let r = screenshots.length - 1;
  let ans = -1;
  while (l <= r) {
    const mid = Math.floor((l + r) / 2);
    if (screenshots[mid].ts <= ts) {
      ans = mid;
      l = mid + 1;
    } else {
      r = mid - 1;
    }
  }
  return ans !== -1 ? screenshots[ans] : undefined;
}

export function createScreenshotsTrack(trace: Trace, uri: string) {
  return new ScreenshotsTrack(trace, uri);
}

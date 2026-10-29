// Copyright (C) 2026 The Android Open Source Project
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

import {Time} from '../../base/time';
import {colorForCpu} from '../../components/colorizer';
import {DEFAULT_MINIMAP_HEIGHT_PX} from '../../core/minimap_manager';
import type {TraceImpl} from '../../core/trace_impl';
import type {MinimapMode, MinimapRenderContext} from '../../public/minimap';

// The default minimap mode: an overview of activity across the trace, from
// the rows of the highest priority minimap content provider.
export class ActivityMinimapMode implements MinimapMode {
  readonly id = 'dev.perfetto.Timeline#activity';
  readonly name = 'Activity';
  readonly priority = 0;
  readonly heightPx = DEFAULT_MINIMAP_HEIGHT_PX;

  constructor(private readonly trace: TraceImpl) {}

  render({ctx, timescale, rect}: MinimapRenderContext) {
    const rows = this.trace.minimap.getLoad();
    if (rows === undefined) return;
    const rowHeight = rect.height / rows.length;
    rows.forEach((row, i) => {
      const top = Math.round(rect.top + i * rowHeight);
      const bottom = Math.round(rect.top + (i + 1) * rowHeight);
      ctx.fillStyle = colorForCpu(i).setHSL({s: 50}).cssString;
      for (const cell of row) {
        const x = Math.round(timescale.timeToPx(cell.ts));
        const xEnd = Math.round(
          timescale.timeToPx(Time.fromRaw(cell.ts + cell.dur)),
        );
        ctx.globalAlpha = cell.load;
        ctx.fillRect(x, top, xEnd - x, bottom - top);
      }
    });
  }
}

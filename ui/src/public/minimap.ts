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

import type m from 'mithril';
import type {Rect2D} from '../base/geom';
import type {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import type {duration, time} from '../base/time';
import type {TimeScale} from '../base/time_scale';

export interface MinimapCell {
  readonly ts: time;
  readonly dur: duration;
  readonly load: number;
}

export type MinimapRow = MinimapCell[];

export interface MinimapContentProvider {
  readonly priority: number;
  getData(
    timeSpan: HighPrecisionTimeSpan,
    resolution: duration,
  ): Promise<MinimapRow[]>;
}

export interface MinimapRenderContext {
  readonly ctx: CanvasRenderingContext2D;
  // Maps the whole trace onto the horizontal extent of the minimap.
  readonly timescale: TimeScale;
  // The content area below the time axis. Drawing is clipped to it.
  readonly rect: Rect2D;
}

/**
 * A minimap visualization, such as the built-in activity overview or a
 * filmstrip of screen recordings. When more than one mode is registered, the
 * minimap gets a menu to switch between them.
 */
export interface MinimapMode {
  // Unique identifier, e.g. 'dev.perfetto.VideoFrames#display0'.
  readonly id: string;

  // Name shown in the minimap menu.
  readonly name: string;

  // The mode with the highest priority is shown by default. The built-in
  // activity overview has priority 0.
  readonly priority: number;

  // Default height of the minimap (including the time axis) in pixels.
  readonly heightPx?: number;

  render(ctx: MinimapRenderContext): void;

  // Optional tooltip shown while hovering the minimap at `ts`.
  renderTooltip?(ts: time): m.Children;

  // Optional handler for clicks (as opposed to drags) on the minimap.
  onClick?(ts: time): void;
}

export interface MinimapManager {
  registerContentProvider(x: MinimapContentProvider): void;
  registerMode(mode: MinimapMode): void;
}

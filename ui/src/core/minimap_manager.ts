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

import {assertFalse} from '../base/assert';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import type {time} from '../base/time';
import {calculateResolution} from '../base/resolution';
import type {
  MinimapContentProvider,
  MinimapManager,
  MinimapMode,
  MinimapRow,
} from '../public/minimap';

export const DEFAULT_MINIMAP_HEIGHT_PX = 69;
export const MIN_MINIMAP_HEIGHT_PX = 40;
export const MAX_MINIMAP_HEIGHT_PX = 400;

export class MinimapManagerImpl implements MinimapManager {
  private readonly contentProviders: MinimapContentProvider[] = [];
  private readonly _modes: MinimapMode[] = [];
  private rows?: MinimapRow[];
  private selectedModeId?: string;
  private userHeightPx?: number;

  getLoad() {
    return this.rows;
  }

  registerContentProvider(x: MinimapContentProvider): void {
    this.contentProviders.push(x);

    // Sort the highest priority content provider first.
    this.contentProviders.sort((a, b) => b.priority - a.priority);
  }

  registerMode(mode: MinimapMode): void {
    assertFalse(
      this._modes.some((m) => m.id === mode.id),
      `Minimap mode ${mode.id} is already registered`,
    );
    this._modes.push(mode);
    // Highest priority first. The sort is stable, so ties keep registration
    // order.
    this._modes.sort((a, b) => b.priority - a.priority);
  }

  getContentProvider(): MinimapContentProvider | undefined {
    return this.contentProviders[0];
  }

  get modes(): ReadonlyArray<MinimapMode> {
    return this._modes;
  }

  // The selected mode, or the highest priority one if none is selected.
  get activeMode(): MinimapMode | undefined {
    return (
      this._modes.find((mode) => mode.id === this.selectedModeId) ??
      this._modes[0]
    );
  }

  selectMode(id: string) {
    this.selectedModeId = id;
  }

  get heightPx(): number {
    return (
      this.userHeightPx ??
      this.activeMode?.heightPx ??
      DEFAULT_MINIMAP_HEIGHT_PX
    );
  }

  set heightPx(px: number) {
    this.userHeightPx = Math.min(
      MAX_MINIMAP_HEIGHT_PX,
      Math.max(MIN_MINIMAP_HEIGHT_PX, px),
    );
  }

  // Restores the default height of the active mode.
  resetHeight() {
    this.userHeightPx = undefined;
  }

  async load(start: time, end: time) {
    const provider = this.getContentProvider();
    if (!provider) {
      return;
    }

    // Find the trace bounds and split it up into a resolution
    const timeSpan = HighPrecisionTimeSpan.fromTime(start, end);
    const resolution = calculateResolution(timeSpan, 100);
    if (!resolution.ok) {
      return;
    }

    this.rows = await provider.getData(timeSpan, resolution.value);
  }
}

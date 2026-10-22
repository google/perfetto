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

import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import type {time} from '../base/time';
import {calculateResolution} from '../base/resolution';
import type {
  MinimapContentProvider,
  MinimapManager,
  MinimapMode,
  MinimapRow,
} from '../public/minimap';

// The built-in activity overview, rendered from the content providers' rows.
export const ACTIVITY_MODE = {id: 'activity', name: 'Activity', priority: 0};

export const DEFAULT_MINIMAP_HEIGHT_PX = 70;
const MIN_MINIMAP_HEIGHT_PX = 40;
const MAX_MINIMAP_HEIGHT_PX = 400;

export class MinimapManagerImpl implements MinimapManager {
  private readonly contentProviders: MinimapContentProvider[] = [];
  private readonly customModes: MinimapMode[] = [];
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
    this.customModes.push(mode);
  }

  getContentProvider(): MinimapContentProvider | undefined {
    return this.contentProviders[0];
  }

  // All selectable modes, highest priority first. Ties keep registration order.
  get modes(): ReadonlyArray<{readonly id: string; readonly name: string}> {
    return [...this.customModes, ACTIVITY_MODE].sort(
      (a, b) => b.priority - a.priority,
    );
  }

  get activeModeId(): string {
    const ids = this.modes.map((mode) => mode.id);
    if (
      this.selectedModeId !== undefined &&
      ids.includes(this.selectedModeId)
    ) {
      return this.selectedModeId;
    }
    return ids[0];
  }

  set activeModeId(id: string) {
    this.selectedModeId = id;
  }

  // The active custom mode, or undefined when showing the activity overview.
  get activeMode(): MinimapMode | undefined {
    const id = this.activeModeId;
    return this.customModes.find((mode) => mode.id === id);
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

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
  MinimapRow,
} from '../public/minimap';

export class MinimapManagerImpl implements MinimapManager {
  private readonly contentProviders: MinimapContentProvider[] = [];
  private rows?: MinimapRow[];
  // The latest load. Loads run one at a time, in order: providers may reuse
  // tables, e.g. Sched's, and an older load must not overwrite newer rows.
  private lastLoad: Promise<void> = Promise.resolve();

  getLoad() {
    return this.rows;
  }

  registerContentProvider(x: MinimapContentProvider): void {
    this.contentProviders.push(x);

    // Sort the highest priority content provider first.
    this.contentProviders.sort((a, b) => b.priority - a.priority);
  }

  getContentProvider(): MinimapContentProvider | undefined {
    return this.contentProviders[0];
  }

  load(start: time, end: time): Promise<void> {
    // After the previous load, whether it failed or not.
    const load = Promise.allSettled([this.lastLoad]).then(() =>
      this.loadNow(start, end),
    );
    this.lastLoad = load;
    return load;
  }

  private async loadNow(start: time, end: time) {
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

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
import {time} from '../base/time';
import {calculateResolution} from '../frontend/timeline_page/resolution';
import {
  MinimapContentProvider,
  MinimapManager,
  MinimapRow,
} from '../public/minimap';

export class MinimapManagerImpl implements MinimapManager {
  private readonly contentProviders: MinimapContentProvider[] = [];
  private rows?: MinimapRow[];

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

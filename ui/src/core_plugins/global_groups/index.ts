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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';

// This plugin is responsible for organizing all the global tracks.
export default class implements PerfettoPlugin {
  static readonly id = 'perfetto.GlobalGroups';
  async onTraceLoad(trace: Trace): Promise<void> {
    trace.onTraceReady.addListener(() => {
      // Move groups underneath tracks
      Array.from(trace.workspace.children)
        .sort((a, b) => {
          // Get the index in the order array
          const indexA = a.hasChildren ? 1 : 0;
          const indexB = b.hasChildren ? 1 : 0;
          return indexA - indexB;
        })
        .forEach((n) => trace.workspace.addChildLast(n));

      // If there is only one group, expand it
      const rootLevelChildren = trace.workspace.children;
      if (rootLevelChildren.length === 1 && rootLevelChildren[0].hasChildren) {
        rootLevelChildren[0].expand();
      }
    });
  }
}

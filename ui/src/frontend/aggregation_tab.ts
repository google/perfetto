// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {Disposable} from '../base/disposable';
import {AggregationPanel} from './aggregation_panel';
import {globals} from './globals';

export class AggregationsTabs implements Disposable {
  private tabs = [
    {
      type: 'cpu_aggregation',
      title: 'CPU by thread',
    },
    {
      type: 'thread_state_aggregation',
      title: 'Thread States',
    },
    {
      type: 'cpu_by_process_aggregation',
      title: 'CPU by process',
    },
    {
      type: 'slice_aggregation',
      title: 'Slices',
    },
    {
      type: 'counter_aggregation',
      title: 'Counters',
    },
    {
      type: 'frame_aggregation',
      title: 'Frames',
    },
  ];

  constructor() {
    for (const {type, title} of this.tabs) {
      globals.tabManager.registerTab({
        uri: `aggregationTab#${type}`,
        isEphemeral: false,
        content: {
          getTitle: () => `Aggregation: ${title}`,
          render: () => {
            const data = globals.aggregateDataStore.get(type);
            return m(AggregationPanel, {kind: type, data});
          },
        },
      });
    }
  }

  dispose(): void {
    for (const {type} of this.tabs) {
      globals.tabManager.unregisterTab(`aggregationTab#${type}`);
    }
  }
}

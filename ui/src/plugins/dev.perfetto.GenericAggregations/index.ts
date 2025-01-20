// Copyright (C) 2021 The Android Open Source Project
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
import {CounterSelectionAggregator} from './counter_selection_aggregator';
import {SliceSelectionAggregator} from './slice_selection_aggregator';

/**
 * This plugin adds the generic aggregations for slice tracks and counter
 * tracks.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GenericAggregations';

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.selection.registerAreaSelectionAggregator(
      new CounterSelectionAggregator(),
    );

    ctx.selection.registerAreaSelectionAggregator(
      new SliceSelectionAggregator(),
    );
  }
}

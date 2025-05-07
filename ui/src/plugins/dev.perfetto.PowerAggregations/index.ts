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

import {createAggregationToTabAdaptor} from '../../components/aggregation_adapter';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {PowerCounterSelectionAggregator} from './power_counter_selection_aggregator';

/**
 * This plugin adds the aggregations for power rail counter tracks.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.PowerAggregations';

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.selection.registerAreaSelectionTab(
      createAggregationToTabAdaptor(
        ctx,
        new PowerCounterSelectionAggregator(),
        200,
      ),
    );
  }
}

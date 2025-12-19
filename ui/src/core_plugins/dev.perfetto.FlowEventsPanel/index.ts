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

import m from 'mithril';
import {PerfettoPlugin} from '../../public/plugin';
import {TraceImpl} from '../../core/trace_impl';
import {FlowEventsAreaSelectedPanel} from './flow_events_panel';

/**
 * This plugin is a core plugin because for now flows are stored in the core and
 * not exposed to plugins. In the future once we normalize how flows should
 * work, we can reassess this and move it into wherever it needs to be.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.FlowEventsPanel';

  async onTraceLoad(trace: TraceImpl): Promise<void> {
    // This type assertion is allowed because we're a core plugin.
    trace.selection.registerAreaSelectionTab({
      id: 'flow_events',
      name: 'Flow Events',
      priority: -100,
      render() {
        if (trace.flows.selectedFlows.length > 0) {
          return {
            isLoading: false,
            content: m(FlowEventsAreaSelectedPanel, {trace}),
          };
        } else {
          return undefined;
        }
      },
    });
  }
}

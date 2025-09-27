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
import {Trace} from '../../public/trace';
import {PprofPage, PprofPageState} from './pprof_page';
import {NUM} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.PprofPage';

  // Persistent state for the lifecycle of a single trace
  private state: PprofPageState = {
    selectedScope: '',
    selectedMetric: '',
    availableScopes: [],
    availableMetrics: [],
  };

  onStateUpdate = (
    update: PprofPageState | ((current: PprofPageState) => PprofPageState),
  ) => {
    if (typeof update === 'function') {
      this.state = update(this.state);
    } else {
      this.state = update;
    }
    m.redraw();
  };

  async onTraceLoad(trace: Trace): Promise<void> {
    // Check if the trace contains pprof data
    const hasAggregateData = await this.checkForAggregateData(trace);

    if (hasAggregateData) {
      trace.pages.registerPage({
        route: '/pprof',
        render: () => {
          return m(PprofPage, {
            trace,
            state: this.state,
            onStateUpdate: this.onStateUpdate,
          });
        },
      });

      trace.sidebar.addMenuItem({
        section: 'current_trace',
        text: 'Pprof Profiles',
        href: '#!/pprof',
        icon: 'local_fire_department',
      });
    }
  }

  private async checkForAggregateData(trace: Trace): Promise<boolean> {
    try {
      const result = await trace.engine.query(`
        SELECT COUNT(*) as count
        FROM __intrinsic_aggregate_profile
        LIMIT 1
      `);
      return result.firstRow({count: NUM}).count > 0;
    } catch {
      return false;
    }
  }
}

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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {V8RuntimeCallStatsTab} from './v8_runtime_call_stats_tab';
import {V8SourcesTab} from './v8_sources_tab';

const RCS_TAB_URI = 'org.chromium.V8#V8RuntimeCallStatsTab';
const SOURCES_TAB_URI = 'org.chromium.V8#V8SourcesTab';

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.V8';
  static readonly description = 'V8 Plugin';

  async onTraceLoad(trace: Trace) {
    if (await this.hasAnyRCSData(trace)) {
      this.enableRCSTab(trace);
    }
    if (await this.hasAnyV8SourceData(trace)) {
      this.enableV8SourcesTab(trace);
    }
  }

  private async hasAnyRCSData(trace: Trace) {
    const hasRCSData = await trace.engine.query(
      `SELECT 1 FROM args WHERE key GLOB 'debug.runtime-call-stats.*' LIMIT 1`,
    );
    return hasRCSData.numRows() > 0;
  }

  private enableRCSTab(trace: Trace) {
    trace.tabs.registerTab({
      uri: RCS_TAB_URI,
      content: new V8RuntimeCallStatsTab(trace),
      isEphemeral: false,
    });
    trace.tabs.addDefaultTab(RCS_TAB_URI);
  }

  private async hasAnyV8SourceData(trace: Trace) {
    const hasV8Data = await trace.engine.query(
      `INCLUDE PERFETTO MODULE v8.jit;
        SELECT 1 FROM v8_js_script LIMIT 1`,
    );
    return hasV8Data.numRows() > 0;
  }

  private enableV8SourcesTab(trace: Trace) {
    trace.tabs.registerTab({
      uri: SOURCES_TAB_URI,
      content: new V8SourcesTab(trace),
      isEphemeral: false,
    });
    trace.tabs.addDefaultTab(SOURCES_TAB_URI);
  }
}

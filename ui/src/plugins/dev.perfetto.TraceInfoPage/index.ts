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
import {TraceInfoPage} from './trace_info_page';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TraceInfoPage';

  async onTraceLoad(trace: Trace): Promise<void> {
    // Create helper functions for accessing metadata
    await trace.engine.query(`
      CREATE PERFETTO FUNCTION _metadata_str(key STRING)
      RETURNS STRING AS
      SELECT str_value FROM metadata WHERE name = $key;

      CREATE PERFETTO FUNCTION _metadata_int(key STRING)
      RETURNS LONG AS
      SELECT int_value FROM metadata WHERE name = $key;
    `);

    trace.pages.registerPage({
      route: '/info',
      render: (subpage) => m(TraceInfoPage, {trace, subpage: subpage}),
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Overview',
      href: '#!/info',
      icon: 'info',
      sortOrder: 15,
    });
  }
}

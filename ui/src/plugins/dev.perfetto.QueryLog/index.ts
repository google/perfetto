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

function formatMillis(millis: number) {
  return millis.toFixed(1);
}

export default class QueryLogPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryLog';
  async onTraceLoad(trace: Trace): Promise<void> {
    const tabUri = `${QueryLogPlugin.id}#QueryLogTab`;

    trace.commands.registerCommand({
      id: `dev.perfetto.ShowQueryLogTab`,
      name: 'Show query log tab',
      callback: () => {
        trace.tabs.showTab(tabUri);
      },
    });

    trace.tabs.registerTab({
      isEphemeral: false,
      uri: tabUri,
      content: {
        getTitle() {
          return 'Query Log';
        },
        render() {
          // Show the logs in reverse order
          const queryLog = Array.from(trace.engine.queryLog).reverse();
          return m(
            'table.pf-query-log-table',
            m(
              'tr',
              m('th', 'Query'),
              m('th', 'Tag'),
              m('th', 'Status'),
              m('th', 'Start time (ms)'),
              m('th', 'Duration (ms)'),
            ),
            queryLog.map((ql) =>
              m(
                'tr',
                m('td', m('pre', ql.query.trim())),
                m('td', ql.tag),
                m(
                  'td',
                  ql.success === undefined
                    ? 'Running...'
                    : ql.success
                      ? 'Completed'
                      : 'Failed',
                ),
                m('td', formatMillis(ql.startTime)),
                m(
                  'td',
                  ql.endTime === undefined
                    ? '...'
                    : formatMillis(ql.endTime - ql.startTime),
                ),
              ),
            ),
          );
        },
      },
    });
  }
}

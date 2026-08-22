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
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {QueryTab} from './query_log';
import './styles.scss';

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
          return m(QueryTab, {trace});
        },
      },
    });
  }
}

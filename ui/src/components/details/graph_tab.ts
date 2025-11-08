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

import m from 'mithril';
import {Trace} from '../../public/trace';
import {Tab} from '../../public/tab';
import {DetailsShell} from '../../widgets/details_shell';
import {Spinner} from '../../widgets/spinner';
import {raf} from '../../core/raf_scheduler';
import {addEphemeralTab} from './add_ephemeral_tab';
import {STR} from '../../trace_processor/query_result';

export interface GraphTabConfig {
  sqlQuery: string;
}

export function addGraphTab(trace: Trace, config: GraphTabConfig) {
  addEphemeralTab(trace, 'graph', new GraphTab(trace, config));
}

export class GraphTab implements Tab {
  constructor(
    private trace: Trace,
    private config: GraphTabConfig,
  ) {}

  private data?: Array<{source: string; dest: string}>;
  private loading = true;
  private error?: string;

  async loadData() {
    try {
      const result = await this.trace.engine.query(this.config.sqlQuery);

      const data = [];
      for (
        const it = result.iter({source: STR, dest: STR});
        it.valid();
        it.next()
      ) {
        data.push({source: it.source, dest: it.dest});
      }

      this.data = data;
      this.loading = false;
    } catch (e) {
      this.error = String(e);
      this.loading = false;
    }
    raf.scheduleFullRedraw();
  }

  render(): m.Children {
    if (this.loading && !this.data) {
      this.loadData();
      return m(
        DetailsShell,
        {
          title: 'Graph',
        },
        m('.pf-graph-container', {style: 'padding: 20px;'}, m(Spinner)),
      );
    }

    if (this.error) {
      return m(
        DetailsShell,
        {
          title: 'Graph',
        },
        m(
          '.pf-graph-container',
          {style: 'padding: 20px;'},
          m('.pf-error', `Error loading graph data: ${this.error}`),
        ),
      );
    }

    return m(
      DetailsShell,
      {
        title: 'Graph',
      },
      m(
        '.pf-graph-container',
        {style: 'padding: 20px; overflow: auto;'},
        m('p', `Showing ${this.data?.length ?? 0} edges`),
        m(
          'table.pf-table',
          {style: 'width: 100%; margin-top: 10px;'},
          m(
            'thead',
            m('tr', m('th', 'Source'), m('th', '→'), m('th', 'Destination')),
          ),
          m(
            'tbody',
            this.data?.map((edge) =>
              m(
                'tr',
                m('td', edge.source),
                m('td', {style: 'text-align: center;'}, '→'),
                m('td', edge.dest),
              ),
            ),
          ),
        ),
      ),
    );
  }

  getTitle(): string {
    return `Graph`;
  }

  isLoading(): boolean {
    return this.loading;
  }
}

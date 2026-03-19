// Copyright (C) 2026 The Android Open Source Project
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
import {S, activeCluster} from './state';
import {ImportPanel} from './components/import_panel';
import {ClusterTabs} from './components/cluster_tabs';
import {TraceList} from './components/trace_list';

export interface QuantizedSlicesPageAttrs {
  readonly subpage?: string;
}

export class QuantizedSlicesPage
  implements m.ClassComponent<QuantizedSlicesPageAttrs>
{
  view(_vnode: m.Vnode<QuantizedSlicesPageAttrs>): m.Children {
    const cl = activeCluster();
    const hasClusters = S.clusters.length > 0;

    return m('.qs-page', [
      // Tooltip element for canvas hover
      m('.qs-tooltip'),

      // Header
      m('.qs-header', [
        m('h2.qs-title', 'Quantized Slices'),
        hasClusters
          ? m(
              'span.qs-header-stats',
              `${S.clusters.reduce((n, c) => n + c.traces.length, 0)} traces in ${S.clusters.length} tab${S.clusters.length !== 1 ? 's' : ''}`,
            )
          : null,
      ]),

      // Import panel
      m(ImportPanel),

      // Cluster tabs + trace list
      hasClusters
        ? [
            m(ClusterTabs, {
              contentForCluster: () => (cl ? m(TraceList) : null),
            }),
          ]
        : null,
    ]);
  }
}

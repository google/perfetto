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
import type {Trace} from '../../../public/trace';
import type {TreeExplorerQueryMetric} from '../../../components/tree_explorer_fetcher';
import {TreeExplorerPanel} from '../../../components/tree_explorer_panel';
import {
  createDefaultTreeExplorerState,
  type TreeExplorerState,
} from '../../../widgets/tree_explorer';
import {Stack} from '../../../widgets/stack';
import {EmptyState} from '../../../widgets/empty_state';
import {DetailsShell} from '../../../widgets/details_shell';

import {
  buildOomeCallstackMetrics,
  renderOomeDetails,
} from '../../dev.perfetto.HeapProfile/oome_callstack_common';
import type {OomeData} from '../types';
import {getOome} from '../queries';
import type {HeapDump} from '../queries';

import {AsyncMemo} from '../../../base/async_memo';

interface CallstackViewAttrs {
  readonly trace: Trace;
  readonly dump: HeapDump;
  readonly state: TreeExplorerState | undefined;
  readonly onStateChange: (state: TreeExplorerState) => void;
}

export class CallstackView implements m.ClassComponent<CallstackViewAttrs> {
  private readonly oomeMemo = new AsyncMemo<OomeData | null>();
  private cachedMetrics?: ReadonlyArray<TreeExplorerQueryMetric>;
  private cachedKey?: string;

  onremove() {
    this.oomeMemo.dispose();
  }

  view({attrs}: m.Vnode<CallstackViewAttrs>) {
    const {trace, dump} = attrs;

    const oomeResult = this.oomeMemo.use({
      key: dump,
      compute: async () => {
        try {
          return (await getOome(trace.engine, dump)) ?? null;
        } catch {
          return null;
        }
      },
    });

    if (oomeResult.data === undefined) {
      return m(
        DetailsShell,
        {title: 'Callstack', fillHeight: true, className: 'pf-hde-tab--padded'},
        m(TreeExplorerPanel, {
          trace: attrs.trace,
          metrics: undefined,
          state: attrs.state,
          onStateChange: attrs.onStateChange,
        }),
      );
    }

    if (oomeResult.data === null) {
      return m(
        DetailsShell,
        {title: 'Callstack', fillHeight: true, className: 'pf-hde-tab--padded'},
        m(
          EmptyState,
          {
            icon: 'data_array',
            title: 'Data is not available in this trace',
            fillHeight: true,
          },
          m(
            'div',
            'Callstacks in heap dumps are only available in Perfetto heap dumps collected on OutOfMemoryError and in recent versions of Android',
          ),
        ),
      );
    }

    const oomeData = oomeResult.data;
    const upid = oomeData.upid;
    const ts = oomeData.ts;
    const key = `${upid}:${ts}`;
    if (this.cachedMetrics === undefined || key !== this.cachedKey) {
      this.cachedMetrics = buildOomeCallstackMetrics(ts);
      this.cachedKey = key;
    }
    const metrics = this.cachedMetrics;

    let state = attrs.state;
    if (state === undefined) {
      state = createDefaultTreeExplorerState(metrics);
      attrs.onStateChange(state);
    }

    return m(
      DetailsShell,
      {title: 'Callstack', fillHeight: true, className: 'pf-hde-tab--padded'},
      m(
        Stack,
        {orientation: 'vertical'},
        renderOomeDetails(oomeData.details),
        m(TreeExplorerPanel, {
          trace: attrs.trace,
          metrics,
          state,
          onStateChange: attrs.onStateChange,
        }),
      ),
    );
  }
}

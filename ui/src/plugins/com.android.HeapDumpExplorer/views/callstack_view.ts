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
import {TreeExplorerFetcher} from '../../../components/tree_explorer_fetcher';
import {Memo} from '../../../base/memo';
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
import * as queries from '../queries';
import {AsyncMemo} from '../../../base/async_memo';

interface CallstackViewAttrs {
  readonly trace: Trace;
  readonly dump: queries.HeapDump;
  readonly state: TreeExplorerState | undefined;
  readonly onStateChange: (state: TreeExplorerState) => void;
}

export class CallstackView implements m.ClassComponent<CallstackViewAttrs> {
  // The fetcher is created for the dump it serves and disposed by the memo when
  // the dump changes or when this view is removed.
  private readonly oomeDataMemo = new AsyncMemo<OomeData | undefined>();
  private readonly fetcherMemo = new Memo<TreeExplorerFetcher>();

  view({attrs}: m.Vnode<CallstackViewAttrs>) {
    const {isPending, data: oomeData} = this.oomeDataMemo.use({
      key: {dump: attrs.dump},
      compute: () => queries.getOome(attrs.trace.engine, attrs.dump),
    });

    if (isPending) {
      return m(DetailsShell, {
        title: 'Callstack',
        fillHeight: true,
        className: 'pf-hde-tab--padded',
      });
    }

    if (!oomeData) {
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

    const upid = oomeData.upid;
    const ts = oomeData.ts;
    const fetcher = this.fetcherMemo.use({
      key: {upid, ts},
      compute: () =>
        new TreeExplorerFetcher(attrs.trace, buildOomeCallstackMetrics(ts)),
    });
    const metrics = fetcher.metrics;

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
          fetcher,
          state,
          onStateChange: attrs.onStateChange,
        }),
      ),
    );
  }

  onremove(): void {
    this.fetcherMemo.dispose();
    this.oomeDataMemo.dispose();
  }
}

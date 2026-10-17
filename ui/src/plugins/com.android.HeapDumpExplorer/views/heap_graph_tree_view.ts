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
import type {time} from '../../../base/time';
import {AsyncMemo} from '../../../base/async_memo';
import {Memo} from '../../../base/memo';
import type {Trace} from '../../../public/trace';
import {
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from '../../../components/tree_explorer_fetcher';
import {
  TreeExplorerPanel,
  type TreeExplorerPanelAttrs,
} from '../../../components/tree_explorer_panel';
import {
  createDefaultTreeExplorerState,
  type TreeExplorerState,
} from '../../../widgets/tree_explorer';
import {
  isHeapGraphIncomplete,
  incompleteFlamegraphModal,
} from '../../dev.perfetto.HeapProfile/incomplete_flamegraph';
import {
  mergedClassNodeIcon,
  mergedClassNodeTooltipRenderer,
} from '../../../components/merged_class_flamegraph_hooks';
import {MERGED_CLASS_PROPERTY} from '../../dev.perfetto.HeapProfile/merged_class_sql';

export interface HeapGraphTreeViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly ts: time;
  readonly state: TreeExplorerState | undefined;
  readonly onStateChange: (state: TreeExplorerState) => void;
  // The metrics of the dump. Only called when the dump changes.
  readonly buildMetrics: (
    upid: number,
    ts: time,
  ) => ReadonlyArray<TreeExplorerQueryMetric>;
  readonly extraDownloadItems?: TreeExplorerPanelAttrs['extraDownloadItems'];
}

// Flag potentially merged classes (see merged_class_sql.ts).
const MERGED_CLASS_NODE_ICON = mergedClassNodeIcon(MERGED_CLASS_PROPERTY);
const MERGED_CLASS_NODE_TOOLTIP = mergedClassNodeTooltipRenderer(
  MERGED_CLASS_PROPERTY,
);

// A flamegraph of a heap graph tree of the active dump.
export function HeapGraphTreeView(): m.Component<HeapGraphTreeViewAttrs> {
  // The fetcher is created for the dump it serves and disposed by the memo when
  // the dump changes or when this view is removed.
  const fetcherMemo = new Memo<TreeExplorerFetcher>();

  // Mirrors dev.perfetto.HeapProfile: if the heap graph is incomplete we gate
  // the flamegraph behind a dismissible warning modal. Keyed by dump so it
  // re-arms when the dump changes; the check runs (and the modal is shown) only
  // when this view is rendered, i.e. when its tab is active.
  const incompleteSlot = new AsyncMemo<{
    isIncomplete: boolean;
    dismissed: boolean;
  }>();

  return {
    view({attrs}) {
      const fetcher = fetcherMemo.use({
        key: {upid: attrs.upid, ts: attrs.ts},
        compute: () =>
          new TreeExplorerFetcher(
            attrs.trace,
            attrs.buildMetrics(attrs.upid, attrs.ts),
          ),
      });

      const incomplete = incompleteSlot.use({
        key: {upid: attrs.upid, ts: attrs.ts},
        compute: async () => ({
          isIncomplete: await isHeapGraphIncomplete(attrs.trace),
          dismissed: false,
        }),
      }).data;

      // First render or after a dump-change reset: create a default
      // state so the panel renders meaningfully on the same frame.
      let state = attrs.state;
      if (state === undefined) {
        state = createDefaultTreeExplorerState(fetcher.metrics);
        attrs.onStateChange(state);
      }

      return [
        incomplete !== undefined &&
          incomplete.isIncomplete &&
          !incomplete.dismissed &&
          incompleteFlamegraphModal(attrs.trace, () => {
            incomplete.dismissed = true;
          }),
        m(TreeExplorerPanel, {
          fetcher,
          state,
          onStateChange: attrs.onStateChange,
          nodeIcon: MERGED_CLASS_NODE_ICON,
          renderNodeTooltip: MERGED_CLASS_NODE_TOOLTIP,
          extraDownloadItems: attrs.extraDownloadItems,
        }),
      ];
    },
    onremove() {
      fetcherMemo.dispose();
      incompleteSlot.dispose();
    },
  };
}

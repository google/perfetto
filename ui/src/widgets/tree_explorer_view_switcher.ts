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
import {TabStrip} from './tab_strip';
import type {TreeExplorerDisplayMode, TreeExplorerState} from './tree_explorer';

export interface TreeExplorerViewSwitcherAttrs {
  readonly state: TreeExplorerState;
  readonly onStateChange: (state: TreeExplorerState) => void;
  readonly className?: string;
}

// Switches a tree explorer between its display modes (flamegraph, call tree,
// flat function table) by rewriting `state.displayMode`.
//
// This is a free-standing widget on purpose: it only needs the caller-owned
// state, so hosts with their own chrome (a DetailsShell header, an existing
// tab strip) can place it wherever fits best. TreeExplorerPanel renders one
// above the filter bar by default; hosts that want a different placement
// compose TreeExplorerFilterBar and the views directly instead of using the
// panel.
export class TreeExplorerViewSwitcher implements m.ClassComponent<TreeExplorerViewSwitcherAttrs> {
  view({attrs}: m.CVnode<TreeExplorerViewSwitcherAttrs>): m.Children {
    return m(TabStrip, {
      className: attrs.className,
      tabs: [
        {key: 'flamegraph', title: 'Flamegraph'},
        {key: 'tree', title: 'Call Tree'},
        {key: 'flat', title: 'Functions'},
      ],
      currentTabKey: attrs.state.displayMode,
      onTabChange: (key: string) => {
        attrs.onStateChange({
          ...attrs.state,
          displayMode: key as TreeExplorerDisplayMode,
        });
      },
    });
  }
}

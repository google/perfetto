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
import {SidePanelManagerImpl} from '../core/side_panel_manager';
import {SplitPanel} from '../widgets/split_panel';
import {Tabs, TabsTab} from '../widgets/tabs';

const DEFAULT_WIDTH_PX = 400;
const MIN_WIDTH_PX = 200;

interface SidePanelContainerAttrs {
  readonly sidePanelMgr: SidePanelManagerImpl;
  readonly pageContent: m.Children;
}

export function SidePanelContainer(
  _vnode: m.Vnode<SidePanelContainerAttrs>,
): m.Component<SidePanelContainerAttrs> {
  let widthPx = DEFAULT_WIDTH_PX;

  return {
    view(vnode) {
      const {sidePanelMgr, pageContent} = vnode.attrs;
      const openTabs = sidePanelMgr.openTabs;
      const hasOpenTabs = openTabs.length > 0 && sidePanelMgr.visible;

      if (!hasOpenTabs) {
        return pageContent;
      }

      const tabs: TabsTab[] = [];
      for (const uri of openTabs) {
        const desc = sidePanelMgr.resolveTab(uri);
        if (!desc) continue;
        tabs.push({
          key: uri,
          title: desc.title,
          content: desc.render(),
          closeButton: false,
          leftIcon: desc.icon,
        });
      }

      const sidePanel = m(
        '.pf-side-panel',
        m(Tabs, {
          tabs,
          activeTabKey: sidePanelMgr.currentTabUri,
          className: 'pf-side-panel__tabs',
          onTabChange: (key) => sidePanelMgr.showTab(key),
          onTabClose: (key) => sidePanelMgr.hideTab(key),
        }),
      );

      return m(SplitPanel, {
        direction: 'horizontal',
        controlledPanel: 'second',
        split: {pixels: widthPx},
        minSize: MIN_WIDTH_PX,
        firstPanel: pageContent,
        secondPanel: sidePanel,
        onResize: (size: number) => {
          widthPx = size;
        },
      });
    },
  };
}

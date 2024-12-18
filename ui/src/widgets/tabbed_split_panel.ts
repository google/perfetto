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
import {SplitPanelDrawerVisibility, SplitPanel} from './split_panel';
import {Gate} from '../base/mithril_utils';
import {Button} from './button';

export interface Tab {
  // A unique key for this tab.
  readonly key: string;

  // The title of the tab to show on the tab strip.
  readonly title: string;

  // The content of this tab to show on the tab drawer.
  readonly content: m.Children;

  // Whether we have a close button or not on the tab handle.
  readonly hasCloseButton?: boolean;

  // Called when the tab is closed via its close button or via middle click on
  // the tab handle.
  onClose?(): void;
}

export interface TabbedSplitPanelAttrs {
  // The list of tabs.
  readonly tabs: ReadonlyArray<Tab>;

  // The key of the currently showing tab.
  readonly currentTabKey?: string;

  // Content to put to the left of the tabs on the split handle.
  readonly leftHandleContent?: m.Children;

  // Whether the drawer is currently visible or not (when in controlled mode).
  readonly visibility?: SplitPanelDrawerVisibility;

  // Extra classes applied to the root element.
  readonly className?: string;

  // What height should the drawer be initially?
  readonly startingHeight?: number;

  // Called when the active tab is changed.
  onTabChange?(key: string): void;

  // Called when the drawer visibility is changed.
  onVisibilityChange?(visibility: SplitPanelDrawerVisibility): void;
}

/**
 * An extended SplitPanel with tabs which are displayed in a tab strip along the
 * handle, and the active tab's content in shown in the drawer.
 */
export class TabbedSplitPanel
  implements m.ClassComponent<TabbedSplitPanelAttrs>
{
  private currentTabKey?: string;

  view({attrs, children}: m.CVnode<TabbedSplitPanelAttrs>) {
    const {
      currentTabKey = this.currentTabKey,
      onTabChange,
      leftHandleContent: leftContent,
      startingHeight,
      tabs,
      visibility,
      onVisibilityChange,
      className,
    } = attrs;
    return m(
      SplitPanel,
      {
        className,
        drawerContent: tabs.map((tab) =>
          m(Gate, {open: tab.key === currentTabKey}, tab.content),
        ),
        startingHeight,
        visibility,
        onVisibilityChange,
        handleContent: m(
          '.pf-tab-handle',
          leftContent,
          m(
            '.pf-tab-handle__tabs',
            tabs.map((tab) => {
              const {key, hasCloseButton = false} = tab;
              return m(
                '.pf-tab-handle__tab',
                {
                  active: currentTabKey === key,
                  key,
                  // Click tab to switch to it
                  onclick: () => {
                    onTabChange?.(tab.key);
                    this.currentTabKey = tab.key;
                  },
                  // Middle click to close
                  onauxclick: () => {
                    tab.onClose?.();
                  },
                },
                m('span.pf-tab-handle__tab-title', tab.title),
                hasCloseButton &&
                  m(Button, {
                    onclick: (e: MouseEvent) => {
                      tab.onClose?.();
                      e.stopPropagation();
                    },
                    compact: true,
                    icon: 'close',
                  }),
              );
            }),
          ),
        ),
      },
      children,
    );
  }
}

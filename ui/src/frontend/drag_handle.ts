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
import {raf} from '../core/raf_scheduler';
import {Button} from '../widgets/button';
import {MenuItem, PopupMenu2} from '../widgets/menu';
import {DEFAULT_DETAILS_CONTENT_HEIGHT} from './css_constants';
import {DragGestureHandler} from '../base/drag_gesture_handler';
import {globals} from './globals';
import {DisposableStack} from '../base/disposable_stack';

const DRAG_HANDLE_HEIGHT_PX = 28;
const UP_ICON = 'keyboard_arrow_up';
const DOWN_ICON = 'keyboard_arrow_down';

export interface Tab {
  // Unique key for this tab, passed to callbacks.
  key: string;

  // Tab title to show on the tab handle.
  title: m.Children;

  // Whether to show a close button on the tab handle or not.
  // Default = false.
  hasCloseButton?: boolean;
}

export interface TabDropdownEntry {
  // Unique key for this tab dropdown entry.
  key: string;

  // Title to show on this entry.
  title: string;

  // Called when tab dropdown entry is clicked.
  onClick: () => void;

  // Whether this tab is checked or not
  checked: boolean;
}

export interface DragHandleAttrs {
  // The current height of the panel.
  height: number;

  // Called when the panel is dragged.
  resize: (height: number) => void;

  // A list of tabs to show in the tab bar.
  tabs: Tab[];

  // The key of the "current" tab.
  currentTabKey?: string;

  // A list of entries to show in the tab dropdown.
  // If undefined, the tab dropdown button will not be displayed.
  tabDropdownEntries?: TabDropdownEntry[];

  // Called when a tab is clicked.
  onTabClick: (key: string) => void;

  // Called when a tab is closed using its close button.
  onTabClose?: (key: string) => void;
}

export function getDefaultDetailsHeight() {
  // This needs to be a function instead of a const to ensure the CSS constants
  // have been initialized by the time we perform this calculation;
  return DRAG_HANDLE_HEIGHT_PX + DEFAULT_DETAILS_CONTENT_HEIGHT;
}

function getFullScreenHeight() {
  const page = document.querySelector('.page') as HTMLElement;
  if (page === null) {
    // Fall back to at least partially open.
    return getDefaultDetailsHeight();
  } else {
    return page.clientHeight;
  }
}

export class DragHandle implements m.ClassComponent<DragHandleAttrs> {
  private dragStartHeight = 0;
  private height = 0;
  private previousHeight = this.height;
  private resize: (height: number) => void = () => {};
  private isClosed = this.height <= 0;
  private isFullscreen = false;
  // We can't get real fullscreen height until the pan_and_zoom_handler
  // exists.
  private fullscreenHeight = 0;
  private trash = new DisposableStack();

  oncreate({dom, attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    this.resize = attrs.resize;
    this.height = attrs.height;
    this.isClosed = this.height <= 0;
    this.fullscreenHeight = getFullScreenHeight();
    const elem = dom as HTMLElement;
    this.trash.use(
      new DragGestureHandler(
        elem,
        this.onDrag.bind(this),
        this.onDragStart.bind(this),
        this.onDragEnd.bind(this),
      ),
    );
    const cmd = globals.commandManager.registerCommand({
      id: 'perfetto.ToggleDrawer',
      name: 'Toggle drawer',
      defaultHotkey: 'Q',
      callback: () => {
        this.toggleVisibility();
      },
    });
    this.trash.use(cmd);
  }

  private toggleVisibility() {
    if (this.height === 0) {
      this.isClosed = false;
      if (this.previousHeight === 0) {
        this.previousHeight = getDefaultDetailsHeight();
      }
      this.resize(this.previousHeight);
    } else {
      this.isFullscreen = false;
      this.isClosed = true;
      this.previousHeight = this.height;
      this.resize(0);
    }
    raf.scheduleFullRedraw();
  }

  onupdate({attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    this.resize = attrs.resize;
    this.height = attrs.height;
    this.isClosed = this.height <= 0;
  }

  onremove(_: m.CVnodeDOM<DragHandleAttrs>) {
    this.trash.dispose();
  }

  onDrag(_x: number, y: number) {
    const newHeight = Math.floor(
      this.dragStartHeight + DRAG_HANDLE_HEIGHT_PX / 2 - y,
    );
    this.isClosed = newHeight <= 0;
    this.isFullscreen = newHeight >= this.fullscreenHeight;
    this.resize(newHeight);
    raf.scheduleFullRedraw();
  }

  onDragStart(_x: number, _y: number) {
    this.dragStartHeight = this.height;
  }

  onDragEnd() {}

  view({attrs}: m.CVnode<DragHandleAttrs>) {
    const {
      tabDropdownEntries,
      currentTabKey,
      tabs,
      onTabClick,
      onTabClose = () => {},
    } = attrs;

    const icon = this.isClosed ? UP_ICON : DOWN_ICON;
    const title = this.isClosed ? 'Show panel' : 'Hide panel';
    const renderTab = (tab: Tab) => {
      const {key, hasCloseButton = false} = tab;
      const tag = currentTabKey === key ? '.tab[active]' : '.tab';
      return m(
        tag,
        {
          key,
          onclick: (event: Event) => {
            if (!event.defaultPrevented) {
              onTabClick(key);
            }
          },
          // Middle click to close
          onauxclick: (event: MouseEvent) => {
            if (!event.defaultPrevented) {
              onTabClose(key);
            }
          },
        },
        m('span.pf-tab-title', tab.title),
        hasCloseButton &&
          m(Button, {
            onclick: (event: Event) => {
              onTabClose(key);
              event.preventDefault();
            },
            compact: true,
            icon: 'close',
          }),
      );
    };

    return m(
      '.handle',
      m(
        '.buttons',
        tabDropdownEntries && this.renderTabDropdown(tabDropdownEntries),
      ),
      m('.tabs', tabs.map(renderTab)),
      m(
        '.buttons',
        m(Button, {
          onclick: () => {
            this.isClosed = false;
            this.isFullscreen = true;
            // Ensure fullscreenHeight is up to date.
            this.fullscreenHeight = getFullScreenHeight();
            this.resize(this.fullscreenHeight);
            raf.scheduleFullRedraw();
          },
          title: 'Open fullscreen',
          disabled: this.isFullscreen,
          icon: 'vertical_align_top',
          compact: true,
        }),
        m(Button, {
          onclick: () => {
            this.toggleVisibility();
          },
          title,
          icon,
          compact: true,
        }),
      ),
    );
  }

  private renderTabDropdown(entries: TabDropdownEntry[]) {
    return m(
      PopupMenu2,
      {
        trigger: m(Button, {
          compact: true,
          icon: 'more_vert',
          disabled: entries.length === 0,
          title: 'More Tabs',
        }),
      },
      entries.map((entry) => {
        return m(MenuItem, {
          key: entry.key,
          label: entry.title,
          onclick: () => entry.onClick(),
          icon: entry.checked ? 'check_box' : 'check_box_outline_blank',
        });
      }),
    );
  }
}

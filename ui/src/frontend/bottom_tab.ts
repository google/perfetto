// Copyright (C) 2022 The Android Open Source Project
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
import {v4 as uuidv4} from 'uuid';

import {stringifyJsonWithBigints} from '../base/json_utils';
import {Actions} from '../common/actions';
import {EngineProxy} from '../common/engine';
import {traceEvent} from '../common/metatracing';
import {Registry} from '../common/registry';
import {raf} from '../core/raf_scheduler';

import {globals} from './globals';

export interface NewBottomTabArgs {
  engine: EngineProxy;
  tag?: string;
  uuid: string;
  config: {};
}

// Interface for allowing registration and creation of bottom tabs.
// See comments on |TrackCreator| for more details.
export interface BottomTabCreator {
  readonly kind: string;

  create(args: NewBottomTabArgs): BottomTab;
}

export const bottomTabRegistry = Registry.kindRegistry<BottomTabCreator>();

// Period to wait for the newly-added tabs which are loading before showing
// them to the user. This period is short enough to not be user-visible,
// while being long enough for most of the simple queries to complete, reducing
// flickering in the UI.
const NEW_LOADING_TAB_DELAY_MS = 50;

// An interface representing a bottom tab displayed on the panel in the bottom
// of the ui (e.g. "Current Selection").
//
// The implementations of this class are provided by different plugins, which
// register the implementations with bottomTabRegistry, keyed by a unique name
// for each type of BottomTab.
//
// Lifetime: the instances of this class are owned by BottomTabPanel and exist
// for as long as a tab header is shown to the user in the bottom tab list (with
// minor exceptions, like a small grace period between when the tab is related).
//
// BottomTab implementations should pass the unique identifier(s) for the
// content displayed via the |Config| and fetch additional details via Engine
// instead of relying on getting the data from the global storage. For example,
// for tabs corresponding to details of the selected objects on a track, a new
// BottomTab should be created for each new selection.
export abstract class BottomTabBase<Config = {}> {
  // Config for this details panel. Should be serializable.
  protected readonly config: Config;
  // Engine for running queries and fetching additional data.
  protected readonly engine: EngineProxy;
  // Optional tag, which is used to ensure that only one tab
  // with the same tag can exist - adding a new tab with the same tag
  // (e.g. 'current_selection') would close the previous one. This
  // also can be used to close existing tab.
  readonly tag?: string;
  // Unique id for this details panel. Can be used to close previously opened
  // panel.
  readonly uuid: string;

  constructor(args: NewBottomTabArgs) {
    this.config = args.config as Config;
    this.engine = args.engine;
    this.tag = args.tag;
    this.uuid = args.uuid;
  }

  // Entry point for customisation of the displayed title for this panel.
  abstract getTitle(): string;

  // Generate a mithril node for this component.
  abstract renderPanel(): m.Children;

  // API for the tab to notify the TabList that it's still preparing the data.
  // If true, adding a new tab will be delayed for a short while (~50ms) to
  // reduce the flickering.
  //
  // Note: it's a "poll" rather than "push" API: there is no explicit API
  // for the tabs to notify the tab list, as the tabs are expected to schedule
  // global redraw anyway and the tab list will poll the tabs as necessary
  // during the redraw.
  isLoading(): boolean {
    return false;
  }
}


// BottomTabBase provides a more generic API allowing users to provide their
// custom mithril component, which would allow them to listen to mithril
// lifecycle events. Most cases, however, don't need them and BottomTab
// provides a simplified API for the common case.
export abstract class BottomTab<Config = {}> extends BottomTabBase<Config> {
  constructor(args: NewBottomTabArgs) {
    super(args);
  }

  abstract viewTab(): void|m.Children;

  close(): void {
    closeTab(this.uuid);
  }

  renderPanel(): m.Children {
    return m(
        BottomTabAdapter,
        {key: this.uuid, panel: this} as BottomTabAdapterAttrs);
  }
}

interface BottomTabAdapterAttrs {
  panel: BottomTab;
}

class BottomTabAdapter implements m.ClassComponent<BottomTabAdapterAttrs> {
  view(vnode: m.CVnode<BottomTabAdapterAttrs>): void|m.Children {
    return vnode.attrs.panel.viewTab();
  }
}

export type AddTabArgs = {
  kind: string,
  config: {},
  tag?: string,
  // Whether to make the new tab current. True by default.
  select?: boolean;
};

export type AddTabResult =
    {
      uuid: string;
    }

// Shorthand for globals.bottomTabList.addTab(...) & redraw.
// Ignored when bottomTabList does not exist (e.g. no trace is open in the UI).
export function
addTab(args: AddTabArgs) {
  const tabList = globals.bottomTabList;
  if (!tabList) {
    return;
  }
  tabList.addTab(args);
  raf.scheduleFullRedraw();
}


// Shorthand for globals.bottomTabList.closeTabById(...) & redraw.
// Ignored when bottomTabList does not exist (e.g. no trace is open in the UI).
export function
closeTab(uuid: string) {
  const tabList = globals.bottomTabList;
  if (!tabList) {
    return;
  }
  tabList.closeTabById(uuid);
  raf.scheduleFullRedraw();
}

interface PendingTab {
  tab: BottomTabBase, args: AddTabArgs, startTime: number,
}

function tabSelectionKey(tab: BottomTabBase) {
  return tab.tag ?? tab.uuid;
}

export class BottomTabList {
  private tabs: BottomTabBase[] = [];
  private pendingTabs: PendingTab[] = [];
  private engine: EngineProxy;
  private scheduledFlushSetTimeoutId?: number;

  constructor(engine: EngineProxy) {
    this.engine = engine;
  }

  getTabs(): BottomTabBase[] {
    this.flushPendingTabs();
    return this.tabs;
  }

  // Add and create a new panel with given kind and config, replacing an
  // existing panel with the same tag if needed. Returns the uuid of a newly
  // created panel (which can be used in the future to close it).
  addTab(args: AddTabArgs): AddTabResult {
    const uuid = uuidv4();
    return traceEvent('addTab', () => {
      const newPanel = bottomTabRegistry.get(args.kind).create({
        engine: this.engine,
        uuid,
        config: args.config,
        tag: args.tag,
      });

      this.pendingTabs.push({
        tab: newPanel,
        args,
        startTime: window.performance.now(),
      });
      this.flushPendingTabs();

      return {
        uuid,
      };
    }, {
      args: {
        'uuid': uuid,
        'kind': args.kind,
        'tag': args.tag ?? '<undefined>',
        'config': stringifyJsonWithBigints(args.config),
      },
    });
  }

  closeTabByTag(tag: string) {
    const index = this.tabs.findIndex((tab) => tab.tag === tag);
    if (index !== -1) {
      this.removeTabAtIndex(index);
    }
    // User closing a tab by tag should affect pending tabs as well, as these
    // tabs were requested to be added to the tab list before this call.
    this.pendingTabs = this.pendingTabs.filter(({tab}) => tab.tag !== tag);
  }

  closeTabById(uuid: string) {
    const index = this.tabs.findIndex((tab) => tab.uuid === uuid);
    if (index !== -1) {
      this.removeTabAtIndex(index);
    }
    // User closing a tab by id should affect pending tabs as well, as these
    // tabs were requested to be added to the tab list before this call.
    this.pendingTabs = this.pendingTabs.filter(({tab}) => tab.uuid !== uuid);
  }

  private removeTabAtIndex(index: number) {
    const tab = this.tabs[index];
    this.tabs.splice(index, 1);
    // If the current tab was closed, select the tab to the right of it.
    // If the closed tab was current and last in the tab list, select the tab
    // that became last.
    if (tab.uuid === globals.state.currentTab && this.tabs.length > 0) {
      const newActiveIndex = index === this.tabs.length ? index - 1 : index;
      globals.dispatch(Actions.setCurrentTab(
          {tab: tabSelectionKey(this.tabs[newActiveIndex])}));
    }
    raf.scheduleFullRedraw();
  }

  // Check the list of the pending tabs and add the ones that are ready
  // (either tab.isLoading returns false or NEW_LOADING_TAB_DELAY_MS ms elapsed
  // since this tab was added).
  // Note: the pending tabs are stored in a queue to preserve the action order,
  // which matters for cases like adding tabs with the same tag.
  private flushPendingTabs() {
    const currentTime = window.performance.now();
    while (this.pendingTabs.length > 0) {
      const {tab, args, startTime} = this.pendingTabs[0];

      // This is a dirty hack^W^W low-lift solution for the world where some
      // "current selection" panels are implemented by BottomTabs and some by
      // details_panel.ts computing vnodes dynamically. Naive implementation
      // will: a) stop showing the old panel (because
      // globals.state.currentSelection changes). b) not showing the new
      // 'current_selection' tab yet. This will result in temporary shifting
      // focus to another tab (as no tab with 'current_selection' tag will
      // exist).
      //
      // To counteract this, short-circuit this logic and when:
      // a) no tag with 'current_selection' tag exists in the list of currently
      // displayed tabs and b) we are adding a tab with 'current_selection' tag.
      // add it immediately without waiting.
      // TODO(altimin): Remove this once all places have switched to be using
      // BottomTab to display panels.
      const currentSelectionTabAlreadyExists =
          this.tabs.filter((tab) => tab.tag === 'current_selection').length > 0;
      const dirtyHackForCurrentSelectionApplies =
          tab.tag === 'current_selection' && !currentSelectionTabAlreadyExists;

      const elapsedTimeMs = currentTime - startTime;
      if (tab.isLoading() && elapsedTimeMs < NEW_LOADING_TAB_DELAY_MS &&
          !dirtyHackForCurrentSelectionApplies) {
        this.schedulePendingTabsFlush(NEW_LOADING_TAB_DELAY_MS - elapsedTimeMs);
        // The first tab is not ready yet, wait.
        return;
      }

      traceEvent('addPendingTab', () => {
        this.pendingTabs.shift();

        const index =
            args.tag ? this.tabs.findIndex((tab) => tab.tag === args.tag) : -1;
        if (index === -1) {
          this.tabs.push(tab);
        } else {
          this.tabs[index] = tab;
        }

        if (args.select === undefined || args.select === true) {
          globals.dispatch(Actions.setCurrentTab({tab: tabSelectionKey(tab)}));
        }
        // setCurrentTab will usually schedule a redraw, but not if we replace
        // the tab with the same tag, so we force an update here.
        raf.scheduleFullRedraw();
      }, {
        args: {
          'uuid': tab.uuid,
          'is_loading': tab.isLoading().toString(),
        },
      });
    }
  }

  private schedulePendingTabsFlush(waitTimeMs: number) {
    if (this.scheduledFlushSetTimeoutId) {
      // The flush is already pending, no action is required.
      return;
    }
    setTimeout(() => {
      this.scheduledFlushSetTimeoutId = undefined;
      this.flushPendingTabs();
    }, waitTimeMs);
  }
}

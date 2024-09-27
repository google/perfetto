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
import {Trace} from '../trace';

export interface NewBottomTabArgs<Config> {
  trace: Trace;
  tag?: string;
  uuid: string;
  config: Config;
}

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
  // The Trace interface to manipulate the state of the UI.
  readonly trace: Trace;
  // Optional tag, which is used to ensure that only one tab
  // with the same tag can exist - adding a new tab with the same tag
  // (e.g. 'current_selection') would close the previous one. This
  // also can be used to close existing tab.
  readonly tag?: string;
  // Unique id for this details panel. Can be used to close previously opened
  // panel.
  readonly uuid: string;

  constructor(args: NewBottomTabArgs<Config>) {
    this.config = args.config;
    this.trace = args.trace;
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

  protected get engine() {
    return this.trace.engine;
  }
}

// BottomTabBase provides a more generic API allowing users to provide their
// custom mithril component, which would allow them to listen to mithril
// lifecycle events. Most cases, however, don't need them and BottomTab
// provides a simplified API for the common case.
export abstract class BottomTab<Config = {}> extends BottomTabBase<Config> {
  constructor(args: NewBottomTabArgs<Config>) {
    super(args);
  }

  abstract viewTab(): m.Children;

  renderPanel(): m.Children {
    return m(BottomTabAdapter, {
      key: this.uuid,
      panel: this,
    } as BottomTabAdapterAttrs);
  }
}

interface BottomTabAdapterAttrs {
  panel: BottomTab;
}

class BottomTabAdapter implements m.ClassComponent<BottomTabAdapterAttrs> {
  view(vnode: m.CVnode<BottomTabAdapterAttrs>): void | m.Children {
    return vnode.attrs.panel.viewTab();
  }
}

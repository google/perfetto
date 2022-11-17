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

import * as m from 'mithril';
import {v4 as uuidv4} from 'uuid';

import {EngineProxy} from '../common/engine';
import {Registry} from '../common/registry';

import {Panel, PanelSize, PanelVNode} from './panel';

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
  abstract createPanelVnode(): PanelVNode;
}


// BottomTabBase provides a more generic API allowing users to provide their
// custom mithril component, which would allow them to listen to mithril
// lifecycle events. Most cases, however, don't need them and BottomTab
// provides a simplified API for the common case.
export abstract class BottomTab<Config = {}> extends BottomTabBase<Config> {
  constructor(args: NewBottomTabArgs) {
    super(args);
  }

  // These methods are direct counterparts to renderCanvas and view with
  // slightly changes names to prevent cases when `BottomTab` will
  // be accidentally used a mithril component.
  abstract renderTabCanvas(ctx: CanvasRenderingContext2D, size: PanelSize):
      void;
  abstract viewTab(): void|m.Children;

  createPanelVnode(): m.Vnode<any, any> {
    return m(BottomTabAdapter, {key: this.uuid, panel: this});
  }
}

interface BottomTabAdapterAttrs {
  panel: BottomTab;
}

class BottomTabAdapter extends Panel<BottomTabAdapterAttrs> {
  renderCanvas(
      ctx: CanvasRenderingContext2D, size: PanelSize,
      vnode: PanelVNode<BottomTabAdapterAttrs>): void {
    vnode.attrs.panel.renderTabCanvas(ctx, size);
  }

  view(vnode: m.CVnode<BottomTabAdapterAttrs>): void|m.Children {
    return vnode.attrs.panel.viewTab();
  }
}

export type AddTabArgs = {
  kind: string,
  config: {},
  tag?: string,
};

export type AddTabResult = {
  uuid: string;
}

export class BottomTabList {
  tabs: BottomTabBase[] = [];
  private engine: EngineProxy;

  constructor(engine: EngineProxy) {
    this.engine = engine;
  }

  // Add and create a new panel with given kind and config, replacing an
  // existing panel with the same tag if needed. Returns the uuid of a newly
  // created panel (which can be used in the future to close it).
  addTab(args: AddTabArgs): AddTabResult {
    const uuid = uuidv4();
    const newPanel = bottomTabRegistry.get(args.kind).create({
      engine: this.engine,
      uuid,
      config: args.config,
      tag: args.tag,
    });

    const index =
        args.tag ? this.tabs.findIndex((tab) => tab.tag === args.tag) : -1;
    if (index === -1) {
      this.tabs.push(newPanel);
    } else {
      this.tabs[index] = newPanel;
    }

    return {
      uuid,
    };
  }

  closeTabByTag(tag: string) {
    this.tabs = this.tabs.filter((panel) => panel.tag !== tag);
  }

  closeTabById(uuid: string) {
    this.tabs = this.tabs.filter((panel) => panel.uuid !== uuid);
  }
}

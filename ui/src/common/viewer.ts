// Copyright (C) 2023 The Android Open Source Project
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

import {Disposable} from '../base/disposable';
import {globals} from '../frontend/globals';
import {runQueryInNewTab} from '../frontend/query_result_tab';
import {Viewer} from '../public';

import {Actions} from './actions';

export class ViewerImpl implements Viewer {
  sidebar = {
    hide: () => {
      globals.dispatch(Actions.setSidebar({
        visible: false,
      }));
    },
    show:
        () => {
          globals.dispatch(Actions.setSidebar({
            visible: true,
          }));
        },
    isVisible: () => globals.state.sidebarVisible,
  };

  tabs = {
    openQuery: runQueryInNewTab,
  };

  constructor() {}

  getProxy(pluginId: string): ViewerProxy {
    return new ViewerProxy(this, pluginId);
  }
}

type AnyFunction = (...args: any[]) => any;
type AnyProcedure = (...args: any[]) => void;

function wrap<F extends AnyFunction>(
    allow: () => boolean, f: F, deadResult: ReturnType<F>) {
  return (...args: Parameters<F>) => {
    if (allow()) {
      return f(...args);
    } else {
      return deadResult;
    }
  };
}

function wrapVoid<F extends AnyProcedure>(allow: () => boolean, f: F) {
  return (...args: Parameters<F>) => {
    if (allow()) {
      f(...args);
    }
  };
}

export class ViewerProxy implements Viewer, Disposable {
  readonly parent: ViewerImpl;
  readonly pluginId: string;
  private alive: boolean;

  // ViewerImpl:
  sidebar: Viewer['sidebar'];
  tabs: Viewer['tabs'];

  // ViewerProxy:
  constructor(parent: ViewerImpl, pluginId: string) {
    this.parent = parent;
    this.pluginId = pluginId;
    this.alive = true;
    const allow = () => this.alive;

    this.sidebar = {
      hide: wrapVoid(allow, parent.sidebar.hide.bind(parent.sidebar)),
      show: wrapVoid(allow, parent.sidebar.show.bind(parent.sidebar)),
      isVisible:
          wrap(allow, parent.sidebar.isVisible.bind(parent.sidebar), false),
    };

    this.tabs = {
      openQuery: wrapVoid(allow, parent.tabs.openQuery.bind(parent.tabs)),
    };
  }

  dispose(): void {
    this.alive = false;
  }
}

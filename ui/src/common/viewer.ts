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
import {TrackPredicate, Viewer} from '../public';

import {Actions} from './actions';

class SidebarImpl {
  hide() {
    globals.dispatch(Actions.setSidebar({
      visible: false,
    }));
  }

  show() {
    globals.dispatch(Actions.setSidebar({
      visible: true,
    }));
  }

  isVisible() {
    return globals.state.sidebarVisible;
  }
};

class TracksImpl {
  pin(predicate: TrackPredicate) {
    const tracks = Object.values(globals.state.tracks);
    for (const track of tracks) {
      const tags = {
        name: track.name,
      };
      if (predicate(tags) && !this.isPinned(track.key)) {
        globals.dispatch(Actions.toggleTrackPinned({
          trackKey: track.key,
        }));
      }
    }
  }

  unpin(predicate: TrackPredicate) {
    const tracks = Object.values(globals.state.tracks);
    for (const track of tracks) {
      const tags = {
        name: track.name,
      };
      if (predicate(tags) && this.isPinned(track.key)) {
        globals.dispatch(Actions.toggleTrackPinned({
          trackKey: track.key,
        }));
      }
    }
  }

  private isPinned(trackId: string): boolean {
    return globals.state.pinnedTracks.includes(trackId);
  }
};

export class ViewerImpl implements Viewer {
  sidebar = new SidebarImpl();
  tracks = new TracksImpl();

  tabs = {
    openQuery: runQueryInNewTab,
  };

  commands = {
    run: (id: string, ...args: any[]) => {
      globals.commandManager.runCommand(id, ...args);
    },
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
  tracks: Viewer['tracks'];
  tabs: Viewer['tabs'];
  commands: Viewer['commands'];

  // ViewerProxy:
  constructor(parent: ViewerImpl, pluginId: string) {
    this.parent = parent;
    this.pluginId = pluginId;
    this.alive = true;
    const allow = () => this.alive;

    const p = parent;
    this.sidebar = {
      hide: wrapVoid(allow, p.sidebar.hide.bind(p.sidebar)),
      show: wrapVoid(allow, p.sidebar.show.bind(p.sidebar)),
      isVisible: wrap(allow, p.sidebar.isVisible.bind(p.sidebar), false),
    };

    this.tracks = {
      pin: wrapVoid(allow, p.tracks.pin.bind(p.tracks)),
      unpin: wrapVoid(allow, p.tracks.unpin.bind(p.tracks)),
    };

    this.tabs = {
      openQuery: wrapVoid(allow, p.tabs.openQuery.bind(p.tabs)),
    };

    this.commands = {
      run: wrapVoid(allow, p.commands.run.bind(p.commands)),
    };
  }

  dispose(): void {
    this.alive = false;
  }
}

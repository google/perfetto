// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {AppImpl} from '../core/app_impl';
import {TrackNode} from '../public/workspace';
import {
  LYNX_ISSUES_PLUGIN_ID,
  LYNX_BACKGROUND_THREAD_NAME,
  LYNX_VITAL_TIMESTAMP_PLUGIN_ID,
} from './constants';

export function isLynxBackgroundScriptThreadGroup(item: TrackNode) {
  return (
    item.hasChildren &&
    item.children.some((item) =>
      item.title.includes(LYNX_BACKGROUND_THREAD_NAME),
    )
  );
}

export function inLynxTrackGroup(currentTrack: TrackNode) {
  const workspace = AppImpl.instance.trace?.workspace;
  if (workspace && workspace?.children.length) {
    for (let i = 0; i < workspace.children.length; i++) {
      const item: TrackNode = workspace.children[i];
      if (
        isLynxBackgroundScriptThreadGroup(item) &&
        item.getTrackById(currentTrack.id)
      ) {
        return true;
      }
    }
  }
  return false;
}

export function customTopTrack(currentTrack: TrackNode) {
  return (
    currentTrack.uri === LYNX_ISSUES_PLUGIN_ID ||
    currentTrack.uri === LYNX_VITAL_TIMESTAMP_PLUGIN_ID
  );
}

export function getBackgroundScriptThreadTrackNode(
  item: TrackNode,
): TrackNode | undefined {
  if (item.hasChildren) {
    return item.children.find((value) =>
      value.title.includes(LYNX_BACKGROUND_THREAD_NAME),
    );
  }
  return undefined;
}

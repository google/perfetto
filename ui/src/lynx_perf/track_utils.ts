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
import {lynxPerfGlobals} from './lynx_perf_globals';

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
    for (let i = item.children.length - 1; i >= 0; i--) {
      const trackNode = item.children[i];
      if (trackNode.title.includes(LYNX_BACKGROUND_THREAD_NAME)) {
        return trackNode;
      }
    }
  }
  return undefined;
}

export function getMainScriptThreadTrackNode(
  item: TrackNode,
): TrackNode | undefined {
  if (item.hasChildren) {
    const mainThreadTrack = item.children.find((value) => {
      if (value.children.length <= 0) {
        return false;
      }
      const trackNode = value.children[0];
      const sliceThread = lynxPerfGlobals.state.trackUriToThreadMap.get(
        trackNode.uri ?? '',
      );
      return (
        sliceThread && sliceThread.isMainThread && !sliceThread.isKernelThread
      );
    });
    if (mainThreadTrack) {
      return mainThreadTrack;
    }
    // Find the thread with the smallest tid for non Linux Trace
    let minTid = Number.MAX_SAFE_INTEGER;
    let minTrackNode: TrackNode | undefined;
    for (let i = 0; i < item.children.length; i++) {
      const trackNode = item.children[i];
      if (trackNode.children.length <= 0) {
        continue;
      }
      const trackChildNode = trackNode.children[0];
      const sliceThread = lynxPerfGlobals.state.trackUriToThreadMap.get(
        trackChildNode.uri ?? '',
      );
      if (sliceThread && sliceThread.tid < minTid) {
        minTid = sliceThread.tid;
        minTrackNode = trackNode;
      }
    }
    return minTrackNode;
  }
  return undefined;
}

/**
 * Checks if the given track URI belongs to the main thread
 */
export function isMainThreadTrack(trackUri: string): boolean {
  const sliceThread = lynxPerfGlobals.state.trackUriToThreadMap.get(trackUri);
  return (
    (sliceThread !== undefined &&
      sliceThread.isMainThread &&
      !sliceThread.isKernelThread) ||
    false
  );
}

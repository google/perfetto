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

import {GroupNode, Workspace} from './workspace';

/**
 * Gets or creates a group for a given process given the normal grouping
 * conventions.
 *
 * @param workspace - The workspace to search for the group on.
 * @param upid - The upid of teh process to find.
 */
export function getOrCreateGroupForProcess(
  workspace: Workspace,
  upid: number,
): GroupNode {
  const uri = `process${upid}`;
  const group = workspace.getGroupByUri(uri);
  if (group) {
    return group;
  } else {
    const group = new GroupNode(`Process ${upid}`);
    group.uri = uri;
    workspace.insertChildInOrder(group);
    return group;
  }
}

/**
 * Gets or creates a group for a given thread given the normal grouping
 * conventions.
 *
 * @param workspace - The workspace to search for the group on.
 * @param utid - The utid of the thread to find.
 */
export function getOrCreateGroupForThread(
  workspace: Workspace,
  utid: number,
): GroupNode {
  const uri = `thread${utid}`;
  const group = workspace.getGroupByUri(uri);
  if (group) {
    return group;
  } else {
    const group = new GroupNode(`Thread ${utid}`);
    group.uri = uri;
    workspace.insertChildInOrder(group);
    return group;
  }
}

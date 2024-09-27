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

import {TrackNode, TrackNodeArgs, Workspace} from './workspace';

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
): TrackNode {
  return getOrCreateGroup(workspace, `process${upid}`, {
    title: `Process ${upid}`,
    isSummary: true,
  });
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
): TrackNode {
  return getOrCreateGroup(workspace, `thread${utid}`, {
    title: `Thread ${utid}`,
    isSummary: true,
  });
}

/**
 * Gets or creates a group for user interaction
 *
 * @param workspace - The workspace on which to create the group.
 */
export function getOrCreateUserInteractionGroup(
  workspace: Workspace,
): TrackNode {
  return getOrCreateGroup(workspace, 'user_interaction', {
    title: 'User Interaction',
    collapsed: false, // Expand this by default
    isSummary: true,
  });
}

// Internal utility function to avoid duplicating the logic to get or create a
// group by ID.
function getOrCreateGroup(
  workspace: Workspace,
  id: string,
  args?: Omit<Partial<TrackNodeArgs>, 'id'>,
): TrackNode {
  const group = workspace.getTrackById(id);
  if (group) {
    return group;
  } else {
    const group = new TrackNode({id, ...args});
    workspace.addChildInOrder(group);
    return group;
  }
}

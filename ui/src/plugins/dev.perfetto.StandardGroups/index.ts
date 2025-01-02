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

import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode, Workspace} from '../../public/workspace';

// Type indicating the standard groups supported by this plugin.
export type StandardGroup =
  | 'USER_INTERACTION'
  | 'THERMALS'
  | 'POWER'
  | 'IO'
  | 'MEMORY'
  | 'HARDWARE'
  | 'CPU'
  | 'GPU'
  | 'NETWORK'
  | 'SYSTEM';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.StandardGroups';

  async onTraceLoad() {}

  /**
   * Gets or creates a standard group to place tracks into.
   *
   * @param workspace - The workspace on which to create the group.
   */
  getOrCreateStandardGroup(
    workspace: Workspace,
    group: StandardGroup,
  ): TrackNode {
    switch (group) {
      case 'USER_INTERACTION':
        // Expand this by default
        return getOrCreateGroup(
          workspace,
          'user_interaction',
          'User Interaction',
          true,
        );
      case 'THERMALS':
        return getOrCreateGroup(workspace, 'thermal', 'Thermals');
      case 'POWER':
        return getOrCreateGroup(workspace, 'power', 'Power');
      case 'CPU':
        return getOrCreateGroup(workspace, 'cpu', 'CPU');
      case 'GPU':
        return getOrCreateGroup(workspace, 'gpu', 'GPU');
      case 'HARDWARE':
        return getOrCreateGroup(workspace, 'hardware', 'Hardware');
      case 'IO':
        return getOrCreateGroup(workspace, 'io', 'IO');
      case 'MEMORY':
        return getOrCreateGroup(workspace, 'memory', 'Memory');
      case 'NETWORK':
        return getOrCreateGroup(workspace, 'network', 'Network');
      case 'SYSTEM':
        return getOrCreateGroup(workspace, 'system', 'System');
    }
  }
}

// Internal utility function to avoid duplicating the logic to get or create a
// group by ID.
function getOrCreateGroup(
  workspace: Workspace,
  id: string,
  title: string,
  collapsed: boolean = true,
): TrackNode {
  const group = workspace.getTrackById(id);
  if (group) {
    return group;
  } else {
    const group = new TrackNode({id, title, isSummary: true, collapsed});
    workspace.addChildInOrder(group);
    return group;
  }
}

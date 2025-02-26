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

  private readonly groups: Record<StandardGroup, TrackNode> = {
    // Expand this group by default
    USER_INTERACTION: makeGroupNode('User Interaction', false),
    THERMALS: makeGroupNode('Thermals'),
    POWER: makeGroupNode('Power'),
    CPU: makeGroupNode('CPU'),
    GPU: makeGroupNode('GPU'),
    HARDWARE: makeGroupNode('Hardware'),
    IO: makeGroupNode('IO'),
    MEMORY: makeGroupNode('Memory'),
    NETWORK: makeGroupNode('Network'),
    SYSTEM: makeGroupNode('System'),
  };

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
    const node = this.groups[group];

    // Only add the group if it's not already been added
    if (node.parent === undefined) {
      workspace.addChildInOrder(node);
    }

    return node;
  }
}

function makeGroupNode(title: string, collapsed = true) {
  return new TrackNode({title, isSummary: true, collapsed});
}

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

import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {PerfettoPlugin} from '../../public/plugin';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import TraceProcessorTrackPlugin from '../dev.perfetto.TraceProcessorTrack';

const NESTING_SEPARATOR: string = ' -> ';

/**
 * Organizes tracks with the name containing ' -> ' into a hierarchical
 * structure. It creates a copy of the tracks at the end of their parent.
 *
 * For example, tracks with names:
 *   - Foo -> Bar -> Track1
 *   - Foo -> Bar -> Track2
 *   - Foo -> Track3
 *
 * Will be reorganized into:
 *   - Foo
 *      - Bar
 *         - Track1
 *         - Track2
 *      - Track3
 */
export default class implements PerfettoPlugin {
  static readonly id = 'com.android.CreateTrackHierarchyFromNames';
  static readonly dependencies = [
    StandardGroupsPlugin,
    TraceProcessorTrackPlugin,
  ];

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.createTrackHierarchyFromNames(ctx);
  }

  private createTrackHierarchyFromNames(ctx: Trace) {
    ctx.defaultWorkspace.flatTracks
      .filter((track) => track.name.includes(NESTING_SEPARATOR))
      .forEach((track) => this.organizeTrack(track));
  }

  private organizeTrack(track: TrackNode) {
    const name = track.name;
    const path = name.split(NESTING_SEPARATOR);

    // No path found, no need to organize
    if (path.length <= 1) return;

    // Root tracks not supported
    if (!track.parent) return;

    const startFrom = track.parent;

    const parentPath = path.slice(0, -1);
    const parentNode = this.lookupTrackGroupOrCreate(startFrom, parentPath);

    const newName = path.at(-1);
    const cloned = new TrackNode({
      uri: track.uri,
      name: newName,
      removable: track.removable,
    });

    parentNode.addChildLast(cloned);
  }

  private _nodeCache = new Map<TrackNode, Map<string, TrackNode>>();

  private lookupTrackGroupOrCreate(
    startFrom: TrackNode,
    path: string[],
  ): TrackNode {
    const pathKey = path.join(NESTING_SEPARATOR);

    const cachedNode = this._nodeCache.get(startFrom)?.get(pathKey);
    if (cachedNode) {
      return cachedNode;
    }

    let currentNode = startFrom;
    for (const pathPart of path) {
      let nextNode = currentNode.children.find(
        (child) => child.name === pathPart,
      );
      if (!nextNode) {
        nextNode = new TrackNode({name: pathPart});
        currentNode.addChildLast(nextNode);
      }
      currentNode = nextNode;
    }

    const finalNode = currentNode;

    let pathMap = this._nodeCache.get(startFrom);
    if (!pathMap) {
      pathMap = new Map<string, TrackNode>();
      this._nodeCache.set(startFrom, pathMap);
    }
    pathMap.set(pathKey, finalNode);
    return finalNode;
  }
}

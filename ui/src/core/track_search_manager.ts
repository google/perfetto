// Copyright (C) 2026 The Android Open Source Project
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

import {TrackNode, Workspace} from '../public/workspace';

export interface TrackSearchMatch {
  readonly node: TrackNode;
  readonly matchStart: number;
  readonly matchLength: number;
}

export interface TrackSearchModel {
  readonly searchTerm: string;
  readonly useRegex: boolean;
  readonly searchWithinCollapsedGroups: boolean;
}

/**
 * Pure function to search tracks in a workspace.
 *
 * Unlike the track filter (which hides non-matching tracks), track search:
 * - Keeps all tracks visible
 * - Highlights matching parts of track names
 * - Allows navigation between matches with Enter/Shift+Enter
 */
export function searchTracks(
  workspace: Workspace,
  model: TrackSearchModel,
): readonly TrackSearchMatch[] {
  const {searchTerm, useRegex, searchWithinCollapsedGroups} = model;

  if (!searchTerm) {
    return [];
  }

  const tracksToSearch = getSearchableTracks(
    workspace,
    searchWithinCollapsedGroups,
  );
  const matches: TrackSearchMatch[] = [];

  if (useRegex) {
    let regex: RegExp;
    try {
      regex = new RegExp(searchTerm, 'i');
    } catch {
      // Invalid regex, no matches
      return [];
    }

    for (const node of tracksToSearch) {
      const match = regex.exec(node.name);
      if (match) {
        matches.push({
          node,
          matchStart: match.index,
          matchLength: match[0].length,
        });
      }
    }
  } else {
    // Plain text search (case-insensitive)
    const searchTermLower = searchTerm.toLowerCase();

    for (const node of tracksToSearch) {
      const nameLower = node.name.toLowerCase();
      const matchIndex = nameLower.indexOf(searchTermLower);

      if (matchIndex !== -1) {
        matches.push({
          node,
          matchStart: matchIndex,
          matchLength: searchTerm.length,
        });
      }
    }
  }

  return matches;
}

function getSearchableTracks(
  workspace: Workspace,
  searchCollapsed: boolean,
): TrackNode[] {
  const result: TrackNode[] = [];

  const collectTracks = (node: TrackNode) => {
    if (!node.headless) {
      result.push(node);
    }

    if (searchCollapsed || node.headless || node.expanded) {
      for (const child of node.children) {
        collectTracks(child);
      }
    }
  };

  // Pinned tracks
  for (const node of workspace.pinnedTracks) {
    collectTracks(node);
  }

  // Main tracks
  for (const node of workspace.children) {
    collectTracks(node);
  }

  return result;
}

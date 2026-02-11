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

import m from 'mithril';
import {TrackNode, Workspace} from '../public/workspace';
import {TrackManagerImpl} from './track_manager';
import {TrackSearchModel} from '../frontend/timeline_page/track_search_bar';

export interface TrackSearchMatch {
  readonly node: TrackNode;
  readonly matchStart: number;
  readonly matchLength: number;
}

export interface TrackSearchResults {
  readonly matches: readonly TrackSearchMatch[];
  readonly currentMatchIndex: number;
}
/**
 * Manages track search state for the timeline.
 *
 * Unlike the track filter (which hides non-matching tracks), track search:
 * - Keeps all tracks visible
 * - Highlights matching parts of track names
 * - Allows navigation between matches with Enter/Shift+Enter
 */
export class TrackSearchCache {
  private cachedMatches: TrackSearchMatch[] = [];
  private currentMatchIndex = -1;
  private workspace?: Workspace;
  private model?: TrackSearchModel;

  useTrackSearchResults(
    workspace: Workspace,
    searchTerm: string,
    useRegex: boolean,
    searchWithinCollapsedGroups: boolean,
  ): TrackSearchResults {
    console.log(
      workspace,
      `Updating track search: term="${searchTerm}", regex=${useRegex}, searchCollapsed=${searchWithinCollapsedGroups}`,
    );

    return {
      matches: this.cachedMatches,
      currentMatchIndex: this._currentMatchIndex,
    };
    // TODO - depending on what changed in the model, maybe update the internal
    // state and return the new track search results.
  }

  stepForwards(): void {
    if (this._matches.length === 0) return;
    this._currentMatchIndex =
      (this._currentMatchIndex + 1) % this._matches.length;
    this.scrollToCurrentMatch();
  }

  /**
   * Navigate to the previous match (with wrap-around).
   */
  stepBackwards(): void {
    if (this._matches.length === 0) return;
    this._currentMatchIndex =
      (this._currentMatchIndex - 1 + this._matches.length) %
      this._matches.length;
    this.scrollToCurrentMatch();
  }

  /**
   * Check if a track matches the current search and return highlight info.
   * Returns undefined if there's no match.
   */
  getMatchForTrack(
    node: TrackNode,
  ): {start: number; length: number} | undefined {
    if (!this._searchTerm) return undefined;

    const match = this._matches.find((m) => m.node === node);
    if (match) {
      return {start: match.matchStart, length: match.matchLength};
    }
    return undefined;
  }

  /**
   * Check if a track is the current match (for special highlighting).
   */
  isCurrentMatch(node: TrackNode): boolean {
    const current = this.currentMatch;
    return current !== undefined && current.node === node;
  }

  /**
   * Scrolls to the current match, expanding parent groups if necessary.
   */
  scrollToCurrentMatch(): void {
    const match = this.currentMatch;
    if (match && this._trackManager) {
      // Expand all parent groups so the track becomes visible
      match.node.reveal();
      // Use the existing scroll-to-track mechanism
      this._trackManager.scrollToTrackNodeId = match.node.id;
    }
  }

  private performSearch(preserveCurrentMatch = false): void {
    // Remember the current match node before re-searching
    const previousMatchNode = this.currentMatch?.node;

    this._matches = [];

    if (!this._searchTerm || !this._workspace) {
      this._currentMatchIndex = -1;
      return;
    }

    const tracksToSearch = this.getSearchableTracks();

    if (this._useRegex) {
      // Regex search
      let regex: RegExp;
      try {
        regex = new RegExp(this._searchTerm, 'i');
      } catch {
        // Invalid regex, no matches
        this._currentMatchIndex = -1;
        return;
      }

      for (const node of tracksToSearch) {
        const match = regex.exec(node.name);
        if (match) {
          this._matches.push({
            node,
            matchStart: match.index,
            matchLength: match[0].length,
          });
        }
      }
    } else {
      // Plain text search (case-insensitive)
      const searchTermLower = this._searchTerm.toLowerCase();

      for (const node of tracksToSearch) {
        const nameLower = node.name.toLowerCase();
        const matchIndex = nameLower.indexOf(searchTermLower);

        if (matchIndex !== -1) {
          this._matches.push({
            node,
            matchStart: matchIndex,
            matchLength: this._searchTerm.length,
          });
        }
      }
    }

    if (this._matches.length > 0) {
      if (preserveCurrentMatch && previousMatchNode) {
        // Try to find the previous match node in the new matches
        const newIndex = this._matches.findIndex(
          (m) => m.node === previousMatchNode,
        );
        if (newIndex !== -1) {
          this._currentMatchIndex = newIndex;
          return; // Don't scroll, we're already there
        }
      }
      // Reset to first match
      this._currentMatchIndex = 0;
      this.scrollToCurrentMatch();
    } else {
      this._currentMatchIndex = -1;
    }
  }

  private getSearchableTracks(): TrackNode[] {
    if (!this._workspace) return [];

    const result: TrackNode[] = [];

    const collectTracks = (node: TrackNode) => {
      if (!node.headless) {
        result.push(node);
      }

      if (this._searchCollapsed || node.headless || node.expanded) {
        for (const child of node.children) {
          collectTracks(child);
        }
      }
    };

    // Pinned tracks
    for (const node of this._workspace.pinnedTracks) {
      collectTracks(node);
    }

    // Main tracks
    for (const node of this._workspace.children) {
      collectTracks(node);
    }

    return result;
  }
}

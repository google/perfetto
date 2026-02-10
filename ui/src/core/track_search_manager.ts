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

import {TrackNode} from '../public/workspace';
import {TrackManagerImpl} from './track_manager';

export interface TrackSearchMatch {
  readonly node: TrackNode;
  readonly matchStart: number;
  readonly matchLength: number;
}

/**
 * Manages track search state for the timeline.
 *
 * Unlike the track filter (which hides non-matching tracks), track search:
 * - Keeps all tracks visible
 * - Highlights matching parts of track names
 * - Allows navigation between matches with Enter/Shift+Enter
 */
export class TrackSearchManager {
  private _searchTerm = '';
  private _matches: TrackSearchMatch[] = [];
  private _currentMatchIndex = -1;
  private _isVisible = false;
  private _searchCollapsed = false;
  private _useRegex = false;
  private _trackManager?: TrackManagerImpl;

  // The list of track nodes to search through.
  // This is updated by TrackTreeView during rendering.
  private _visibleTracks: TrackNode[] = [];
  private _allTracks: TrackNode[] = [];

  /**
   * Set the track manager reference for scrolling support.
   */
  setTrackManager(trackManager: TrackManagerImpl): void {
    this._trackManager = trackManager;
  }

  get searchTerm(): string {
    return this._searchTerm;
  }

  get matches(): ReadonlyArray<TrackSearchMatch> {
    return this._matches;
  }

  get currentMatchIndex(): number {
    return this._currentMatchIndex;
  }

  get currentMatch(): TrackSearchMatch | undefined {
    if (
      this._currentMatchIndex >= 0 &&
      this._currentMatchIndex < this._matches.length
    ) {
      return this._matches[this._currentMatchIndex];
    }
    return undefined;
  }

  get matchCount(): number {
    return this._matches.length;
  }

  get isVisible(): boolean {
    return this._isVisible;
  }

  get searchCollapsed(): boolean {
    return this._searchCollapsed;
  }

  set searchCollapsed(value: boolean) {
    if (this._searchCollapsed !== value) {
      this._searchCollapsed = value;
      // Re-run search with the appropriate track list
      if (this._searchTerm) {
        this.performSearch(true /* preserveCurrentMatch */);
      }
    }
  }

  get useRegex(): boolean {
    return this._useRegex;
  }

  set useRegex(value: boolean) {
    if (this._useRegex !== value) {
      this._useRegex = value;
      // Re-run search with new mode
      if (this._searchTerm) {
        this.performSearch(true /* preserveCurrentMatch */);
      }
    }
  }

  /**
   * Updates the list of tracks to search through.
   * Called by TrackTreeView during rendering.
   * @param visibleTracks - Tracks that are currently visible (expanded)
   * @param allTracks - All tracks including those inside collapsed groups
   */
  setTracks(visibleTracks: TrackNode[], allTracks: TrackNode[]): void {
    this._visibleTracks = visibleTracks;
    this._allTracks = allTracks;
    // Re-run search with new tracks, preserving current match
    if (this._searchTerm) {
      this.performSearch(true /* preserveCurrentMatch */);
    }
  }

  /**
   * Sets the search term and performs the search.
   */
  setSearchTerm(term: string): void {
    const termChanged = this._searchTerm !== term;
    this._searchTerm = term;
    // Only reset to first match if the search term actually changed
    this.performSearch(!termChanged /* preserveCurrentMatch */);
  }

  /**
   * Shows the search overlay.
   */
  show(): void {
    this._isVisible = true;
  }

  /**
   * Hides the search overlay and clears the search.
   */
  hide(): void {
    this._isVisible = false;
    this._searchTerm = '';
    this._matches = [];
    this._currentMatchIndex = -1;
  }

  /**
   * Navigate to the next match (with wrap-around).
   */
  stepForward(): void {
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

    if (!this._searchTerm) {
      this._currentMatchIndex = -1;
      return;
    }

    const tracksToSearch = this._searchCollapsed
      ? this._allTracks
      : this._visibleTracks;

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
}

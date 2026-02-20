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
import {z} from 'zod';
import {assertTrue} from '../../base/assert';
import {Card, CardStack} from '../../widgets/card';
import {Button} from '../../widgets/button';
import {Icons} from '../../base/semantic_icons';
import {ExplorePageState} from './explore_page';
import {serializeState} from './json_handler';
import {getAllNodes} from './query_builder/graph_utils';

const RECENT_GRAPHS_KEY = 'recentExploreGraphs';

// Schema for a single recent graph entry
const RECENT_GRAPH_ENTRY_SCHEMA = z.object({
  name: z.string(),
  json: z.string(),
  timestamp: z.number(),
  nodeCount: z.number().optional(),
  labelCount: z.number().optional(),
  starred: z.boolean().default(false),
});

export type RecentGraphEntry = z.infer<typeof RECENT_GRAPH_ENTRY_SCHEMA>;

const RECENT_GRAPHS_SCHEMA = z.array(RECENT_GRAPH_ENTRY_SCHEMA);

export type RecentGraphs = z.infer<typeof RECENT_GRAPHS_SCHEMA>;

/**
 * Storage class for recent explore graphs.
 * Stores serialized graph states in localStorage.
 *
 * The data array uses index 0 as a "working slot" for the current graph being
 * edited. When the user switches to a different graph (via import, example, or
 * clear), the current graph is "finalized" - it gets a proper name and becomes
 * a historical entry at index 1+. A new empty placeholder is then inserted at
 * index 0 for the next graph.
 *
 * Historical graphs (index 1+) are displayed in the Recent Graphs section.
 * The working slot (index 0) is never displayed to the user.
 */
export class RecentGraphsStorage {
  private _data: RecentGraphs;
  maxItems = 10;

  constructor() {
    this._data = this.load();
  }

  /**
   * Returns the recent graphs data. Historical graphs start at index 1.
   * Index 0 is the current working graph (not displayed in UI).
   */
  get data(): RecentGraphs {
    return this._data;
  }

  /**
   * Sets the data directly. Used for testing.
   */
  set data(value: RecentGraphs) {
    this._data = value;
  }

  /**
   * Saves the current graph state. This updates the working slot (index 0),
   * or creates it if the list is empty.
   * Called on every state change to persist the current work.
   */
  saveCurrentState(state: ExplorePageState): void {
    // Don't save empty graphs
    if (state.rootNodes.length === 0) {
      return;
    }

    const json = serializeState(state);
    const timestamp = Date.now();
    const nodeCount = getAllNodes(state.rootNodes).length;
    const labelCount = state.labels.length;

    if (this._data.length === 0) {
      // No entries yet - create the working slot
      this._data.unshift({
        name: this.generateName(),
        json,
        timestamp,
        nodeCount,
        labelCount,
        starred: false,
      });
    } else {
      // Update the working slot in place
      this._data[0].json = json;
      this._data[0].timestamp = timestamp;
      this._data[0].nodeCount = nodeCount;
      this._data[0].labelCount = labelCount;
    }
    this.save();
  }

  /**
   * Finalizes the current graph and prepares for a new one.
   * Called when the user switches to a different graph (New graph, tutorial, etc.).
   * The working slot (index 0) becomes a historical entry (moved to index 1+).
   */
  finalizeCurrentGraph(): void {
    const hasWorkingGraph =
      this._data.length > 0 && (this._data[0].nodeCount ?? 0) > 0;
    if (!hasWorkingGraph) {
      return;
    }

    // Give the current graph a proper name based on when it was finalized
    this._data[0].name = this.generateName();

    // Count unstarred items (excluding working slot which will become historical)
    let lastUnstarredIndex = -1;
    let unstarredCount = 0;
    for (let i = 0; i < this._data.length; i++) {
      if (!this._data[i].starred) {
        unstarredCount++;
        lastUnstarredIndex = i;
      }
    }

    // If we're at max unstarred capacity, remove the oldest unstarred
    // (but not the working slot at index 0)
    if (unstarredCount >= this.maxItems && lastUnstarredIndex > 0) {
      this._data.splice(lastUnstarredIndex, 1);
    }

    // Insert a new empty working slot at index 0
    // This pushes the finalized graph to index 1
    this._data.unshift({
      name: 'Current',
      json: '',
      timestamp: Date.now(),
      nodeCount: 0,
      starred: false,
    });

    this.save();
  }

  /**
   * Gets the most recent valid graph's JSON for restoring state on page load.
   * Skips empty working slots and returns the first graph with actual content.
   */
  getCurrentJson(): string | undefined {
    for (const entry of this._data) {
      if (entry.json && (entry.nodeCount ?? 0) > 0) {
        return entry.json;
      }
    }
    return undefined;
  }

  /**
   * Sets the starred status of a graph.
   */
  setStarred(index: number, starred: boolean): void {
    assertTrue(index >= 0 && index < this._data.length);
    this._data[index].starred = starred;
    this.save();
  }

  /**
   * Renames a graph.
   */
  rename(index: number, newName: string): void {
    assertTrue(index >= 0 && index < this._data.length);
    this._data[index].name = newName.trim() || this._data[index].name;
    this.save();
  }

  /**
   * Generates a unique name for a new graph based on timestamp.
   */
  generateName(): string {
    const now = new Date();
    const dateStr = now.toLocaleDateString('en-US', {
      month: 'short',
      day: 'numeric',
    });
    const timeStr = now.toLocaleTimeString('en-US', {
      hour: '2-digit',
      minute: '2-digit',
      hour12: false,
    });
    return `Graph ${dateStr} ${timeStr}`;
  }

  /**
   * Gets a graph's JSON by index.
   */
  getJson(index: number): string | undefined {
    if (index >= 0 && index < this._data.length) {
      return this._data[index].json;
    }
    return undefined;
  }

  /**
   * Removes a graph from the list by index.
   */
  remove(index: number): void {
    assertTrue(index >= 0 && index < this._data.length);
    this._data.splice(index, 1);
    this.save();
  }

  /**
   * Clears all stored data. Used when stored data is corrupted.
   */
  clear(): void {
    this._data = [];
    this.save();
  }

  private load(): RecentGraphs {
    const value = window.localStorage.getItem(RECENT_GRAPHS_KEY);
    if (value === null) {
      return [];
    }
    try {
      const res = RECENT_GRAPHS_SCHEMA.safeParse(JSON.parse(value));
      return res.success ? res.data : [];
    } catch {
      // Invalid JSON in localStorage, return empty array
      return [];
    }
  }

  private save(): void {
    try {
      window.localStorage.setItem(
        RECENT_GRAPHS_KEY,
        JSON.stringify(this._data),
      );
    } catch (e) {
      // Handle localStorage quota exceeded or other storage errors.
      // Log the error but don't crash - recent graphs is a nice-to-have feature.
      console.warn('Failed to save recent graphs to localStorage:', e);
    }
  }
}

// Singleton instance
export const recentGraphsStorage = new RecentGraphsStorage();

export interface RecentGraphsSectionAttrs {
  readonly onLoadGraph: (json: string) => void;
}

interface RecentGraphCardAttrs {
  readonly entry: RecentGraphEntry;
  readonly index: number;
  readonly onLoadGraph: (json: string) => void;
}

/**
 * Component that renders a single recent graph card.
 */
class RecentGraphCard implements m.ClassComponent<RecentGraphCardAttrs> {
  private isEditing = false;
  private editName = '';

  private startEditing(entry: RecentGraphEntry, e: Event): void {
    e.stopPropagation();
    this.isEditing = true;
    this.editName = entry.name;
  }

  private finishEditing(index: number): void {
    if (this.editName.trim()) {
      recentGraphsStorage.rename(index, this.editName);
    }
    this.isEditing = false;
  }

  private cancelEditing(): void {
    this.isEditing = false;
  }

  view({attrs}: m.CVnode<RecentGraphCardAttrs>): m.Children {
    const {entry, index} = attrs;

    if (this.isEditing) {
      return m(
        Card,
        {
          className: 'pf-recent-graph-card',
          onclick: (e: Event) => e.stopPropagation(),
        },
        m(
          'div',
          m('input', {
            type: 'text',
            value: this.editName,
            oninput: (e: Event) => {
              this.editName = (e.target as HTMLInputElement).value;
            },
            onkeydown: (e: KeyboardEvent) => {
              if (e.key === 'Enter') {
                e.preventDefault();
                this.finishEditing(index);
              } else if (e.key === 'Escape') {
                e.preventDefault();
                this.cancelEditing();
              }
            },
            oncreate: (vnode: m.VnodeDOM<unknown>) => {
              (vnode.dom as HTMLInputElement).focus();
              (vnode.dom as HTMLInputElement).select();
            },
          }),
          m('p', `${entry.nodeCount ?? '?'} nodes`),
        ),
        m(
          '.pf-recent-graph-card__actions',
          m(Button, {
            icon: 'check',
            title: 'Save',
            onclick: (e: Event) => {
              e.stopPropagation();
              this.finishEditing(index);
            },
          }),
          m(Button, {
            icon: 'close',
            title: 'Cancel',
            onclick: (e: Event) => {
              e.stopPropagation();
              this.cancelEditing();
            },
          }),
        ),
      );
    }

    const nodeCount = entry.nodeCount ?? 0;
    const labelCount = entry.labelCount ?? 0;

    return m(
      Card,
      {
        interactive: true,
        className: 'pf-recent-graph-card',
        onclick: () => {
          const json = recentGraphsStorage.getJson(index);
          if (json !== undefined) {
            attrs.onLoadGraph(json);
          }
        },
      },
      m(Button, {
        icon: Icons.Star,
        iconFilled: entry.starred,
        title: entry.starred ? 'Unstar' : 'Star',
        onclick: (e: Event) => {
          e.stopPropagation();
          recentGraphsStorage.setStarred(index, !entry.starred);
        },
      }),
      m(
        'div',
        m('h3', entry.name),
        m('p', `${nodeCount} nodes, ${labelCount} labels`),
      ),
      m(
        '.pf-recent-graph-card__actions',
        m(Button, {
          icon: 'edit',
          title: 'Rename',
          onclick: (e: Event) => this.startEditing(entry, e),
        }),
        m(Button, {
          icon: Icons.Delete,
          title: 'Remove from recent',
          onclick: (e: Event) => {
            e.stopPropagation();
            recentGraphsStorage.remove(index);
          },
        }),
      ),
    );
  }
}

/**
 * Component that renders the "Recent graphs" section in the sidebar.
 * Shows historical graphs (not the current one at index 0).
 * Starred graphs appear first, followed by unstarred graphs.
 * Always shows the section header, even when empty.
 */
export class RecentGraphsSection
  implements m.ClassComponent<RecentGraphsSectionAttrs>
{
  view({attrs}: m.CVnode<RecentGraphsSectionAttrs>): m.Children {
    const recentGraphs = recentGraphsStorage.data;

    // Separate starred and unstarred graphs while preserving indices
    // Start from index 1 to skip the current graph (index 0)
    const starred: Array<{entry: RecentGraphEntry; index: number}> = [];
    const unstarred: Array<{entry: RecentGraphEntry; index: number}> = [];

    for (let i = 1; i < recentGraphs.length; i++) {
      const entry = recentGraphs[i];
      // Skip empty placeholders (working slots with no content)
      if ((entry.nodeCount ?? 0) === 0) continue;

      if (entry.starred) {
        starred.push({entry, index: i});
      } else {
        unstarred.push({entry, index: i});
      }
    }

    // Render starred first, then unstarred
    const allCards = [...starred, ...unstarred];

    return m(
      '.pf-recent-graphs-section',
      m('h4.pf-starting-section-title', 'Recent graphs'),
      allCards.length > 0
        ? m(
            '.pf-recent-graphs-scroll-container',
            m(
              CardStack,
              allCards.map(({entry, index}) =>
                m(RecentGraphCard, {
                  key: index,
                  entry,
                  index,
                  onLoadGraph: attrs.onLoadGraph,
                }),
              ),
            ),
          )
        : m('.pf-recent-graphs-empty', 'No recent graphs'),
    );
  }
}

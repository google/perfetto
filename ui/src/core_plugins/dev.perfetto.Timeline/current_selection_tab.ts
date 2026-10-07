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

import m from 'mithril';
import {raf} from '../../core/raf_scheduler';
import type {TraceImpl} from '../../core/trace_impl';
import {DetailsShell} from '../../widgets/details_shell';
import {EmptyState} from '../../widgets/empty_state';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import type {
  AreaSelection,
  NoteSelection,
  Selection,
  SelectionTab,
  TrackEventSelection,
  TrackSelection,
} from '../../public/selection';
import {assertUnreachable} from '../../base/assert';
import {Button, ButtonBar} from '../../widgets/button';
import {NoteEditor} from './note_editor';
import {Gate} from '../../base/mithril_utils';

interface TabEntry {
  readonly id: string;
  readonly name: string;
  readonly content: m.Children;
  readonly isLoading: boolean;
  readonly buttons?: m.Children;
}

function renderTabs(
  tabs: ReadonlyArray<SelectionTab>,
  selection: Selection,
): TabEntry[] {
  return tabs
    .slice()
    .sort((a, b) => (b.priority ?? 0) - (a.priority ?? 0))
    .flatMap((tab) => {
      const content = tab.render(selection);
      if (!content) return [];
      return [
        {
          id: tab.id,
          name: tab.name,
          content: content.content,
          isLoading: content.isLoading,
          buttons: content.buttons,
        },
      ];
    });
}

function renderTabbedDetails(
  trace: TraceImpl,
  title: string,
  tabs: ReadonlyArray<TabEntry>,
) {
  if (tabs.length === 0) {
    return undefined;
  }

  // Find the active tab or just pick the first one if that selected tab is
  // not available.
  const activeTab =
    tabs.find((tab) => tab.id === trace.selection.currentSelectionSubTab) ??
    tabs[0];

  // Determine if any tab content is loading
  const isLoading = tabs.some((tab) => tab.isLoading);

  return {
    isLoading,
    content: m(
      DetailsShell,
      {
        title,
        description: m(
          ButtonBar,
          tabs.map((tab) =>
            m(Button, {
              label: tab.name,
              key: tab.id,
              active: activeTab === tab,
              onclick: () => trace.selection.setCurrentSelectionSubTab(tab.id),
            }),
          ),
        ),
        buttons: activeTab.buttons,
      },
      // Render all tabs but control visibility with Gate
      tabs.map((tab) => m(Gate, {open: activeTab === tab}, tab.content)),
    ),
  };
}

export interface CurrentSelectionTabAttrs {
  readonly trace: TraceImpl;
}

export class CurrentSelectionTab implements m.ClassComponent<CurrentSelectionTabAttrs> {
  private readonly fadeContext = new FadeContext();

  view({attrs}: m.Vnode<CurrentSelectionTabAttrs>): m.Children {
    const section = this.renderCurrentSelectionTabContent(attrs.trace);
    if (section.isLoading) {
      return m(FadeIn, section.content);
    } else {
      return m(FadeOut, {context: this.fadeContext}, section.content);
    }
  }

  private renderCurrentSelectionTabContent(trace: TraceImpl) {
    const selection = trace.selection.selection;
    const selectionKind = selection.kind;

    switch (selectionKind) {
      case 'empty':
        return this.renderEmptySelection('Nothing selected');
      case 'track':
        return this.renderTrackSelection(trace, selection);
      case 'track_event':
        return this.renderTrackEventSelection(trace, selection);
      case 'area':
        return this.renderAreaSelection(trace, selection);
      case 'note':
        return this.renderNoteSelection(trace, selection);
      default:
        assertUnreachable(selectionKind);
    }
  }

  private renderEmptySelection(message: string) {
    return {
      isLoading: false,
      content: m(EmptyState, {
        fillHeight: true,
        title: message,
      }),
    };
  }

  private renderTrackSelection(trace: TraceImpl, selection: TrackSelection) {
    return {
      isLoading: false,
      content: this.renderTrackDetailsPanel(trace, selection.trackUri),
    };
  }

  private renderTrackEventSelection(
    trace: TraceImpl,
    selection: TrackEventSelection,
  ) {
    // The selection panel has already loaded the details panel for us... let's
    // hope it's the right one!
    const detailsPanel = trace.selection.getDetailsPanelForSelection();
    const extraTabs = renderTabs(trace.selection.selectionTabs, selection);

    if (extraTabs.length === 0) {
      if (detailsPanel) {
        return {
          isLoading: detailsPanel.isLoading,
          content: detailsPanel.render(),
        };
      }
      return {
        isLoading: true,
        content: 'Loading...',
      };
    }

    const allTabs: TabEntry[] = [
      {
        id: 'overview',
        name: 'Details',
        content: detailsPanel ? detailsPanel.render() : 'Loading...',
        isLoading: detailsPanel ? detailsPanel.isLoading : true,
      },
      ...extraTabs,
    ];

    return renderTabbedDetails(trace, 'Selection', allTabs)!;
  }

  private renderAreaSelection(trace: TraceImpl, selection: AreaSelection) {
    const tabs = renderTabs(trace.selection.selectionTabs, selection);
    return (
      renderTabbedDetails(trace, 'Area Selection', tabs) ??
      this.renderEmptySelection('No details available for selection')
    );
  }

  private renderNoteSelection(trace: TraceImpl, selection: NoteSelection) {
    return {
      isLoading: false,
      content: m(NoteEditor, {trace, selection}),
    };
  }

  private renderTrackDetailsPanel(trace: TraceImpl, trackUri: string) {
    const track = trace.tracks.getTrack(trackUri);
    if (track) {
      return m(
        DetailsShell,
        {title: 'Track', description: track.uri},
        m(
          GridLayout,
          m(
            GridLayoutColumn,
            m(
              Section,
              {title: 'Details'},
              m(
                Tree,
                m(TreeNode, {left: 'URI', right: track.uri}),
                m(TreeNode, {left: 'Plugin ID', right: track.pluginId}),
                m(
                  TreeNode,
                  {left: 'Tags'},
                  track.tags &&
                    Object.entries(track.tags).map(([key, value]) => {
                      return m(TreeNode, {left: key, right: value?.toString()});
                    }),
                ),
              ),
            ),
          ),
        ),
      );
    } else {
      return undefined; // TODO show something sensible here
    }
  }
}

const FADE_TIME_MS = 50;

class FadeContext {
  private resolver = () => {};

  putResolver(res: () => void) {
    this.resolver = res;
  }

  resolve() {
    this.resolver();
    this.resolver = () => {};
  }
}

interface FadeOutAttrs {
  readonly context: FadeContext;
}

class FadeOut implements m.ClassComponent<FadeOutAttrs> {
  onbeforeremove({attrs}: m.VnodeDOM<FadeOutAttrs>): Promise<void> {
    return new Promise((res) => {
      attrs.context.putResolver(res);
      setTimeout(res, FADE_TIME_MS);
    });
  }

  oncreate({attrs}: m.VnodeDOM<FadeOutAttrs>) {
    attrs.context.resolve();
  }

  view(vnode: m.Vnode<FadeOutAttrs>): void | m.Children {
    return vnode.children;
  }
}

class FadeIn implements m.ClassComponent {
  private show = false;

  oncreate(_: m.VnodeDOM) {
    setTimeout(() => {
      this.show = true;
      raf.scheduleFullRedraw();
    }, FADE_TIME_MS);
  }

  view(vnode: m.Vnode): m.Children {
    return this.show ? vnode.children : undefined;
  }
}

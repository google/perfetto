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
import {TraceImpl} from '../../core/trace_impl';
import {DetailsShell} from '../../widgets/details_shell';
import {EmptyState} from '../../widgets/empty_state';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  AreaSelection,
  NoteSelection,
  TrackSelection,
} from '../../public/selection';
import {assertUnreachable} from '../../base/logging';
import {Button, ButtonBar} from '../../widgets/button';
import {NoteEditor} from '../note_editor';
import {Gate} from '../../base/mithril_utils';

export interface CurrentSelectionTabAttrs {
  readonly trace: TraceImpl;
}

export class CurrentSelectionTab
  implements m.ClassComponent<CurrentSelectionTabAttrs>
{
  private readonly fadeContext = new FadeContext();
  private currentAreaSubTabId?: string;

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
        return this.renderTrackEventSelection(trace);
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

  private renderTrackEventSelection(trace: TraceImpl) {
    // The selection panel has already loaded the details panel for us... let's
    // hope it's the right one!
    const detailsPanel = trace.selection.getDetailsPanelForSelection();
    if (detailsPanel) {
      return {
        isLoading: detailsPanel.isLoading,
        content: detailsPanel.render(),
      };
    } else {
      return {
        isLoading: true,
        content: 'Loading...',
      };
    }
  }

  private renderAreaSelection(trace: TraceImpl, selection: AreaSelection) {
    const tabs = trace.selection.areaSelectionTabs.sort(
      (a, b) => (b.priority ?? 0) - (a.priority ?? 0),
    );

    const renderedTabs = tabs
      .map((tab) => [tab, tab.render(selection)] as const)
      .filter(([_, content]) => content !== undefined);

    if (renderedTabs.length === 0) {
      return this.renderEmptySelection('No details available for selection');
    }

    // Find the active tab or just pick the first one if that selected tab is
    // not available.
    const [activeTab, activeTabContent] =
      renderedTabs.find(([tab]) => tab.id === this.currentAreaSubTabId) ??
      renderedTabs[0];

    // Determine if any tab content is loading
    const isLoading = renderedTabs.some(([_, content]) => content?.isLoading);

    return {
      isLoading,
      content: m(
        DetailsShell,
        {
          title: 'Area Selection',
          description: m(
            ButtonBar,
            renderedTabs.map(([tab]) => {
              return m(Button, {
                label: tab.name,
                key: tab.id,
                active: activeTab === tab,
                onclick: () => (this.currentAreaSubTabId = tab.id),
              });
            }),
          ),
          buttons: activeTabContent?.buttons,
        },
        // Render all tabs but control visibility with Gate
        renderedTabs.map(([tab, content]) =>
          m(Gate, {open: activeTab === tab}, content?.content),
        ),
      ),
    };
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

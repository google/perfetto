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

export interface CurrentSelectionTabAttrs {
  readonly trace: TraceImpl;
}

export class CurrentSelectionTab
  implements m.ClassComponent<CurrentSelectionTabAttrs>
{
  private readonly fadeContext = new FadeContext();

  view({attrs}: m.Vnode<CurrentSelectionTabAttrs>): m.Children {
    const section = this.renderCSTabContent(attrs.trace);
    if (section.isLoading) {
      return m(FadeIn, section.content);
    } else {
      return m(FadeOut, {context: this.fadeContext}, section.content);
    }
  }

  private renderCSTabContent(trace: TraceImpl): {
    isLoading: boolean;
    content: m.Children;
  } {
    const currentSelection = trace.selection.selection;

    switch (currentSelection.kind) {
      case 'empty':
        return {
          isLoading: false,
          content: m(
            EmptyState,
            {
              className: 'pf-noselection',
              title: 'Nothing selected',
            },
            'Selection details will appear here',
          ),
        };
      case 'track':
        return {
          isLoading: false,
          content: this.renderTrackDetailsPanel(
            trace,
            currentSelection.trackUri,
          ),
        };
      case 'track_event':
        const detailsPanel = trace.selection.getDetailsPanelForSelection();
        if (detailsPanel) {
          return {
            isLoading: detailsPanel.isLoading,
            content: detailsPanel.render(),
          };
        }
        break;
    }

    // Get the first "truthy" details panel
    const panel = trace.tabs.detailsPanels
      .map((dp) => {
        return {
          content: dp.render(currentSelection),
          isLoading: dp.isLoading?.() ?? false,
        };
      })
      .find(({content}) => content);

    if (panel) {
      return panel;
    } else {
      return {
        isLoading: false,
        content: m(
          EmptyState,
          {
            className: 'pf-noselection',
            title: 'No details available',
            icon: 'warning',
          },
          `Selection kind: '${currentSelection.kind}'`,
        ),
      };
    }
  }

  private renderTrackDetailsPanel(trace: TraceImpl, trackUri: string) {
    const track = trace.tracks.getTrack(trackUri);
    if (track) {
      return m(
        DetailsShell,
        {title: 'Track', description: track.title},
        m(
          GridLayout,
          m(
            GridLayoutColumn,
            m(
              Section,
              {title: 'Details'},
              m(
                Tree,
                m(TreeNode, {left: 'Name', right: track.title}),
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

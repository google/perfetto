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

import m from 'mithril';
import {ensureExists} from '../../base/assert';
import {Memo} from '../../base/memo';
import {TreeExplorerPanel} from '../../components/tree_explorer_panel';
import {
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from '../../components/tree_explorer_fetcher';
import type {Trace} from '../../public/trace';
import {Button} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {EmptyState} from '../../widgets/empty_state';
import {HotkeyContext} from '../../widgets/hotkey_context';
import {Stack, StackAuto, StackFixed} from '../../widgets/stack';
import {updateTreeExplorerState} from '../../widgets/tree_explorer';
import {
  getTreeExplorerMergeSelection,
  stepTreeExplorerProfile,
} from '../../widgets/tree_explorer_merge';
import type {AggregateProfile, AggregateProfilesPageState} from './types';

const HIDE_PAGE_EXPLANATION_KEY = 'hideAggregateProfilesPageExplanation';
const HIDE_VIEW_EXPLANATION_KEY = 'hideAggregateProfilesViewExplanation';

export interface AggregateProfilesPageAttrs {
  readonly trace: Trace;
  readonly state: AggregateProfilesPageState;
  // The profiles, in the order the page steps through them.
  readonly profiles: ReadonlyArray<AggregateProfile>;
  // The metrics of the merge of all the profiles.
  readonly mergedMetrics: ReadonlyArray<TreeExplorerQueryMetric>;
  readonly onStateChange: (state: AggregateProfilesPageState) => void;
}

export class AggregateProfilesPage implements m.ClassComponent<AggregateProfilesPageAttrs> {
  // The fetchers (and so the virtual tables built for their metrics) of the
  // merged tree, and of the profile shown on its own. The latter is created
  // for the profile it serves and disposed by the memo when the user shows
  // another profile, so at most one generation of it is alive at a time.
  private readonly mergedFetcherMemo = new Memo<TreeExplorerFetcher>();
  private readonly profileFetcherMemo = new Memo<TreeExplorerFetcher>();

  view({attrs}: m.CVnode<AggregateProfilesPageAttrs>): m.Children {
    if (attrs.profiles.length === 0) {
      return this.renderEmptyState();
    }

    return m(
      HotkeyContext,
      {
        fillHeight: true,
        autoFocus: true,
        hotkeys: [
          {
            hotkey: 'ArrowLeft',
            callback: () => this.stepProfile(attrs, -1),
          },
          {
            hotkey: 'ArrowRight',
            callback: () => this.stepProfile(attrs, 1),
          },
        ],
      },
      m(
        Stack,
        {
          fillHeight: true,
          spacing: 'medium',
          className: 'pf-aggregate-profiles-page',
        },
        [
          this.shouldShowExplanation(HIDE_PAGE_EXPLANATION_KEY) &&
            m(StackFixed, this.renderPageExplanation()),
          this.renderControlsRow(),
          this.shouldShowExplanation(HIDE_VIEW_EXPLANATION_KEY) &&
            m(StackFixed, this.renderViewExplanation()),
          m(StackAuto, [this.renderFlamegraph(attrs)]),
        ],
      ),
    );
  }

  onremove(): void {
    this.mergedFetcherMemo.dispose();
    this.profileFetcherMemo.dispose();
  }

  // Steps through the profiles while they are shown one at a time, like the
  // buttons next to the flamegraph do.
  private stepProfile(attrs: AggregateProfilesPageAttrs, step: number): void {
    const state = updateTreeExplorerState(
      attrs.state.flamegraphState,
      attrs.mergedMetrics,
    );
    const merge = stepTreeExplorerProfile(
      attrs.profiles,
      getTreeExplorerMergeSelection(state),
      step,
    );
    if (merge !== undefined) {
      attrs.onStateChange({
        ...attrs.state,
        flamegraphState: {...state, merge},
      });
    }
  }

  private renderFlamegraph(attrs: AggregateProfilesPageAttrs): m.Children {
    const {trace, profiles, mergedMetrics} = attrs;
    const fetcher = this.mergedFetcherMemo.use({
      key: {},
      compute: () => new TreeExplorerFetcher(trace, mergedMetrics),
    });

    return m(TreeExplorerPanel, {
      fetcher,
      state: attrs.state.flamegraphState,
      onStateChange: (state) => {
        attrs.onStateChange({
          ...attrs.state,
          flamegraphState: state,
        });
      },
      merge: {
        profiles,
        profileFetcher: (key) =>
          this.profileFetcherMemo.use({
            key: {key},
            compute: () => {
              const profile = profiles.find((p) => p.key === key);
              return new TreeExplorerFetcher(
                trace,
                ensureExists(profile).metrics,
              );
            },
          }),
      },
    });
  }

  private shouldShowExplanation(key: string): boolean {
    return localStorage.getItem(key) !== 'true';
  }

  private dismissExplanation(key: string): void {
    localStorage.setItem(key, 'true');
  }

  private showExplanation(key: string): void {
    localStorage.removeItem(key);
  }

  // The page's controls: the help buttons on the right. The view tabs and
  // the profile selector are not here -- they live in the
  // TreeExplorerPanel's own tab bar.
  private renderControlsRow(): m.Children {
    const showViewHelp = !this.shouldShowExplanation(HIDE_VIEW_EXPLANATION_KEY);
    const showPageHelp = this.shouldShowExplanation(HIDE_PAGE_EXPLANATION_KEY);
    if (!showViewHelp && !showPageHelp) {
      return undefined;
    }
    return m(
      StackFixed,
      m(Stack, {orientation: 'horizontal', spacing: 'medium'}, [
        m(StackAuto),
        showViewHelp &&
          m(
            StackFixed,
            m(Button, {
              label: 'About views',
              icon: 'help',
              compact: true,
              onclick: () => this.showExplanation(HIDE_VIEW_EXPLANATION_KEY),
            }),
          ),
        showPageHelp && m(StackFixed, this.renderPageHelpButton()),
      ]),
    );
  }

  private renderPageHelpButton(): m.Children {
    return m(Button, {
      label: 'About page',
      icon: 'help',
      compact: true,
      onclick: () => this.showExplanation(HIDE_PAGE_EXPLANATION_KEY),
    });
  }

  private renderPageExplanation(): m.Children {
    return m(
      Callout,
      {
        icon: 'help',
        dismissible: true,
        onDismiss: () => this.dismissExplanation(HIDE_PAGE_EXPLANATION_KEY),
        className: 'pf-aggregate-profiles-page__page-explanation',
      },
      m(
        'p',
        `This page shows aggregate profile analysis, complementing the timeline view.
         While the timeline visualizes events across time, this page aggregates
         samples from profiles (pprof, collapsed stack, etc.) in the trace.`,
      ),
    );
  }

  private renderViewExplanation(): m.Children {
    return m(
      Callout,
      {
        icon: 'help',
        dismissible: true,
        onDismiss: () => this.dismissExplanation(HIDE_VIEW_EXPLANATION_KEY),
        className: 'pf-aggregate-profiles-page__view-explanation',
      },
      m(
        'p',
        `Flamegraphs display weighted tree structures where the x-axis shows
         proportion and y-axis shows hierarchy depth. Most commonly used for
         call stacks where each rectangle is a function and width shows CPU
         time or sample count. More generally, each rectangle represents a
         node (function, span, allocation site, etc.), helping identify
         hotspots in call stacks, span trees, heap dumps, and other
         hierarchical data.`,
      ),
    );
  }

  private renderEmptyState(): m.Children {
    return m(
      EmptyState,
      {
        icon: 'analytics',
        title: 'No Aggregate Profiles Available',
        fillHeight: true,
        className: 'pf-aggregate-profiles-page__empty',
      },
      [
        m(
          'p',
          'This trace contains no aggregate profiles. ' +
            'Aggregate profiles (pprof, collapsed stack) can be captured using various profiling tools and imported into traces.',
        ),
      ],
    );
  }
}

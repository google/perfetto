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
import {assertExists, assertIsInstance} from '../../base/assert';
import {Memo} from '../../base/memo';
import {maybeUndefined} from '../../base/utils';
import {TreeExplorerPanel} from '../../components/tree_explorer_panel';
import type {Trace} from '../../public/trace';
import {Button} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {EmptyState} from '../../widgets/empty_state';
import {HotkeyContext} from '../../widgets/hotkey_context';
import {Select} from '../../widgets/select';
import {Stack, StackAuto, StackFixed} from '../../widgets/stack';
import {
  createDefaultTreeExplorerState,
  type TreeExplorerState,
  updateTreeExplorerState,
} from '../../widgets/tree_explorer';
import type {AggregateProfile, AggregateProfilesPageState} from './types';

const HIDE_PAGE_EXPLANATION_KEY = 'hideAggregateProfilesPageExplanation';
const HIDE_VIEW_EXPLANATION_KEY = 'hideAggregateProfilesViewExplanation';

export interface AggregateProfilesPageAttrs {
  readonly trace: Trace;
  readonly state: AggregateProfilesPageState;
  readonly profiles: ReadonlyArray<AggregateProfile>;
  readonly onStateChange: (state: AggregateProfilesPageState) => void;
}

export class AggregateProfilesPage implements m.ClassComponent<AggregateProfilesPageAttrs> {
  private readonly memo = new Memo<TreeExplorerState>();

  view({attrs}: m.CVnode<AggregateProfilesPageAttrs>): m.Children {
    // Use the selected profile from the state or just use the first one if none
    // supplied, or if we can't find a match.
    const selectedProfile =
      attrs.profiles.find((p) => p.id === attrs.state.selectedProfileId) ??
      maybeUndefined(attrs.profiles[0]);

    if (selectedProfile === undefined) {
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
          this.renderControlsRow(attrs, selectedProfile),
          this.shouldShowExplanation(HIDE_VIEW_EXPLANATION_KEY) &&
            m(StackFixed, this.renderViewExplanation()),
          m(StackAuto, [this.renderFlamegraph(selectedProfile, attrs)]),
        ],
      ),
    );
  }

  private stepProfile(attrs: AggregateProfilesPageAttrs, step: number): void {
    if (attrs.profiles.length < 2) return;
    const cur = attrs.profiles.findIndex(
      (p) => p.id === attrs.state.selectedProfileId,
    );
    const next = Math.max(cur, 0) + step;
    if (next >= 0 && next < attrs.profiles.length) {
      this.selectProfile(attrs, attrs.profiles[next]);
    }
  }

  private selectProfile(
    attrs: AggregateProfilesPageAttrs,
    profile: AggregateProfile,
  ): void {
    attrs.onStateChange({
      selectedProfileId: profile.id,
      flamegraphState: updateTreeExplorerState(
        attrs.state.flamegraphState,
        profile.metrics,
      ),
    });
  }

  private renderFlamegraph(
    selectedProfile: AggregateProfile,
    attrs: AggregateProfilesPageAttrs,
  ): m.Children {
    // This is a hack necessitated by two issues:
    // 1. TreeExplorerPanel is unable to handle state=undefined despite it being
    //    optional in the attrs interface defintion.
    // 2. The flamegraph compares attrs by reference equality to detect changes,
    //    so while we could simply recreate the state every frame if it's
    //    undefined and avoid the memo entirely - this would trigger an infite
    //    loading loop as we'd have a new object reference every frame.
    let flamegraphState = attrs.state.flamegraphState;
    if (flamegraphState === undefined) {
      flamegraphState = this.memo.use({
        key: selectedProfile.id,
        compute: () => {
          return createDefaultTreeExplorerState(selectedProfile.metrics);
        },
      });
    }

    return m(TreeExplorerPanel, {
      trace: attrs.trace,
      metrics: selectedProfile.metrics,
      state: flamegraphState,
      onStateChange: (state) => {
        attrs.onStateChange({
          ...attrs.state,
          flamegraphState: state,
        });
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

  // The page's controls: the profile selector on the left, the help buttons
  // on the right. The view tabs are not here -- they live in the
  // TreeExplorerPanel's own switcher.
  private renderControlsRow(
    attrs: AggregateProfilesPageAttrs,
    selectedProfile: AggregateProfile,
  ): m.Children {
    const showViewHelp = !this.shouldShowExplanation(HIDE_VIEW_EXPLANATION_KEY);
    const showPageHelp = this.shouldShowExplanation(HIDE_PAGE_EXPLANATION_KEY);
    const showSelector = attrs.profiles.length > 1;
    if (!showViewHelp && !showPageHelp && !showSelector) {
      return undefined;
    }
    return m(
      StackFixed,
      m(Stack, {orientation: 'horizontal', spacing: 'medium'}, [
        showSelector &&
          m(StackFixed, this.renderProfileSelector(attrs, selectedProfile)),
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

  private renderProfileSelector(
    attrs: AggregateProfilesPageAttrs,
    selectedProfile: AggregateProfile,
  ): m.Children {
    return m(Stack, {orientation: 'horizontal', spacing: 'small'}, [
      m(
        'label',
        {className: 'pf-aggregate-profiles-page__profile-label'},
        'Profile:',
      ),
      m(
        Select,
        {
          className: 'pf-aggregate-profiles-page__profile-select',
          oninput: (e: Event) => {
            assertIsInstance(e.target, HTMLSelectElement);
            const newProfileId = e.target.value;
            const newProfile = attrs.profiles.find(
              (p) => p.id === newProfileId,
            );
            assertExists(newProfile); // Assume this profile actually exists
            this.selectProfile(attrs, newProfile);
          },
        },
        attrs.profiles.map((profile) =>
          m(
            'option',
            {
              value: profile.id,
              selected: selectedProfile.id === profile.id,
            },
            profile.displayName,
          ),
        ),
      ),
      // The name is only in <option> text, which the mouse cannot select.
      m(CopyToClipboardButton, {
        textToCopy: () => selectedProfile.displayName,
        tooltip: 'Copy profile name',
      }),
    ]);
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

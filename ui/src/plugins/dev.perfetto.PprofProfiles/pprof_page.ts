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

import {Monitor} from '../../base/monitor';
import {
  QueryFlamegraph,
  QueryFlamegraphWithMetrics,
} from '../../components/query_flamegraph';
import {Trace} from '../../public/trace';
import {Select} from '../../widgets/select';
import {Button} from '../../widgets/button';
import {Stack, StackAuto, StackFixed} from '../../widgets/stack';
import {EmptyState} from '../../widgets/empty_state';
import {Callout} from '../../widgets/callout';
import {TabStrip} from '../../widgets/tabs';
import {Icon} from '../../widgets/icon';
import {Flamegraph} from '../../widgets/flamegraph';
import {PprofProfile, PprofPageState} from './types';

const HIDE_PAGE_EXPLANATION_KEY = 'hidePprofPageExplanation';
const HIDE_VIEW_EXPLANATION_KEY = 'hidePprofViewExplanation';

export interface PprofPageAttrs {
  readonly trace: Trace;
  readonly state: PprofPageState;
  readonly onStateChange: (state: PprofPageState) => void;
  readonly profiles: ReadonlyArray<PprofProfile>;
}

export class PprofPage implements m.ClassComponent<PprofPageAttrs> {
  private profiles?: ReadonlyArray<PprofProfile>;
  private readonly monitor = new Monitor([() => this.profiles]);
  private flamegraphWithMetrics?: QueryFlamegraphWithMetrics;
  private currentTab = 'flamegraph';

  view({attrs}: m.CVnode<PprofPageAttrs>): m.Children {
    this.profiles = attrs.profiles;
    if (this.monitor.ifStateChanged()) {
      const selectedProfile =
        attrs.profiles.find((p) => p.id === attrs.state.selectedProfileId) ||
        (attrs.profiles.length > 0 ? attrs.profiles[0] : undefined);
      attrs.onStateChange({
        flamegraphState: undefined,
        selectedProfileId: selectedProfile?.id,
      });
      if (selectedProfile) {
        this.createFlamegraph(attrs, selectedProfile);
      }
    }
    return m(
      Stack,
      {
        fillHeight: true,
        spacing: 'medium',
        className: 'pf-pprof-page',
      },
      [
        attrs.profiles.length > 1 &&
          m(StackFixed, this.renderControlsRow(attrs)),
        this.shouldShowExplanation(HIDE_PAGE_EXPLANATION_KEY) &&
          m(StackFixed, this.renderPageExplanation()),
        m(
          StackFixed,
          m(Stack, {orientation: 'horizontal', spacing: 'medium'}, [
            m(StackAuto, this.renderTabStrip()),
            this.shouldShowExplanation(HIDE_PAGE_EXPLANATION_KEY) &&
              m(StackFixed, this.renderPageHelpButton()),
          ]),
        ),
        this.shouldShowExplanation(HIDE_VIEW_EXPLANATION_KEY) &&
          m(StackFixed, this.renderViewExplanation()),
        m(StackAuto, [
          this.flamegraphWithMetrics &&
            attrs.state.flamegraphState &&
            this.flamegraphWithMetrics.flamegraph.render({
              metrics: this.flamegraphWithMetrics.metrics,
              state: attrs.state.flamegraphState,
              onStateChange: (state) => {
                attrs.onStateChange({
                  ...attrs.state,
                  flamegraphState: state,
                });
              },
            }),
          !this.flamegraphWithMetrics && this.renderEmptyState(),
        ]),
      ],
    );
  }

  private createFlamegraph(attrs: PprofPageAttrs, profile: PprofProfile): void {
    if (profile.metrics.length === 0) {
      this.flamegraphWithMetrics = undefined;
      attrs.onStateChange({
        ...attrs.state,
        flamegraphState: undefined,
      });
      return;
    }
    attrs.onStateChange({
      ...attrs.state,
      flamegraphState: attrs.state.flamegraphState
        ? Flamegraph.updateState(attrs.state.flamegraphState, profile.metrics)
        : Flamegraph.createDefaultState(profile.metrics),
    });
    this.flamegraphWithMetrics = {
      flamegraph: new QueryFlamegraph(attrs.trace),
      metrics: profile.metrics,
    };
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

  private renderTabStrip(): m.Children {
    const showViewExplanation = this.shouldShowExplanation(
      HIDE_VIEW_EXPLANATION_KEY,
    );
    return m(TabStrip, {
      className: 'pf-pprof-page__tabs',
      tabs: [
        {
          key: 'flamegraph',
          title: 'Flamegraph',
          rightIcon: !showViewExplanation
            ? m(Icon, {
                icon: 'help',
                className: 'pf-pprof-page__help-icon',
                onclick: () => this.showExplanation(HIDE_VIEW_EXPLANATION_KEY),
              })
            : undefined,
        },
        // Future tabs: top-down table, bottom-up table, etc.
      ],
      currentTabKey: this.currentTab,
      onTabChange: (key: string) => {
        this.currentTab = key;
      },
    });
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
        className: 'pf-pprof-page__page-explanation',
      },
      m(
        'p',
        `This page shows pprof profile analysis, complementing the timeline view.
         While the timeline visualizes events across time, this page aggregates
         samples from pprof profiles in the trace.`,
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
        className: 'pf-pprof-page__view-explanation',
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

  private renderControlsRow(attrs: PprofPageAttrs): m.Children {
    return m(
      Stack,
      {
        orientation: 'horizontal',
        spacing: 'medium',
        className: 'pf-pprof-page__controls',
      },
      [
        m(StackAuto),
        m(StackFixed, this.renderProfileSelector(attrs)),
        m(StackAuto),
      ],
    );
  }

  private renderProfileSelector(attrs: PprofPageAttrs): m.Children {
    return m(Stack, {orientation: 'horizontal', spacing: 'small'}, [
      m('label', {className: 'pf-pprof-page__profile-label'}, 'Profile:'),
      m(
        Select,
        {
          className: 'pf-pprof-page__profile-select',
          oninput: (e: Event) => {
            const selectedIndex = parseInt(
              (e.target as HTMLSelectElement).value,
            );
            const newProfile = attrs.profiles[selectedIndex];
            attrs.onStateChange({
              ...attrs.state,
              selectedProfileId: newProfile.id,
            });
            this.createFlamegraph(attrs, newProfile);
          },
        },
        attrs.profiles.map((profile, index) =>
          m(
            'option',
            {
              value: index.toString(),
              selected: attrs.state.selectedProfileId === profile.id,
            },
            profile.displayName,
          ),
        ),
      ),
    ]);
  }

  private renderEmptyState(): m.Children {
    return m(
      EmptyState,
      {
        icon: 'analytics',
        title: 'No pprof Profiles Available',
        fillHeight: true,
        className: 'pf-pprof-page__empty',
      },
      [
        m(
          'p',
          'This trace contains no pprof profiles. ' +
            'pprof profiles can be captured using various profiling tools and imported into traces.',
        ),
      ],
    );
  }
}

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
import {QueryFlamegraph} from '../../components/query_flamegraph';
import {Trace} from '../../public/trace';
import {Select} from '../../widgets/select';
import {Button} from '../../widgets/button';
import {Stack, StackAuto, StackFixed} from '../../widgets/stack';
import {LinearProgress} from '../../widgets/linear_progress';
import {EmptyState} from '../../widgets/empty_state';
import {Callout} from '../../widgets/callout';
import {TabStrip} from '../../widgets/tabs';
import {Icon} from '../../widgets/icon';
import {Popup, PopupPosition} from '../../widgets/popup';
import {
  Flamegraph,
  FLAMEGRAPH_STATE_SCHEMA,
  FlamegraphState,
} from '../../widgets/flamegraph';
import {AggregateScope, AggregatesPageState} from './types';

const HIDE_PAGE_EXPLANATION_KEY = 'hidePageExplanation';
const HIDE_VIEW_EXPLANATION_KEY = 'hideViewExplanation';

type ExplanationType = 'page' | 'view';

export interface AggregatesPageAttrs {
  readonly trace: Trace;
  readonly state: AggregatesPageState;
  readonly onStateUpdate: (update: AggregatesPageState) => void;
  readonly registeredScopes: AggregateScope[];
}

interface SerializationState {
  schema: typeof FLAMEGRAPH_STATE_SCHEMA;
  state: FlamegraphState;
}

export class AggregatesPage implements m.ClassComponent<AggregatesPageAttrs> {
  private currentTrace?: Trace;
  private readonly monitor = new Monitor([() => this.currentTrace]);
  private flamegraph?: QueryFlamegraph;
  private serialization?: SerializationState;
  private loading = false;
  private currentTab = 'flamegraph';
  private showPageHelp = false;
  private showViewHelp = false;
  private initializedPopups = false;

  view({attrs}: m.CVnode<AggregatesPageAttrs>): m.Children {
    this.currentTrace = attrs.trace;
    this.initializeScopesIfNeeded(attrs);

    // Auto-show popups based on localStorage (only once)
    if (!this.initializedPopups) {
      this.initializedPopups = true;
      if (this.shouldShowExplanation('page')) {
        this.showPageHelp = true;
      }
      if (this.shouldShowExplanation('view')) {
        this.showViewHelp = true;
      }
    }

    return this.renderPage(attrs);
  }

  private initializeScopesIfNeeded(attrs: AggregatesPageAttrs): void {
    const shouldInitialize =
      this.monitor.ifStateChanged() &&
      !this.loading &&
      attrs.state.availableScopes.length === 0;
    if (shouldInitialize) {
      this.loading = true;
      this.loadAvailableScopes(attrs).finally(() => {
        this.loading = false;
      });
    }
  }

  private async loadAvailableScopes(attrs: AggregatesPageAttrs): Promise<void> {
    // Filter to only scopes with metrics (metrics are already resolved)
    const availableScopes = attrs.registeredScopes.filter(
      (scope) => scope.metrics.length > 0,
    );

    const selectedScope =
      attrs.state.selectedScope ||
      (availableScopes.length > 0 ? availableScopes[0] : undefined);
    attrs.onStateUpdate({
      selectedScope,
      availableScopes,
    });
    if (selectedScope) {
      this.createFlamegraph(selectedScope);
    }
  }

  private createFlamegraph(scope: AggregateScope): void {
    if (scope.metrics.length === 0) {
      this.flamegraph = undefined;
      this.serialization = undefined;
      return;
    }
    this.serialization = {
      schema: FLAMEGRAPH_STATE_SCHEMA,
      state: Flamegraph.createDefaultState(scope.metrics),
    };
    this.flamegraph = new QueryFlamegraph(
      this.currentTrace!,
      scope.metrics,
      this.serialization,
    );
  }

  private getExplanationKey(type: ExplanationType): string {
    return type === 'page'
      ? HIDE_PAGE_EXPLANATION_KEY
      : HIDE_VIEW_EXPLANATION_KEY;
  }

  private shouldShowExplanation(type: ExplanationType): boolean {
    return localStorage.getItem(this.getExplanationKey(type)) !== 'true';
  }

  private setExplanationVisible(type: ExplanationType, visible: boolean): void {
    if (visible) {
      localStorage.removeItem(this.getExplanationKey(type));
    } else {
      localStorage.setItem(this.getExplanationKey(type), 'true');
    }

    if (type === 'page') {
      this.showPageHelp = visible;
    } else {
      this.showViewHelp = visible;
    }
    m.redraw();
  }

  private renderPage(attrs: AggregatesPageAttrs): m.Children {
    return m(
      Stack,
      {
        fillHeight: true,
        spacing: 'medium',
        className: 'pf-aggregates-page',
      },
      [
        m(StackFixed, this.renderControlsRow(attrs)),
        m(StackFixed, this.renderTabStrip()),
        m(StackAuto, this.renderMainContent(attrs)),
      ],
    );
  }

  private renderTabStrip(): m.Children {
    return m(
      Stack,
      {
        orientation: 'horizontal',
        spacing: 'medium',
      },
      [
        m(
          StackAuto,
          m(TabStrip, {
            className: 'pf-aggregates-page__tabs',
            tabs: [
              {
                key: 'flamegraph',
                title: 'Flamegraph',
                rightIcon: m(
                  Popup,
                  {
                    trigger: m(Icon, {
                      icon: 'help',
                    }),
                    isOpen: this.showViewHelp,
                    position: PopupPosition.Right,
                    onChange: (shouldOpen: boolean) => {
                      this.setExplanationVisible('view', shouldOpen);
                    },
                  },
                  m(
                    Callout,
                    {
                      icon: 'help',
                      dismissible: true,
                      onDismiss: () =>
                        this.setExplanationVisible('view', false),
                      className: 'pf-aggregates-page__view-explanation',
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
                  ),
                ),
              },
              // Future tabs: top-down table, bottom-up table, etc.
            ],
            currentTabKey: this.currentTab,
            onTabChange: (key: string) => {
              this.currentTab = key;
              m.redraw();
            },
          }),
        ),
        m(
          StackFixed,
          m(
            Popup,
            {
              trigger: m(Button, {
                label: 'About page',
                icon: 'help',
                compact: true,
              }),
              isOpen: this.showPageHelp,
              position: PopupPosition.Bottom,
              onChange: (shouldOpen: boolean) => {
                this.setExplanationVisible('page', shouldOpen);
              },
            },
            m(
              Callout,
              {
                icon: 'help',
                dismissible: true,
                onDismiss: () => this.setExplanationVisible('page', false),
                className: 'pf-aggregates-page__page-explanation',
              },
              m(
                'p',
                `This page shows aggregate analysis of profiling data, complementing
             the timeline view. While the timeline visualizes events across time,
             this page aggregates samples from a specific slice of the trace.`,
              ),
            ),
          ),
        ),
      ],
    );
  }

  private renderControlsRow(attrs: AggregatesPageAttrs): m.Children {
    if (attrs.state.availableScopes.length <= 1) {
      return null;
    }

    return m(
      Stack,
      {
        orientation: 'horizontal',
        spacing: 'medium',
        className: 'pf-aggregates-page__controls',
      },
      [
        m(StackAuto),
        m(StackFixed, this.renderProfileSelector(attrs)),
        m(StackAuto),
      ],
    );
  }

  private renderProfileSelector(attrs: AggregatesPageAttrs): m.Children {
    return m(Stack, {orientation: 'horizontal', spacing: 'small'}, [
      m('label', {className: 'pf-aggregates-page__profile-label'}, 'Source:'),
      m(
        Select,
        {
          className: 'pf-aggregates-page__profile-select',
          oninput: (e: Event) => {
            const selectedIndex = parseInt(
              (e.target as HTMLSelectElement).value,
            );
            const newScope = attrs.state.availableScopes[selectedIndex];
            attrs.onStateUpdate({
              ...attrs.state,
              selectedScope: newScope,
            });
            if (newScope !== undefined) {
              this.createFlamegraph(newScope);
            }
          },
        },
        attrs.state.availableScopes.map((scope, index) =>
          m(
            'option',
            {
              value: index.toString(),
              selected: attrs.state.selectedScope === scope,
            },
            scope.displayName,
          ),
        ),
      ),
    ]);
  }

  private renderMainContent(attrs: AggregatesPageAttrs): m.Children {
    return m(
      Stack,
      {
        fillHeight: true,
      },
      [
        this.renderLoadingProgress(),
        this.renderFlamegraph(),
        this.renderLoadingState(attrs),
        this.renderEmptyState(attrs),
      ],
    );
  }

  private renderLoadingProgress(): m.Children {
    if (!this.loading) return null;
    return m(LinearProgress, {
      state: 'indeterminate',
      className: 'pf-aggregates-page__progress',
    });
  }

  private renderFlamegraph(): m.Children {
    if (!this.flamegraph) {
      return null;
    }
    return m(
      'div',
      {className: 'pf-aggregates-page__flamegraph'},
      this.flamegraph.render(),
    );
  }

  private renderLoadingState(attrs: AggregatesPageAttrs): m.Children {
    if (
      this.flamegraph ||
      attrs.state.selectedScope === undefined ||
      !this.loading
    ) {
      return null;
    }
    return m(
      Stack,
      {
        spacing: 'medium',
        className: 'pf-aggregates-page__loading',
      },
      [
        m('div', 'Loading aggregate analysis...'),
        m('div', 'Processing data...'),
      ],
    );
  }

  private renderEmptyState(attrs: AggregatesPageAttrs): m.Children {
    if (attrs.state.availableScopes.length > 0 || this.loading) {
      return null;
    }
    return m(
      EmptyState,
      {
        icon: 'analytics',
        title: 'No Aggregate Data Available',
        fillHeight: true,
        className: 'pf-aggregates-page__empty',
      },
      [
        m(
          'p',
          'This trace contains no aggregate data. ' +
            'Flamegraphs require data such as pprof profiles, slice tracks with hierarchies, or other aggregatable data.',
        ),
      ],
    );
  }
}

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

import './tree_explorer_filter_bar.scss';
import m from 'mithril';
import {fuzzySearch} from '../base/fuzzy';
import {Button} from './button';
import {Chip} from './chip';
import {Intent} from './common';
import {CopyToClipboardButton} from './copy_to_clipboard_button';
import {EmptyState} from './empty_state';
import {
  ExportButton,
  type ExportDownloadItem,
  type ExportFormat,
} from './export_button';
import {Form, FormLabel} from './form';
import {Icon} from './icon';
import {MiddleEllipsis} from './middle_ellipsis';
import {Popup, PopupPosition} from './popup';
import {RadioGroup} from './radio_group';
import {Select} from './select';
import {TagInput} from './tag_input';
import {TextInput} from './text_input';
import {Tooltip} from './tooltip';
import {MenuDivider, MenuItem, PopupMenu} from './menu';
import {Icons} from '../base/semantic_icons';
import {
  FILTER_TYPES,
  type FilterType,
  type PatternViewKind,
  type TreeExplorerAddableMetric,
  type TreeExplorerData,
  type TreeExplorerMetric,
  type TreeExplorerState,
  addFilter,
  metricId,
  parseFilter,
  splitFilters,
  toTags,
} from './tree_explorer';

export interface TreeExplorerFilterBarAttrs {
  readonly metrics: ReadonlyArray<TreeExplorerMetric>;
  readonly state: TreeExplorerState;
  readonly data: TreeExplorerData | undefined;
  readonly onStateChange: (state: TreeExplorerState) => void;

  readonly addableMetrics?: ReadonlyArray<TreeExplorerAddableMetric>;
  readonly onAddMetric?: (metric: TreeExplorerAddableMetric) => void;

  // Highlight state is owned by the caller so sibling views (e.g. the
  // flamegraph canvas) can consume the same pattern.
  readonly highlightPattern: string;
  readonly highlightRegex: RegExp | undefined;
  readonly onHighlightChange: (pattern: string) => void;
  // Greys out the highlight control, e.g. when the active view does not
  // render highlights. The stored pattern is kept.
  readonly highlightDisabled?: boolean;

  // Greys out the Top Down / Bottom Up selector, e.g. in the flat function
  // view where direction has no meaning.
  readonly directionDisabled?: boolean;

  // Builds the exported representation of the currently displayed view.
  // Undefined hides the export button.
  readonly onExportData?: (format: ExportFormat) => Promise<string>;
  readonly exportFileBaseName?: string;

  // Downloads the host produces itself, appended to the export menu for
  // representations this bar cannot build from the displayed tree.
  readonly extraDownloadItems?: ReadonlyArray<ExportDownloadItem>;
}

// The filtering / measure-selection bar shared by all tree explorer views.
// Operates purely on TreeExplorerState: what the filters mean for the
// displayed data is up to the view consuming the state.
export class TreeExplorerFilterBar implements m.ClassComponent<TreeExplorerFilterBarAttrs> {
  private showFilterBuilder = false;
  private quickAddValue = '';
  private showHighlightSearch = false;

  view({attrs}: m.CVnode<TreeExplorerFilterBarAttrs>): m.Children {
    const tags = toTags(attrs.state);
    const hasFilters = tags.length > 0;
    const activePatternView =
      attrs.state.view.kind === 'FROM_FRAME' ||
      attrs.state.view.kind === 'PIVOT'
        ? attrs.state.view.kind
        : undefined;

    const removeTag = (i: number) => {
      if (i === attrs.state.filters.length) {
        attrs.onStateChange({
          ...attrs.state,
          view: {kind: 'TOP_DOWN'},
        });
      } else {
        const filters = attrs.state.filters.filter((_, j) => j !== i);
        attrs.onStateChange({...attrs.state, filters});
      }
    };

    const addFilterFn = (filters: Array<{type: FilterType; value: string}>) => {
      let newState = attrs.state;
      for (const {type, value} of filters) {
        if (type === 'SHOW_FROM_FRAME') {
          newState = {...newState, view: {kind: 'FROM_FRAME', pattern: value}};
        } else if (type === 'PIVOT') {
          newState = {...newState, view: {kind: 'PIVOT', pivot: value}};
        } else {
          newState = addFilter(newState, {kind: type, filter: value});
        }
      }
      attrs.onStateChange(newState);
    };

    return m(
      '.pf-tree-explorer-filter-bar',
      m('span.pf-tree-explorer-filter-bar__label', 'Measure:'),
      this.renderMeasurePicker(attrs),
      m('span.pf-tree-explorer-filter-bar__label', 'Filters:'),
      m(TagInput, {
        tags,
        value: this.quickAddValue,
        onChange: (text) => {
          this.quickAddValue = text;
        },
        onTagAdd: (text) => {
          const filters = splitFilters(text).map((part) => parseFilter(part));
          if (filters.length > 0) {
            addFilterFn(filters);
            this.quickAddValue = '';
          }
        },
        onTagRemove: removeTag,
        placeholder: hasFilters
          ? ''
          : 'e.g. malloc (contains), or /^main$/ for regex; press + for more filter options',
        renderTag: (text, onRemove) =>
          m(Chip, {
            ondblclick: () => {
              this.quickAddValue = text;
              onRemove();
            },
            label: m(MiddleEllipsis, {text}),
            removable: true,
            compact: true,
            intent: Intent.Primary,
            onRemove,
          }),
      }),
      m(
        Popup,
        {
          trigger: m(Button, {
            icon: Icons.Add,
            compact: true,
            active: this.showFilterBuilder,
            onclick: () => {
              this.showFilterBuilder = !this.showFilterBuilder;
            },
          }),
          isOpen: this.showFilterBuilder,
          onChange: (shouldOpen: boolean) => {
            this.showFilterBuilder = shouldOpen;
          },
          position: PopupPosition.RightStart,
          closeOnOutsideClick: true,
          closeOnEscape: true,
          className: 'pf-filter-builder',
        },
        m(FilterBuilder, {
          activePatternView,
          onAdd: addFilterFn,
        }),
      ),
      m(CopyToClipboardButton(), {
        textToCopy: () => tags.join(' '),
        compact: true,
        disabled: !hasFilters,
      }),
      m(Button, {
        icon: 'delete',
        compact: true,
        disabled: !hasFilters,
        onclick: () => {
          attrs.onStateChange({
            ...attrs.state,
            filters: [],
            view:
              activePatternView === undefined
                ? attrs.state.view
                : {kind: 'TOP_DOWN'},
          });
        },
      }),
      m(
        RadioGroup,
        {
          disabled: attrs.directionDisabled,
          title: attrs.directionDisabled
            ? 'Direction does not apply to the current view'
            : undefined,
          selectedValue:
            attrs.state.view.kind === 'TOP_DOWN'
              ? 'top-down'
              : attrs.state.view.kind === 'BOTTOM_UP'
                ? 'bottom-up'
                : undefined,
          onValueChange: (value) => {
            attrs.onStateChange({
              ...attrs.state,
              view: {kind: value === 'top-down' ? 'TOP_DOWN' : 'BOTTOM_UP'},
            });
          },
        },
        [
          m(RadioGroup.Button, {value: 'top-down'}, 'Top Down'),
          m(RadioGroup.Button, {value: 'bottom-up'}, 'Bottom Up'),
        ],
      ),
      m(Button, {
        icon: Icons.Search,
        label: 'Highlight',
        disabled: attrs.highlightDisabled,
        title: attrs.highlightDisabled
          ? 'Highlight does not apply to the current view'
          : undefined,
        active:
          !attrs.highlightDisabled &&
          (this.showHighlightSearch || attrs.highlightPattern !== ''),
        onclick: () => {
          this.showHighlightSearch = !this.showHighlightSearch;
        },
      }),
      attrs.onExportData !== undefined &&
        attrs.data !== undefined &&
        attrs.data.nodes.length > 0 &&
        m(ExportButton, {
          fileBaseName: attrs.exportFileBaseName ?? 'tree_explorer',
          onExportData: attrs.onExportData,
          extraDownloadItems: attrs.extraDownloadItems,
        }),
      this.showHighlightSearch &&
        !attrs.highlightDisabled &&
        m(
          '.pf-tree-explorer-filter-bar__secondary-row',
          m('span.pf-tree-explorer-filter-bar__label', 'Highlight:'),
          this.renderHighlightSearch(attrs),
          attrs.highlightPattern !== '' &&
            m(Button, {
              icon: Icons.Close,
              compact: true,
              title: 'Clear highlight',
              onclick: () => attrs.onHighlightChange(''),
            }),
        ),
    );
  }

  private renderMeasurePicker(attrs: TreeExplorerFilterBarAttrs) {
    const selected = attrs.metrics.find(
      (metric) => metricId(metric) === attrs.state.selectedMetricId,
    );
    if (selected === undefined) {
      return undefined;
    }
    const defaultMetrics = attrs.metrics.filter(
      (metric) => metric.provenance === 'DEFAULT',
    );
    const otherMetrics = attrs.metrics.filter(
      (metric) => metric.provenance !== 'DEFAULT',
    );
    const renderMetric = (metric: TreeExplorerMetric) =>
      m(MenuItem, {
        label: metric.name,
        rightIcon:
          metricId(metric) === attrs.state.selectedMetricId
            ? Icons.Check
            : undefined,
        onclick: () => {
          attrs.onStateChange({
            ...attrs.state,
            selectedMetricId: metricId(metric),
          });
        },
      });

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          label: selected.name,
          rightIcon: Icons.ExpandDown,
        }),
      },
      defaultMetrics.map(renderMetric),
      defaultMetrics.length > 0 && otherMetrics.length > 0 && m(MenuDivider),
      otherMetrics.map(renderMetric),
      attrs.addableMetrics !== undefined &&
        attrs.addableMetrics.length > 0 && [
          m(MenuDivider),
          m(
            MenuItem,
            {label: 'Add measure...', icon: Icons.Add},
            m(AddMetricMenu, {
              metrics: attrs.addableMetrics,
              onSelect: (metric) => attrs.onAddMetric?.(metric),
            }),
          ),
        ],
    );
  }

  private renderHighlightSearch(attrs: TreeExplorerFilterBarAttrs) {
    const {highlightPattern, highlightRegex} = attrs;
    const matchCount =
      highlightRegex === undefined
        ? 0
        : (attrs.data?.nodes.filter((node) => highlightRegex.test(node.name))
            .length ?? 0);
    return m(
      '.pf-tree-explorer-highlight-search',
      m(TextInput, {
        autofocus: true,
        leftIcon: Icons.Search,
        placeholder: 'Name, /Regex/, or /regex/i…',
        value: highlightPattern,
        onInput: (value) => attrs.onHighlightChange(value),
        onkeydown: (event: KeyboardEvent) => {
          if (event.key === 'Escape') {
            attrs.onHighlightChange('');
          }
        },
      }),
      highlightPattern !== '' &&
        m(
          'span.pf-tree-explorer-highlight-search__count',
          highlightRegex === undefined
            ? 'Invalid regex'
            : `${matchCount} ${matchCount === 1 ? 'match' : 'matches'}`,
        ),
    );
  }
}

interface FilterBuilderAttrs {
  readonly activePatternView?: PatternViewKind;
  readonly onAdd: (filters: Array<{type: FilterType; value: string}>) => void;
}

class FilterBuilder implements m.ClassComponent<FilterBuilderAttrs> {
  private type: FilterType = 'SHOW_STACK';
  private filter = '';

  view({attrs}: m.CVnode<FilterBuilderAttrs>) {
    const {onAdd} = attrs;
    const opt = FILTER_TYPES.find((o) => o.value === this.type);
    const replacesPatternView =
      (this.type === 'SHOW_FROM_FRAME' || this.type === 'PIVOT') &&
      attrs.activePatternView !== undefined;

    return m(
      Form,
      {
        submitLabel: 'Add',
        cancelLabel: 'Cancel',
        onSubmit: () => {
          if (!this.filter.trim()) return;
          onAdd([{type: this.type, value: this.filter.trim()}]);
          this.filter = '';
        },
        validation: () => this.filter.trim() !== '',
      },
      m(FormLabel, 'Type'),
      m(
        Select,
        {
          oninput: (e: Event) => {
            this.type = (e.target as HTMLSelectElement).value as FilterType;
          },
        },
        FILTER_TYPES.map((o) => m('option', {value: o.value}, o.friendlyLabel)),
      ),
      opt && m('.pf-filter-builder__desc', opt.description),
      m(FormLabel, 'Filter'),
      m(TextInput, {
        autofocus: true,
        placeholder: 'e.g. malloc',
        value: this.filter,
        onInput: (value) => {
          this.filter = value;
        },
      }),
      m(
        '.pf-filter-builder__hint',
        'Bare text matches literally and case-insensitively (e.g. ',
        m('code', 'malloc'),
        '). Use ',
        m('code', '/…/'),
        ' for a case-sensitive regex (e.g. ',
        m('code', '/.*Alloc.*/'),
        '), or append ',
        m('code', 'i'),
        ' for a case-insensitive regex (e.g. ',
        m('code', '/.*alloc.*/i'),
        ').',
      ),
      replacesPatternView &&
        m(
          '.pf-filter-builder__warn',
          `Replaces the current ${
            attrs.activePatternView === 'PIVOT' ? 'Pivot' : 'Show From Frame'
          } filter.`,
        ),
      m('.pf-filter-builder__separator'),
      opt &&
        m(
          '.pf-filter-builder__tip',
          m(Icon, {icon: 'lightbulb_outline'}),
          ' Tip: type ',
          m('code', `${opt.shortLabel}: ${opt.example}`),
          ' directly in the filter bar ',
          m(
            Tooltip,
            {trigger: m(Icon, {icon: 'help_outline'})},
            m(
              '.pf-filter-builder__help',
              m(
                '.pf-filter-builder__help-title',
                'Filter bar syntax (bare text is case-insensitive; /…/ is a ' +
                  'case-sensitive regex and /…/i is case-insensitive):',
              ),
              FILTER_TYPES.map((o) =>
                m(
                  '.pf-filter-builder__help-row',
                  m('strong', `${o.shortLabel}:`),
                  ` ${o.label}, e.g. `,
                  m('code', `${o.shortLabel}: ${o.example}`),
                ),
              ),
              m(
                '.pf-filter-builder__help-row',
                'Combine operations by separating them with spaces, e.g. ',
                m('code', 'SS: HandleRequest HF: /.*alloc.*/i'),
              ),
            ),
          ),
        ),
    );
  }
}

interface AddMetricMenuAttrs {
  readonly metrics: ReadonlyArray<TreeExplorerAddableMetric>;
  readonly onSelect: (metric: TreeExplorerAddableMetric) => void;
}

class AddMetricMenu implements m.ClassComponent<AddMetricMenuAttrs> {
  private static readonly MAX_VISIBLE_ITEMS = 100;
  private searchQuery = '';

  view({attrs}: m.CVnode<AddMetricMenuAttrs>) {
    const results =
      this.searchQuery === ''
        ? attrs.metrics.map((metric) => ({
            metric,
            segments: [{matching: false, value: metric.name}],
          }))
        : fuzzySearch(
            attrs.metrics,
            (metric) => metric.name,
            this.searchQuery,
          ).map((result) => ({
            metric: result.item,
            segments: result.segments,
          }));
    const visible = results.slice(0, AddMetricMenu.MAX_VISIBLE_ITEMS);
    const remaining = results.length - visible.length;

    return m('.pf-distinct-values-menu', [
      m(
        '.pf-distinct-values-menu__search',
        {
          onclick: (event: MouseEvent) => event.stopPropagation(),
        },
        m(TextInput, {
          placeholder: 'Search measures...',
          value: this.searchQuery,
          oninput: (event: InputEvent) => {
            this.searchQuery = (event.target as HTMLInputElement).value;
          },
          onkeydown: (event: KeyboardEvent) => {
            if (this.searchQuery !== '' && event.key === 'Escape') {
              this.searchQuery = '';
              event.stopPropagation();
            }
          },
        }),
      ),
      m(
        '.pf-distinct-values-menu__list',
        visible.length > 0
          ? [
              visible.map(({metric, segments}) =>
                m(MenuItem, {
                  label: segments.map((segment) =>
                    segment.matching
                      ? m('strong.pf-fuzzy-match', segment.value)
                      : segment.value,
                  ),
                  onclick: () => {
                    attrs.onSelect(metric);
                    this.searchQuery = '';
                  },
                }),
              ),
              remaining > 0 &&
                m(MenuItem, {
                  label: `...and ${remaining} more`,
                  disabled: true,
                }),
            ]
          : m(EmptyState, {title: 'No matches'}),
      ),
    ]);
  }
}

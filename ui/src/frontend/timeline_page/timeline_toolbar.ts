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
import {findRef} from '../../base/dom_utils';
import {Icons} from '../../base/semantic_icons';
import {renderTrackSettingMenu} from '../../components/track_settings_renderer';
import {TraceImpl} from '../../core/trace_impl';
import {AreaSelection, Selection} from '../../public/selection';
import {TrackSetting, TrackSettingDescriptor} from '../../public/track';
import {TrackNode, Workspace} from '../../public/workspace';
import {Button, ButtonVariant} from '../../widgets/button';
import {MenuDivider, MenuItem, MenuTitle, PopupMenu} from '../../widgets/menu';
import {MultiSelectOption, PopupMultiSelect} from '../../widgets/multiselect';
import {Popup} from '../../widgets/popup';
import {Stack, StackAuto} from '../../widgets/stack';
import {TextInput} from '../../widgets/text_input';
import {Intent} from '../../widgets/common';
import {Callout} from '../../widgets/callout';

const FILTER_TEXT_BOX_REF = 'filter-text-box';
const COMPACT_BUTTONS = true;

export interface TimelineToolbarAttrs {
  readonly trace: TraceImpl;
}

export class TimelineToolbar implements m.ClassComponent<TimelineToolbarAttrs> {
  view({attrs}: m.Vnode<TimelineToolbarAttrs>) {
    const trace = attrs.trace;
    const workspace = trace.currentWorkspace;
    const allCollapsed = workspace.flatTracks.every((n) => n.collapsed);
    const selection = trace.selection.selection;

    return m(
      Stack,
      {
        className: 'pf-timeline-toolbar',
        orientation: 'horizontal',
        spacing: 'small',
      },
      m(Button, {
        onclick: (e: Event) => {
          e.preventDefault();
          if (allCollapsed) {
            trace.currentWorkspace.flatTracks.forEach((track) =>
              track.expand(),
            );
          } else {
            trace.currentWorkspace.flatTracks.forEach((track) =>
              track.collapse(),
            );
          }
        },
        title: allCollapsed ? 'Expand all' : 'Collapse all',
        icon: allCollapsed ? 'unfold_more' : 'unfold_less',
        compact: COMPACT_BUTTONS,
      }),
      m(Button, {
        onclick: (e: Event) => {
          e.preventDefault();
          trace.currentWorkspace.pinnedTracks.forEach((t) =>
            trace.currentWorkspace.unpinTrack(t),
          );
        },
        title: 'Unpin all pinned tracks',
        icon: 'keep_off',
        disabled: trace.currentWorkspace.pinnedTracks.length === 0,
        compact: COMPACT_BUTTONS,
      }),
      this.renderTrackFilter(trace),
      this.renderWorkspaceMenu(trace),
      m(StackAuto),
      selection.kind === 'area' && this.renderBulkTracksMenu(trace, selection),
    );
  }

  private renderBulkTracksMenu(trace: TraceImpl, selection: AreaSelection) {
    const settingsMenuItems = this.renderBulkSettingsMenu(trace);

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          rightIcon: 'arrow_drop_down',
          icon: 'check',
          compact: true,
          rounded: true,
          label: `${selection.tracks.length} ${selection.tracks.length === 1 ? 'track' : 'tracks'}`,
          variant: ButtonVariant.Filled,
          intent: Intent.Primary,
          title: `Bulk operations on all ${selection.tracks.length} selected tracks`,
        }),
      },
      [
        m(
          Callout,
          {
            className: 'pf-timeline-toolbar__bulk-callout',
            icon: Icons.Info,
          },
          `Changes apply to all selected tracks`,
        ),
        m(MenuDivider, {label: 'Workspace'}),
        this.renderCopySelectedTracksToWorkspace(trace, selection),
        m(MenuDivider, {label: 'Bulk track settings'}, settingsMenuItems),
      ],
    );
  }

  private renderWorkspaceMenu(trace: TraceImpl) {
    const workspaces = trace.workspaces;
    const currentWorkspace = trace.currentWorkspace;
    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          label: currentWorkspace.title,
          icon: 'workspaces',
          compact: COMPACT_BUTTONS,
          shrink: true,
        }),
      },
      [
        m(MenuTitle, {label: 'All workspaces'}),
        workspaces.all.map((ws) => {
          return m(MenuItem, {
            label: ws.title,
            icon:
              ws === trace?.currentWorkspace
                ? 'radio_button_checked'
                : 'radio_button_unchecked',
            onclick: () => {
              workspaces.switchWorkspace(ws);
            },
          });
        }),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace',
          icon: 'add',
          onclick: () => {
            const ws = workspaces.createEmptyWorkspace('Untitled Workspace');
            workspaces.switchWorkspace(ws);
          },
        }),
        m(MenuDivider, {label: 'Current workspace'}),
        m(MenuItem, {
          icon: 'edit',
          label: 'Rename',
          disabled: !currentWorkspace.userEditable,
          title: currentWorkspace.userEditable
            ? 'Rename current workspace'
            : 'This workspace is not editable - please create a new workspace if you wish to modify it',
          onclick: async () => {
            const newName = await trace.omnibox.prompt('Enter a new name...');
            if (newName) {
              workspaces.currentWorkspace.title = newName;
            }
          },
        }),
        m(MenuItem, {
          icon: Icons.Delete,
          label: 'Remove',
          disabled: !currentWorkspace.userEditable,
          title: currentWorkspace.userEditable
            ? 'Remove current workspace'
            : 'This workspace is not editable - please reate a new workspace if you wish to modify it',
          onclick: () => {
            workspaces.removeWorkspace(workspaces.currentWorkspace);
          },
        }),
        m(MenuItem, {
          icon: 'create_new_folder',
          label: 'New track group',
          disabled: !trace.currentWorkspace.userEditable,
          title: trace.currentWorkspace.userEditable
            ? 'Create new group'
            : 'This workspace is not editable - please create a new workspace if you wish to modify it',
          onclick: async () => {
            const result = await trace.omnibox.prompt('Group name...');
            if (result) {
              const group = new TrackNode({name: result, isSummary: true});
              trace.currentWorkspace.addChildLast(group);
            }
          },
        }),
      ],
    );
  }

  private renderTrackFilter(trace: TraceImpl) {
    const trackFilters = trace.tracks.filters;

    return m(
      Popup,
      {
        trigger: m(Button, {
          icon: Icons.Filter,
          title: 'Track filter',
          compact: COMPACT_BUTTONS,
          iconFilled: trackFilters.areFiltersSet(),
        }),
      },
      m(
        'form.pf-track-filter',
        {
          oncreate({dom}) {
            // Focus & select text box when the popup opens.
            const input = findRef(dom, FILTER_TEXT_BOX_REF) as HTMLInputElement;
            input.focus();
            input.select();
          },
        },
        m(
          '.pf-track-filter__row',
          m('label', {for: 'filter-name'}, 'Filter by name'),
          m(TextInput, {
            ref: FILTER_TEXT_BOX_REF,
            id: 'filter-name',
            placeholder: 'Filter by name...',
            title: 'Filter by name (comma separated terms)',
            value: trackFilters.nameFilter,
            oninput: (e: Event) => {
              const value = (e.target as HTMLInputElement).value;
              trackFilters.nameFilter = value;
            },
          }),
        ),
        trace.tracks.trackFilterCriteria.map((filter) => {
          return m(
            '.pf-track-filter__row',
            m('label', 'Filter by ', filter.name),
            m(PopupMultiSelect, {
              label: filter.name,
              showNumSelected: true,
              // It usually doesn't make sense to select all filters - if users
              // want to pass all they should just remove the filters instead.
              showSelectAllButton: false,
              onChange: (diff) => {
                for (const {id, checked} of diff) {
                  if (checked) {
                    // Add the filter option to the criteria.
                    const criteriaFilters = trackFilters.criteriaFilters.get(
                      filter.name,
                    );
                    if (criteriaFilters) {
                      criteriaFilters.push(id);
                    } else {
                      trackFilters.criteriaFilters.set(filter.name, [id]);
                    }
                  } else {
                    // Remove the filter option from the criteria.
                    const filterOptions = trackFilters.criteriaFilters.get(
                      filter.name,
                    );

                    if (!filterOptions) continue;
                    const newOptions = filterOptions.filter((f) => f !== id);
                    if (newOptions.length === 0) {
                      trackFilters.criteriaFilters.delete(filter.name);
                    } else {
                      trackFilters.criteriaFilters.set(filter.name, newOptions);
                    }
                  }
                }
              },
              options: filter.options
                .map((o): MultiSelectOption => {
                  const filterOptions = trackFilters.criteriaFilters.get(
                    filter.name,
                  );
                  const checked = Boolean(
                    filterOptions && filterOptions.includes(o.key),
                  );
                  return {id: o.key, name: o.label, checked};
                })
                .filter((f) => f.name !== ''),
            }),
          );
        }),
        m(Button, {
          type: 'reset',
          label: 'Clear All Filters',
          icon: Icons.FilterOff,
          onclick: () => {
            trackFilters.clearAll();
          },
        }),
      ),
    );
  }

  private renderCopySelectedTracksToWorkspace(
    trace: TraceImpl,
    selection: Selection,
  ) {
    const isArea = selection.kind === 'area';
    return [
      m(
        MenuItem,
        {
          label: 'Copy to',
          disabled: !isArea,
          title: isArea
            ? 'Copy selected tracks to workspace'
            : 'Please create an area selection to copy tracks',
        },
        trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            disabled: !ws.userEditable,
            onclick: isArea
              ? () => this.copySelectedToWorkspace(trace, ws, selection)
              : undefined,
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace',
          onclick: isArea
            ? () => this.copySelectedToWorkspace(trace, undefined, selection)
            : undefined,
        }),
      ),
      m(
        MenuItem,
        {
          label: 'Copy & switch to',
          disabled: !isArea,
          title: isArea
            ? 'Copy selected tracks to workspace and switch to that workspace'
            : 'Please create an area selection to copy tracks',
        },
        trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            disabled: !ws.userEditable,
            onclick: isArea
              ? async () => {
                  this.copySelectedToWorkspace(trace, ws, selection);
                  trace.workspaces.switchWorkspace(ws);
                }
              : undefined,
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'new workspace',
          onclick: isArea
            ? async () => {
                const ws = this.copySelectedToWorkspace(
                  trace,
                  undefined,
                  selection,
                );
                trace.workspaces.switchWorkspace(ws);
              }
            : undefined,
        }),
      ),
    ];
  }

  private copySelectedToWorkspace(
    trace: TraceImpl,
    ws: Workspace | undefined,
    selection: AreaSelection,
  ) {
    // If no workspace provided, create a new one.
    if (!ws) {
      ws = trace.workspaces.createEmptyWorkspace('Untitled Workspace');
    }
    for (const track of selection.tracks) {
      const node = trace.currentWorkspace.getTrackByUri(track.uri);
      if (!node) continue;
      const newNode = node.clone();
      ws.addChildLast(newNode);
    }
    return ws;
  }

  private renderBulkSettingsMenu(trace: TraceImpl): m.Children {
    const selection = trace.selection.selection;
    if (selection.kind !== 'area') return null;

    // Get all unique settings for the selected tracks.
    const allSettings = new Map<
      TrackSettingDescriptor<unknown>,
      TrackSetting<unknown>[]
    >();
    for (const track of selection.tracks) {
      const settings = track.renderer?.settings;
      if (!settings) continue;
      for (const setting of settings) {
        const existing = allSettings.get(setting.descriptor) ?? [];
        existing.push(setting);
        allSettings.set(setting.descriptor, existing);
      }
    }

    // Remove any settings that are not common to all selected tracks.
    for (const [descriptor, settings] of allSettings) {
      if (settings.length !== selection.tracks.length) {
        allSettings.delete(descriptor);
      }
    }

    // If no settings remain, don't render the menu.
    if (allSettings.size === 0) return null;

    // Iterate over all unique settings, rendering a menu entry for each.
    const settingEntries: m.Children[] = [];
    for (const [descriptor, settings] of allSettings) {
      // If all values are the same, we can render a single select.

      settingEntries.push(
        renderTrackSettingMenu(
          descriptor,
          (value) => {
            // Set all the settings to the same thing!
            for (const setting of settings) {
              setting.setValue(value);
            }
          },
          settings.map((s) => s.getValue()),
        ),
      );
    }

    return settingEntries;
  }
}

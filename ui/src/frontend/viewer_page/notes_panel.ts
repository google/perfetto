// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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
import {canvasClip} from '../../base/canvas_utils';
import {currentTargetOffset, findRef} from '../../base/dom_utils';
import {Size2D} from '../../base/geom';
import {assertUnreachable} from '../../base/logging';
import {Icons} from '../../base/semantic_icons';
import {TimeScale} from '../../base/time_scale';
import {randomColor} from '../../components/colorizer';
import {raf} from '../../core/raf_scheduler';
import {TraceImpl} from '../../core/trace_impl';
import {Note, SpanNote} from '../../public/note';
import {Button, ButtonBar} from '../../widgets/button';
import {MenuDivider, MenuItem, PopupMenu} from '../../widgets/menu';
import {Select} from '../../widgets/select';
import {TRACK_SHELL_WIDTH} from '../css_constants';
import {generateTicks, getMaxMajorTicks, TickType} from './gridline_helper';
import {TextInput} from '../../widgets/text_input';
import {Popup} from '../../widgets/popup';
import {TrackNode, Workspace} from '../../public/workspace';
import {AreaSelection, Selection} from '../../public/selection';
import {MultiSelectOption, PopupMultiSelect} from '../../widgets/multiselect';

const FLAG_WIDTH = 16;
const AREA_TRIANGLE_WIDTH = 10;
const FLAG = `\uE153`;

function toSummary(s: string) {
  const newlineIndex = s.indexOf('\n') > 0 ? s.indexOf('\n') : s.length;
  return s.slice(0, Math.min(newlineIndex, s.length, 16));
}

function getStartTimestamp(note: Note | SpanNote) {
  const noteType = note.noteType;
  switch (noteType) {
    case 'SPAN':
      return note.start;
    case 'DEFAULT':
      return note.timestamp;
    default:
      assertUnreachable(noteType);
  }
}

const FILTER_TEXT_BOX_REF = 'filter-text-box';

export class NotesPanel {
  private readonly trace: TraceImpl;
  private timescale?: TimeScale; // The timescale from the last render()
  private hoveredX: null | number = null;
  private mouseDragging = false;
  readonly height = 20;

  constructor(trace: TraceImpl) {
    this.trace = trace;
  }

  render(): m.Children {
    const allCollapsed = this.trace.workspace.flatTracks.every(
      (n) => n.collapsed,
    );

    const workspaces = this.trace.workspaces;
    const selection = this.trace.selection.selection;

    return m(
      '',
      {
        style: {height: `${this.height}px`},
        onmousedown: () => {
          // If the user clicks & drags, very likely they just want to measure
          // the time horizontally, not set a flag. This debouncing is done to
          // avoid setting accidental flags like measuring the time on the brush
          // timeline.
          this.mouseDragging = false;
        },
        onclick: (e: MouseEvent) => {
          if (!this.mouseDragging) {
            const x = currentTargetOffset(e).x - TRACK_SHELL_WIDTH;
            this.onClick(x);
            e.stopPropagation();
          }
        },
        onmousemove: (e: MouseEvent) => {
          this.mouseDragging = true;
          this.hoveredX = currentTargetOffset(e).x - TRACK_SHELL_WIDTH;
          raf.scheduleCanvasRedraw();
        },
        onmouseenter: (e: MouseEvent) => {
          this.hoveredX = currentTargetOffset(e).x - TRACK_SHELL_WIDTH;
          raf.scheduleCanvasRedraw();
        },
        onmouseout: () => {
          this.hoveredX = null;
          this.trace.timeline.hoveredNoteTimestamp = undefined;
        },
      },
      m(
        ButtonBar,
        {className: 'pf-timeline-toolbar'},
        m(Button, {
          onclick: (e: Event) => {
            e.preventDefault();
            if (allCollapsed) {
              this.trace.commands.runCommand(
                'perfetto.CoreCommands#ExpandAllGroups',
              );
            } else {
              this.trace.commands.runCommand(
                'perfetto.CoreCommands#CollapseAllGroups',
              );
            }
          },
          title: allCollapsed ? 'Expand all' : 'Collapse all',
          icon: allCollapsed ? 'unfold_more' : 'unfold_less',
          compact: true,
        }),
        m(Button, {
          onclick: (e: Event) => {
            e.preventDefault();
            this.trace.workspace.pinnedTracks.forEach((t) =>
              this.trace.workspace.unpinTrack(t),
            );
          },
          title: 'Clear all pinned tracks',
          icon: 'clear_all',
          compact: true,
        }),
        this.renderTrackFilter(),
        m(
          Select,
          {
            className: 'pf-timeline-toolbar__workspace-selector',
            onchange: async (e) => {
              const value = (e.target as HTMLSelectElement).value;
              if (value === 'new-workspace') {
                const ws =
                  workspaces.createEmptyWorkspace('Untitled Workspace');
                workspaces.switchWorkspace(ws);
              } else {
                const ws = workspaces.all.find(({id}) => id === value);
                ws && this.trace?.workspaces.switchWorkspace(ws);
              }
            },
          },
          workspaces.all
            .map((ws) => {
              return m('option', {
                value: `${ws.id}`,
                label: ws.title,
                selected: ws === this.trace?.workspace,
              });
            })
            .concat([
              m('option', {
                value: 'new-workspace',
                label: 'New workspace...',
              }),
            ]),
        ),
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              icon: 'more_vert',
              title: 'Workspace options',
              compact: true,
            }),
          },
          this.renderCopySelectedTracksToWorkspace(selection),
          m(MenuDivider),
          this.renderNewGroupButton(),
          m(MenuDivider),
          m(MenuItem, {
            icon: 'edit',
            label: 'Rename current workspace',
            disabled: !this.trace.workspace.userEditable,
            title: this.trace.workspace.userEditable
              ? 'Create new group'
              : 'This workspace is not editable - please create a new workspace if you wish to modify it',
            onclick: async () => {
              const newName = await this.trace.omnibox.prompt(
                'Enter a new name...',
              );
              if (newName) {
                workspaces.currentWorkspace.title = newName;
              }
            },
          }),
          m(MenuItem, {
            icon: Icons.Delete,
            label: 'Delete current workspace',
            disabled: !this.trace.workspace.userEditable,
            title: this.trace.workspace.userEditable
              ? 'Create new group'
              : 'This workspace is not editable - please create a new workspace if you wish to modify it',
            onclick: () => {
              workspaces.removeWorkspace(workspaces.currentWorkspace);
            },
          }),
        ),
      ),
    );
  }

  private renderTrackFilter() {
    const trackFilters = this.trace.tracks.filters;

    return m(
      Popup,
      {
        trigger: m(Button, {
          icon: 'filter_alt',
          title: 'Track filter',
          compact: true,
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
        this.trace.tracks.trackFilterCriteria.map((filter) => {
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
          icon: 'filter_alt_off',
          onclick: () => {
            trackFilters.clearAll();
          },
        }),
      ),
    );
  }

  private renderNewGroupButton() {
    return m(MenuItem, {
      icon: 'create_new_folder',
      label: 'Create new group track',
      disabled: !this.trace.workspace.userEditable,
      title: this.trace.workspace.userEditable
        ? 'Create new group'
        : 'This workspace is not editable - please create a new workspace if you wish to modify it',
      onclick: async () => {
        const result = await this.trace.omnibox.prompt('Group name...');
        if (result) {
          const group = new TrackNode({title: result, isSummary: true});
          this.trace.workspace.addChildLast(group);
        }
      },
    });
  }

  private renderCopySelectedTracksToWorkspace(selection: Selection) {
    const isArea = selection.kind === 'area';
    return [
      m(
        MenuItem,
        {
          label: 'Copy selected tracks to workspace',
          disabled: !isArea,
          title: isArea
            ? 'Copy selected tracks to workspace'
            : 'Please create an area selection to copy tracks',
        },
        this.trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            disabled: !ws.userEditable,
            onclick: isArea
              ? () => this.copySelectedToWorkspace(ws, selection)
              : undefined,
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace...',
          onclick: isArea
            ? () => this.copySelectedToWorkspace(undefined, selection)
            : undefined,
        }),
      ),
      m(
        MenuItem,
        {
          label: 'Copy selected tracks & switch to workspace',
          disabled: !isArea,
          title: isArea
            ? 'Copy selected tracks to workspace and switch to that workspace'
            : 'Please create an area selection to copy tracks',
        },
        this.trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            disabled: !ws.userEditable,
            onclick: isArea
              ? async () => {
                  this.copySelectedToWorkspace(ws, selection);
                  this.trace.workspaces.switchWorkspace(ws);
                }
              : undefined,
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace...',
          onclick: isArea
            ? async () => {
                const ws = this.copySelectedToWorkspace(undefined, selection);
                this.trace.workspaces.switchWorkspace(ws);
              }
            : undefined,
        }),
      ),
    ];
  }

  private copySelectedToWorkspace(
    ws: Workspace | undefined,
    selection: AreaSelection,
  ) {
    // If no workspace provided, create a new one.
    if (!ws) {
      ws = this.trace.workspaces.createEmptyWorkspace('Untitled Workspace');
    }
    for (const track of selection.tracks) {
      const node = this.trace.workspace.getTrackByUri(track.uri);
      if (!node) continue;
      const newNode = node.clone();
      ws.addChildLast(newNode);
    }
    return ws;
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D) {
    ctx.fillStyle = '#999';
    ctx.fillRect(TRACK_SHELL_WIDTH - 1, 0, 1, size.height);

    const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};

    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);
    this.renderPanel(ctx, trackSize);
    ctx.restore();
  }

  private renderPanel(ctx: CanvasRenderingContext2D, size: Size2D): void {
    let aNoteIsHovered = false;

    const visibleWindow = this.trace.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: size.width,
    });
    const timespan = visibleWindow.toTimeSpan();

    this.timescale = timescale;

    if (size.width > 0 && timespan.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width);
      const offset = this.trace.timeline.timestampOffset();
      const tickGen = generateTicks(timespan, maxMajorTicks, offset);
      for (const {type, time} of tickGen) {
        const px = Math.floor(timescale.timeToPx(time));
        if (type === TickType.MAJOR) {
          ctx.fillRect(px, 0, 1, size.height);
        }
      }
    }

    ctx.textBaseline = 'bottom';
    ctx.font = '10px Helvetica';

    for (const note of this.trace.notes.notes.values()) {
      const timestamp = getStartTimestamp(note);
      // TODO(hjd): We should still render area selection marks in viewport is
      // *within* the area (e.g. both lhs and rhs are out of bounds).
      if (
        (note.noteType === 'DEFAULT' &&
          !visibleWindow.contains(note.timestamp)) ||
        (note.noteType === 'SPAN' &&
          !visibleWindow.overlaps(note.start, note.end))
      ) {
        continue;
      }
      const currentIsHovered =
        this.hoveredX !== null && this.hitTestNote(this.hoveredX, note);
      if (currentIsHovered) aNoteIsHovered = true;

      const selection = this.trace.selection.selection;
      const isSelected = selection.kind === 'note' && selection.id === note.id;
      const x = timescale.timeToPx(timestamp);
      const left = Math.floor(x);

      // Draw flag or marker.
      if (note.noteType === 'SPAN') {
        this.drawAreaMarker(
          ctx,
          left,
          Math.floor(timescale.timeToPx(note.end)),
          note.color,
          isSelected,
        );
      } else {
        this.drawFlag(ctx, left, size.height, note.color, isSelected);
      }

      if (note.text) {
        const summary = toSummary(note.text);
        const measured = ctx.measureText(summary);
        // Add a white semi-transparent background for the text.
        ctx.fillStyle = 'rgba(255, 255, 255, 0.8)';
        ctx.fillRect(
          left + FLAG_WIDTH + 2,
          size.height + 2,
          measured.width + 2,
          -12,
        );
        ctx.fillStyle = '#3c4b5d';
        ctx.fillText(summary, left + FLAG_WIDTH + 3, size.height + 1);
      }
    }

    // A real note is hovered so we don't need to see the preview line.
    // TODO(hjd): Change cursor to pointer here.
    if (aNoteIsHovered) {
      this.trace.timeline.hoveredNoteTimestamp = undefined;
    }

    // View preview note flag when hovering on notes panel.
    if (!aNoteIsHovered && this.hoveredX !== null) {
      const timestamp = timescale.pxToHpTime(this.hoveredX).toTime();
      if (visibleWindow.contains(timestamp)) {
        this.trace.timeline.hoveredNoteTimestamp = timestamp;
        const x = timescale.timeToPx(timestamp);
        const left = Math.floor(x);
        this.drawFlag(ctx, left, size.height, '#aaa', /* fill */ true);
      }
    }

    ctx.restore();
  }

  private drawAreaMarker(
    ctx: CanvasRenderingContext2D,
    x: number,
    xEnd: number,
    color: string,
    fill: boolean,
  ) {
    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    const topOffset = 10;
    // Don't draw in the track shell section.
    if (x >= 0) {
      // Draw left triangle.
      ctx.beginPath();
      ctx.moveTo(x, topOffset);
      ctx.lineTo(x, topOffset + AREA_TRIANGLE_WIDTH);
      ctx.lineTo(x + AREA_TRIANGLE_WIDTH, topOffset);
      ctx.lineTo(x, topOffset);
      if (fill) ctx.fill();
      ctx.stroke();
    }
    // Draw right triangle.
    ctx.beginPath();
    ctx.moveTo(xEnd, topOffset);
    ctx.lineTo(xEnd, topOffset + AREA_TRIANGLE_WIDTH);
    ctx.lineTo(xEnd - AREA_TRIANGLE_WIDTH, topOffset);
    ctx.lineTo(xEnd, topOffset);
    if (fill) ctx.fill();
    ctx.stroke();

    // Start line after track shell section, join triangles.
    const startDraw = Math.max(x, 0);
    ctx.beginPath();
    ctx.moveTo(startDraw, topOffset);
    ctx.lineTo(xEnd, topOffset);
    ctx.stroke();
  }

  private drawFlag(
    ctx: CanvasRenderingContext2D,
    x: number,
    height: number,
    color: string,
    fill?: boolean,
  ) {
    const prevFont = ctx.font;
    const prevBaseline = ctx.textBaseline;
    ctx.textBaseline = 'alphabetic';
    // Adjust height for icon font.
    ctx.font = '24px Material Symbols Sharp';
    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    // The ligatures have padding included that means the icon is not drawn
    // exactly at the x value. This adjusts for that.
    const iconPadding = 6;
    if (fill) {
      ctx.fillText(FLAG, x - iconPadding, height + 2);
    } else {
      ctx.strokeText(FLAG, x - iconPadding, height + 2.5);
    }
    ctx.font = prevFont;
    ctx.textBaseline = prevBaseline;
  }

  private onClick(x: number) {
    if (!this.timescale) {
      return;
    }

    // Select the hovered note, or create a new single note & select it
    if (x < 0) return;
    for (const note of this.trace.notes.notes.values()) {
      if (this.hoveredX !== null && this.hitTestNote(this.hoveredX, note)) {
        this.trace.selection.selectNote({id: note.id});
        return;
      }
    }
    const timestamp = this.timescale.pxToHpTime(x).toTime();
    const color = randomColor();
    const noteId = this.trace.notes.addNote({timestamp, color});
    this.trace.selection.selectNote({id: noteId});
  }

  private hitTestNote(x: number, note: SpanNote | Note): boolean {
    if (!this.timescale) {
      return false;
    }

    const timescale = this.timescale;
    const noteX = timescale.timeToPx(getStartTimestamp(note));
    if (note.noteType === 'SPAN') {
      return (
        (noteX <= x && x < noteX + AREA_TRIANGLE_WIDTH) ||
        (timescale.timeToPx(note.end) > x &&
          x > timescale.timeToPx(note.end) - AREA_TRIANGLE_WIDTH)
      );
    } else {
      const width = FLAG_WIDTH;
      return noteX <= x && x < noteX + width;
    }
  }
}

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

import {currentTargetOffset} from '../base/dom_utils';
import {Icons} from '../base/semantic_icons';
import {Time} from '../base/time';
import {Actions} from '../common/actions';
import {randomColor} from '../common/colorizer';
import {AreaNote, Note} from '../common/state';
import {raf} from '../core/raf_scheduler';
import {Button} from '../widgets/button';

import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from './bottom_tab';
import {TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {
  getMaxMajorTicks,
  TickGenerator,
  TickType,
  timeScaleForVisibleWindow,
} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {isTraceLoaded} from './sidebar';
import {Timestamp} from './widgets/timestamp';

const FLAG_WIDTH = 16;
const AREA_TRIANGLE_WIDTH = 10;
const FLAG = `\uE153`;

function toSummary(s: string) {
  const newlineIndex = s.indexOf('\n') > 0 ? s.indexOf('\n') : s.length;
  return s.slice(0, Math.min(newlineIndex, s.length, 16));
}

function getStartTimestamp(note: Note|AreaNote) {
  if (note.noteType === 'AREA') {
    return globals.state.areas[note.areaId].start;
  } else {
    return note.timestamp;
  }
}

export class NotesPanel extends Panel {
  hoveredX: null|number = null;

  view() {
    const allCollapsed = Object.values(globals.state.trackGroups)
                             .every((group) => group.collapsed);

    return m(
        '.notes-panel',
        {
          onclick: (e: MouseEvent) => {
            const {x, y} = currentTargetOffset(e);
            this.onClick(x - TRACK_SHELL_WIDTH, y);
            e.stopPropagation();
          },
          onmousemove: (e: MouseEvent) => {
            this.hoveredX = currentTargetOffset(e).x - TRACK_SHELL_WIDTH;
            raf.scheduleRedraw();
          },
          mouseenter: (e: MouseEvent) => {
            this.hoveredX = currentTargetOffset(e).x - TRACK_SHELL_WIDTH;
            raf.scheduleRedraw();
          },
          onmouseout: () => {
            this.hoveredX = null;
            globals.dispatch(
                Actions.setHoveredNoteTimestamp({ts: Time.INVALID}));
          },
        },
        isTraceLoaded() ?
            [
              m('button',
                {
                  onclick: (e: Event) => {
                    e.preventDefault();
                    globals.dispatch(Actions.toggleAllTrackGroups(
                        {collapsed: !allCollapsed}));
                  },
                },
                m('i.material-icons',
                  {title: allCollapsed ? 'Expand all' : 'Collapse all'},
                  allCollapsed ? 'unfold_more' : 'unfold_less')),
              m('button',
                {
                  onclick: (e: Event) => {
                    e.preventDefault();
                    globals.dispatch(Actions.clearAllPinnedTracks({}));
                  },
                },
                m('i.material-icons',
                  {title: 'Clear all pinned tracks'},
                  'clear_all')),
            ] :
            '');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    let aNoteIsHovered = false;

    ctx.fillStyle = '#999';
    ctx.fillRect(TRACK_SHELL_WIDTH - 2, 0, 2, size.height);

    ctx.save();
    ctx.beginPath();
    ctx.rect(TRACK_SHELL_WIDTH, 0, size.width - TRACK_SHELL_WIDTH, size.height);
    ctx.clip();

    const span = globals.frontendLocalState.visibleTimeSpan;
    const {visibleTimeScale} = globals.frontendLocalState;
    if (size.width > TRACK_SHELL_WIDTH && span.duration > 0n) {
      const maxMajorTicks = getMaxMajorTicks(size.width - TRACK_SHELL_WIDTH);
      const map = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, size.width);
      const offset = globals.timestampOffset();
      const tickGen = new TickGenerator(span, maxMajorTicks, offset);
      for (const {type, time} of tickGen) {
        const px = Math.floor(map.timeToPx(time));
        if (type === TickType.MAJOR) {
          ctx.fillRect(px, 0, 1, size.height);
        }
      }
    }

    ctx.textBaseline = 'bottom';
    ctx.font = '10px Helvetica';

    for (const note of Object.values(globals.state.notes)) {
      const timestamp = getStartTimestamp(note);
      // TODO(hjd): We should still render area selection marks in viewport is
      // *within* the area (e.g. both lhs and rhs are out of bounds).
      if ((note.noteType !== 'AREA' && !span.contains(timestamp)) ||
          (note.noteType === 'AREA' &&
           !span.contains(globals.state.areas[note.areaId].end) &&
           !span.contains(globals.state.areas[note.areaId].start))) {
        continue;
      }
      const currentIsHovered =
          this.hoveredX && this.mouseOverNote(this.hoveredX, note);
      if (currentIsHovered) aNoteIsHovered = true;

      const selection = globals.state.currentSelection;
      const isSelected = selection !== null &&
          ((selection.kind === 'NOTE' && selection.id === note.id) ||
           (selection.kind === 'AREA' && selection.noteId === note.id));
      const x = visibleTimeScale.timeToPx(timestamp);
      const left = Math.floor(x + TRACK_SHELL_WIDTH);

      // Draw flag or marker.
      if (note.noteType === 'AREA') {
        const area = globals.state.areas[note.areaId];
        this.drawAreaMarker(
            ctx,
            left,
            Math.floor(visibleTimeScale.timeToPx(area.end) + TRACK_SHELL_WIDTH),
            note.color,
            isSelected);
      } else {
        this.drawFlag(ctx, left, size.height, note.color, isSelected);
      }

      if (note.text) {
        const summary = toSummary(note.text);
        const measured = ctx.measureText(summary);
        // Add a white semi-transparent background for the text.
        ctx.fillStyle = 'rgba(255, 255, 255, 0.8)';
        ctx.fillRect(
            left + FLAG_WIDTH + 2, size.height + 2, measured.width + 2, -12);
        ctx.fillStyle = '#3c4b5d';
        ctx.fillText(summary, left + FLAG_WIDTH + 3, size.height + 1);
      }
    }

    // A real note is hovered so we don't need to see the preview line.
    // TODO(hjd): Change cursor to pointer here.
    if (aNoteIsHovered) {
      globals.dispatch(Actions.setHoveredNoteTimestamp({ts: Time.INVALID}));
    }

    // View preview note flag when hovering on notes panel.
    if (!aNoteIsHovered && this.hoveredX !== null) {
      const timestamp = visibleTimeScale.pxToHpTime(this.hoveredX).toTime();
      if (span.contains(timestamp)) {
        globals.dispatch(Actions.setHoveredNoteTimestamp({ts: timestamp}));
        const x = visibleTimeScale.timeToPx(timestamp);
        const left = Math.floor(x + TRACK_SHELL_WIDTH);
        this.drawFlag(ctx, left, size.height, '#aaa', /* fill */ true);
      }
    }

    ctx.restore();
  }

  private drawAreaMarker(
      ctx: CanvasRenderingContext2D, x: number, xEnd: number, color: string,
      fill: boolean) {
    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    const topOffset = 10;
    // Don't draw in the track shell section.
    if (x >= globals.frontendLocalState.windowSpan.start + TRACK_SHELL_WIDTH) {
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
    const startDraw = Math.max(
        x, globals.frontendLocalState.windowSpan.start + TRACK_SHELL_WIDTH);
    ctx.beginPath();
    ctx.moveTo(startDraw, topOffset);
    ctx.lineTo(xEnd, topOffset);
    ctx.stroke();
  }

  private drawFlag(
      ctx: CanvasRenderingContext2D, x: number, height: number, color: string,
      fill?: boolean) {
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


  private onClick(x: number, _: number) {
    if (x < 0) return;
    const {visibleTimeScale} = globals.frontendLocalState;
    const timestamp = visibleTimeScale.pxToHpTime(x).toTime();
    for (const note of Object.values(globals.state.notes)) {
      if (this.hoveredX && this.mouseOverNote(this.hoveredX, note)) {
        if (note.noteType === 'AREA') {
          globals.makeSelection(
              Actions.reSelectArea({areaId: note.areaId, noteId: note.id}));
        } else {
          globals.makeSelection(Actions.selectNote({id: note.id}));
        }
        return;
      }
    }
    const color = randomColor();
    globals.makeSelection(Actions.addNote({timestamp, color}));
  }

  private mouseOverNote(x: number, note: AreaNote|Note): boolean {
    const timeScale = globals.frontendLocalState.visibleTimeScale;
    const noteX = timeScale.timeToPx(getStartTimestamp(note));
    if (note.noteType === 'AREA') {
      const noteArea = globals.state.areas[note.areaId];
      return (noteX <= x && x < noteX + AREA_TRIANGLE_WIDTH) ||
          (timeScale.timeToPx(noteArea.end) > x &&
           x > timeScale.timeToPx(noteArea.end) - AREA_TRIANGLE_WIDTH);
    } else {
      const width = FLAG_WIDTH;
      return noteX <= x && x < noteX + width;
    }
  }
}

interface NotesEditorTabConfig {
  id: string;
}

export class NotesEditorTab extends BottomTab<NotesEditorTabConfig> {
  static readonly kind = 'org.perfetto.NotesEditorTab';

  static create(args: NewBottomTabArgs): NotesEditorTab {
    return new NotesEditorTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
  }

  getTitle() {
    return 'Current Selection';
  }

  viewTab() {
    const note = globals.state.notes[this.config.id];
    if (note === undefined) {
      return m('.', `No Note with id ${this.config.id}`);
    }
    const startTime = getStartTimestamp(note);
    return m(
        '.notes-editor-panel',
        m('.notes-editor-panel-heading-bar',
          m('.notes-editor-panel-heading',
            `Annotation at `,
            m(Timestamp, {ts: startTime})),
          m('input[type=text]', {
            value: note.text,
            onchange: (e: InputEvent) => {
              const newText = (e.target as HTMLInputElement).value;
              globals.dispatch(Actions.changeNoteText({
                id: this.config.id,
                newText,
              }));
            },
          }),
          m('span.color-change', `Change color: `, m('input[type=color]', {
              value: note.color,
              onchange: (e: Event) => {
                const newColor = (e.target as HTMLInputElement).value;
                globals.dispatch(Actions.changeNoteColor({
                  id: this.config.id,
                  newColor,
                }));
              },
            })),
          m(Button, {
            label: 'Remove',
            icon: Icons.Delete,
            minimal: true,
            onclick: () => {
              globals.dispatch(Actions.removeNote({id: this.config.id}));
              globals.dispatch(Actions.setCurrentTab({tab: undefined}));
              raf.scheduleFullRedraw();
            },
          })),
    );
  }
}

bottomTabRegistry.register(NotesEditorTab);

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

import * as m from 'mithril';

import {Actions} from '../common/actions';
import {randomColor} from '../common/colorizer';
import {AreaNote, Note} from '../common/state';
import {timeToString} from '../common/time';

import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from './bottom_tab';
import {TRACK_SHELL_WIDTH} from './css_constants';
import {PerfettoMouseEvent} from './events';
import {globals} from './globals';
import {
  TickGenerator,
  TickType,
  timeScaleForVisibleWindow,
} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {isTraceLoaded} from './sidebar';

const FLAG_WIDTH = 16;
const AREA_TRIANGLE_WIDTH = 10;
const FLAG = `\uE153`;

function toSummary(s: string) {
  const newlineIndex = s.indexOf('\n') > 0 ? s.indexOf('\n') : s.length;
  return s.slice(0, Math.min(newlineIndex, s.length, 16));
}

function getStartTimestamp(note: Note|AreaNote) {
  if (note.noteType === 'AREA') {
    return globals.state.areas[note.areaId].startSec;
  } else {
    return note.timestamp;
  }
}

export class NotesPanel extends Panel {
  hoveredX: null|number = null;

  oncreate({dom}: m.CVnodeDOM) {
    dom.addEventListener('mousemove', (e: Event) => {
      this.hoveredX = (e as PerfettoMouseEvent).layerX - TRACK_SHELL_WIDTH;
      globals.rafScheduler.scheduleRedraw();
    }, {passive: true});
    dom.addEventListener('mouseenter', (e: Event) => {
      this.hoveredX = (e as PerfettoMouseEvent).layerX - TRACK_SHELL_WIDTH;
      globals.rafScheduler.scheduleRedraw();
    });
    dom.addEventListener('mouseout', () => {
      this.hoveredX = null;
      globals.dispatch(Actions.setHoveredNoteTimestamp({ts: -1}));
    }, {passive: true});
  }

  view() {
    const allCollapsed = Object.values(globals.state.trackGroups)
                             .every((group) => group.collapsed);

    return m(
        '.notes-panel',
        {
          onclick: (e: PerfettoMouseEvent) => {
            this.onClick(e.layerX - TRACK_SHELL_WIDTH, e.layerY);
            e.stopPropagation();
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
    const timeScale = globals.frontendLocalState.timeScale;
    let aNoteIsHovered = false;

    ctx.fillStyle = '#999';
    ctx.fillRect(TRACK_SHELL_WIDTH - 2, 0, 2, size.height);
    const relScale = timeScaleForVisibleWindow(TRACK_SHELL_WIDTH, size.width);
    if (relScale.timeSpan.duration > 0 && relScale.widthPx > 0) {
      for (const {type, position} of new TickGenerator(relScale)) {
        if (type === TickType.MAJOR) ctx.fillRect(position, 0, 1, size.height);
      }
    }

    ctx.textBaseline = 'bottom';
    ctx.font = '10px Helvetica';

    for (const note of Object.values(globals.state.notes)) {
      const timestamp = getStartTimestamp(note);
      // TODO(hjd): We should still render area selection marks in viewport is
      // *within* the area (e.g. both lhs and rhs are out of bounds).
      if ((note.noteType !== 'AREA' && !timeScale.timeInBounds(timestamp)) ||
          (note.noteType === 'AREA' &&
           !timeScale.timeInBounds(globals.state.areas[note.areaId].endSec) &&
           !timeScale.timeInBounds(
               globals.state.areas[note.areaId].startSec))) {
        continue;
      }
      const currentIsHovered =
          this.hoveredX && this.mouseOverNote(this.hoveredX, note);
      if (currentIsHovered) aNoteIsHovered = true;

      const selection = globals.state.currentSelection;
      const isSelected = selection !== null &&
          ((selection.kind === 'NOTE' && selection.id === note.id) ||
           (selection.kind === 'AREA' && selection.noteId === note.id));
      const x = timeScale.timeToPx(timestamp);
      const left = Math.floor(x + TRACK_SHELL_WIDTH);

      // Draw flag or marker.
      if (note.noteType === 'AREA') {
        const area = globals.state.areas[note.areaId];
        this.drawAreaMarker(
            ctx,
            left,
            Math.floor(timeScale.timeToPx(area.endSec) + TRACK_SHELL_WIDTH),
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
      globals.dispatch(Actions.setHoveredNoteTimestamp({ts: -1}));
    }

    // View preview note flag when hovering on notes panel.
    if (!aNoteIsHovered && this.hoveredX !== null) {
      const timestamp = timeScale.pxToTime(this.hoveredX);
      if (timeScale.timeInBounds(timestamp)) {
        globals.dispatch(Actions.setHoveredNoteTimestamp({ts: timestamp}));
        const x = timeScale.timeToPx(timestamp);
        const left = Math.floor(x + TRACK_SHELL_WIDTH);
        this.drawFlag(ctx, left, size.height, '#aaa', /* fill */ true);
      }
    }
  }

  private drawAreaMarker(
      ctx: CanvasRenderingContext2D, x: number, xEnd: number, color: string,
      fill: boolean) {
    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    const topOffset = 10;
    // Don't draw in the track shell section.
    if (x >= globals.frontendLocalState.timeScale.startPx + TRACK_SHELL_WIDTH) {
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
        x, globals.frontendLocalState.timeScale.startPx + TRACK_SHELL_WIDTH);
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
    ctx.font = '24px Material Icons';
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
    const timeScale = globals.frontendLocalState.timeScale;
    const timestamp = timeScale.pxToTime(x);
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
    const timeScale = globals.frontendLocalState.timeScale;
    const noteX = timeScale.timeToPx(getStartTimestamp(note));
    if (note.noteType === 'AREA') {
      const noteArea = globals.state.areas[note.areaId];
      return (noteX <= x && x < noteX + AREA_TRIANGLE_WIDTH) ||
          (timeScale.timeToPx(noteArea.endSec) > x &&
           x > timeScale.timeToPx(noteArea.endSec) - AREA_TRIANGLE_WIDTH);
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

  renderTabCanvas() {}

  getTitle() {
    return 'Current Selection';
  }

  viewTab() {
    const note = globals.state.notes[this.config.id];
    if (note === undefined) {
      return m('.', `No Note with id ${this.config.id}`);
    }
    const startTime =
        getStartTimestamp(note) - globals.state.traceTime.startSec;
    return m(
        '.notes-editor-panel',
        m('.notes-editor-panel-heading-bar',
          m('.notes-editor-panel-heading',
            `Annotation at ${timeToString(startTime)}`),
          m('input[type=text]', {
            onkeydown: (e: Event) => {
              e.stopImmediatePropagation();
            },
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
          m('button',
            {
              onclick: () => {
                globals.dispatch(Actions.removeNote({id: this.config.id}));
                globals.dispatch(Actions.setCurrentTab({tab: undefined}));
                globals.rafScheduler.scheduleFullRedraw();
              },
            },
            'Remove')),
    );
  }
}

bottomTabRegistry.register(NotesEditorTab);

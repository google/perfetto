// Copyright (C) 2020 The Android Open Source Project
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

import {time} from '../base/time';
import {pluginManager} from '../common/plugins';
import {TrackState} from '../common/state';

import {TRACK_SHELL_WIDTH} from './css_constants';
import {ALL_CATEGORIES, getFlowCategories} from './flow_events_panel';
import {Flow, FlowPoint, globals} from './globals';
import {PanelVNode} from './panel';
import {SliceRect} from './track';
import {TrackGroupPanel} from './track_group_panel';
import {TrackPanel} from './track_panel';

const TRACK_GROUP_CONNECTION_OFFSET = 5;
const TRIANGLE_SIZE = 5;
const CIRCLE_RADIUS = 3;
const BEZIER_OFFSET = 30;

const CONNECTED_FLOW_HUE = 10;
const SELECTED_FLOW_HUE = 230;

const DEFAULT_FLOW_WIDTH = 2;
const FOCUSED_FLOW_WIDTH = 3;

const HIGHLIGHTED_FLOW_INTENSITY = 45;
const FOCUSED_FLOW_INTENSITY = 55;
const DEFAULT_FLOW_INTENSITY = 70;

type LineDirection = 'LEFT'|'RIGHT'|'UP'|'DOWN';
type ConnectionType = 'TRACK'|'TRACK_GROUP';

interface TrackPanelInfo {
  panel: TrackPanel;
  yStart: number;
}

interface TrackGroupPanelInfo {
  panel: TrackGroupPanel;
  yStart: number;
  height: number;
}

function hasTrackKey(obj: {}): obj is {trackKey: number} {
  return (obj as {trackKey?: number}).trackKey !== undefined;
}

function hasTrackGroupId(obj: {}): obj is {trackGroupId: string} {
  return (obj as {trackGroupId?: string}).trackGroupId !== undefined;
}

function getTrackIds(track: TrackState): number[] {
  const trackDesc = pluginManager.resolveTrackInfo(track.uri);
  return trackDesc?.trackIds ?? [];
}

export class FlowEventsRendererArgs {
  trackIdToTrackPanel: Map<number, TrackPanelInfo>;
  groupIdToTrackGroupPanel: Map<string, TrackGroupPanelInfo>;

  constructor(public canvasWidth: number, public canvasHeight: number) {
    this.trackIdToTrackPanel = new Map<number, TrackPanelInfo>();
    this.groupIdToTrackGroupPanel = new Map<string, TrackGroupPanelInfo>();
  }

  registerPanel(panel: PanelVNode, yStart: number, height: number) {
    if (panel.state instanceof TrackPanel && hasTrackKey(panel.attrs)) {
      const track = globals.state.tracks[panel.attrs.trackKey];
      for (const trackId of getTrackIds(track)) {
        this.trackIdToTrackPanel.set(trackId, {panel: panel.state, yStart});
      }

      // Register new "plugin track" ids
      const trackState = globals.state.tracks[panel.attrs.trackKey];
      if (trackState.uri) {
        const trackInfo = pluginManager.resolveTrackInfo(trackState.uri);
        if (trackInfo?.trackIds) {
          for (const trackId of trackInfo.trackIds) {
            this.trackIdToTrackPanel.set(trackId, {panel: panel.state, yStart});
          }
        }
      }
    } else if (
        panel.state instanceof TrackGroupPanel &&
        hasTrackGroupId(panel.attrs)) {
      this.groupIdToTrackGroupPanel.set(
          panel.attrs.trackGroupId, {panel: panel.state, yStart, height});
    }
  }
}

export class FlowEventsRenderer {
  private getTrackGroupIdByTrackId(trackId: number): string|undefined {
    const trackKey = globals.state.trackKeyByTrackId[trackId];
    return trackKey ? globals.state.tracks[trackKey].trackGroup : undefined;
  }

  private getTrackGroupYCoordinate(
      args: FlowEventsRendererArgs, trackId: number): number|undefined {
    const trackGroupId = this.getTrackGroupIdByTrackId(trackId);
    if (!trackGroupId) {
      return undefined;
    }
    const trackGroupInfo = args.groupIdToTrackGroupPanel.get(trackGroupId);
    if (!trackGroupInfo) {
      return undefined;
    }
    return trackGroupInfo.yStart + trackGroupInfo.height -
        TRACK_GROUP_CONNECTION_OFFSET;
  }

  private getTrackYCoordinate(args: FlowEventsRendererArgs, trackId: number):
      number|undefined {
    return args.trackIdToTrackPanel.get(trackId) ?.yStart;
  }

  private getYConnection(
      args: FlowEventsRendererArgs, trackId: number,
      rect?: SliceRect): {y: number, connection: ConnectionType}|undefined {
    if (!rect) {
      const y = this.getTrackGroupYCoordinate(args, trackId);
      if (y === undefined) {
        return undefined;
      }
      return {y, connection: 'TRACK_GROUP'};
    }
    const y = (this.getTrackYCoordinate(args, trackId) || 0) + rect.top +
        rect.height * 0.5;

    return {
      y: Math.min(Math.max(0, y), args.canvasHeight),
      connection: 'TRACK',
    };
  }

  private getXCoordinate(ts: time): number {
    return globals.frontendLocalState.visibleTimeScale.timeToPx(ts);
  }

  private getSliceRect(args: FlowEventsRendererArgs, point: FlowPoint):
      SliceRect|undefined {
    const {visibleTimeScale, visibleTimeSpan, windowSpan} =
        globals.frontendLocalState;
    const trackPanel = args.trackIdToTrackPanel.get(point.trackId) ?.panel;
    if (!trackPanel) {
      return undefined;
    }
    return trackPanel.getSliceRect(
        visibleTimeScale,
        visibleTimeSpan,
        windowSpan,
        point.sliceStartTs,
        point.sliceEndTs,
        point.depth);
  }

  render(ctx: CanvasRenderingContext2D, args: FlowEventsRendererArgs) {
    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    ctx.rect(0, 0, args.canvasWidth - TRACK_SHELL_WIDTH, args.canvasHeight);
    ctx.clip();

    globals.connectedFlows.forEach((flow) => {
      this.drawFlow(ctx, args, flow, CONNECTED_FLOW_HUE);
    });

    globals.selectedFlows.forEach((flow) => {
      const categories = getFlowCategories(flow);
      for (const cat of categories) {
        if (globals.visibleFlowCategories.get(cat) ||
            globals.visibleFlowCategories.get(ALL_CATEGORIES)) {
          this.drawFlow(ctx, args, flow, SELECTED_FLOW_HUE);
          break;
        }
      }
    });

    ctx.restore();
  }

  private drawFlow(
      ctx: CanvasRenderingContext2D, args: FlowEventsRendererArgs, flow: Flow,
      hue: number) {
    const beginSliceRect = this.getSliceRect(args, flow.begin);
    const endSliceRect = this.getSliceRect(args, flow.end);

    const beginYConnection =
        this.getYConnection(args, flow.begin.trackId, beginSliceRect);
    const endYConnection =
        this.getYConnection(args, flow.end.trackId, endSliceRect);

    if (!beginYConnection || !endYConnection) {
      return;
    }

    let beginDir: LineDirection = 'LEFT';
    let endDir: LineDirection = 'RIGHT';
    if (beginYConnection.connection === 'TRACK_GROUP') {
      beginDir = beginYConnection.y > endYConnection.y ? 'DOWN' : 'UP';
    }
    if (endYConnection.connection === 'TRACK_GROUP') {
      endDir = endYConnection.y > beginYConnection.y ? 'DOWN' : 'UP';
    }

    const begin = {
      x: this.getXCoordinate(flow.begin.sliceEndTs),
      y: beginYConnection.y,
      dir: beginDir,
    };
    const end = {
      x: this.getXCoordinate(flow.end.sliceStartTs),
      y: endYConnection.y,
      dir: endDir,
    };
    const highlighted = flow.end.sliceId === globals.state.highlightedSliceId ||
        flow.begin.sliceId === globals.state.highlightedSliceId;
    const focused = flow.id === globals.state.focusedFlowIdLeft ||
        flow.id === globals.state.focusedFlowIdRight;

    let intensity = DEFAULT_FLOW_INTENSITY;
    let width = DEFAULT_FLOW_WIDTH;
    if (focused) {
      intensity = FOCUSED_FLOW_INTENSITY;
      width = FOCUSED_FLOW_WIDTH;
    }
    if (highlighted) {
      intensity = HIGHLIGHTED_FLOW_INTENSITY;
    }
    this.drawFlowArrow(ctx, begin, end, hue, intensity, width);
  }

  private getDeltaX(dir: LineDirection, offset: number): number {
    switch (dir) {
      case 'LEFT':
        return -offset;
      case 'RIGHT':
        return offset;
      case 'UP':
        return 0;
      case 'DOWN':
        return 0;
      default:
        return 0;
    }
  }

  private getDeltaY(dir: LineDirection, offset: number): number {
    switch (dir) {
      case 'LEFT':
        return 0;
      case 'RIGHT':
        return 0;
      case 'UP':
        return -offset;
      case 'DOWN':
        return offset;
      default:
        return 0;
    }
  }

  private drawFlowArrow(
      ctx: CanvasRenderingContext2D,
      begin: {x: number, y: number, dir: LineDirection},
      end: {x: number, y: number, dir: LineDirection}, hue: number,
      intensity: number, width: number) {
    const hasArrowHead = Math.abs(begin.x - end.x) > 3 * TRIANGLE_SIZE;
    const END_OFFSET =
        (((end.dir === 'RIGHT' || end.dir === 'LEFT') && hasArrowHead) ?
             TRIANGLE_SIZE :
             0);
    const color = `hsl(${hue}, 50%, ${intensity}%)`;
    // draw curved line from begin to end (bezier curve)
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.beginPath();
    ctx.moveTo(begin.x, begin.y);
    ctx.bezierCurveTo(
        begin.x - this.getDeltaX(begin.dir, BEZIER_OFFSET),
        begin.y - this.getDeltaY(begin.dir, BEZIER_OFFSET),
        end.x - this.getDeltaX(end.dir, BEZIER_OFFSET + END_OFFSET),
        end.y - this.getDeltaY(end.dir, BEZIER_OFFSET + END_OFFSET),
        end.x - this.getDeltaX(end.dir, END_OFFSET),
        end.y - this.getDeltaY(end.dir, END_OFFSET));
    ctx.stroke();

    // TODO (andrewbb): probably we should add a parameter 'MarkerType' to be
    // able to choose what marker we want to draw _before_ the function call.
    // e.g. triangle, circle, square?
    if (begin.dir !== 'RIGHT' && begin.dir !== 'LEFT') {
      // draw a circle if we the line has a vertical connection
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(begin.x, begin.y, 3, 0, 2 * Math.PI);
      ctx.closePath();
      ctx.fill();
    }


    if (end.dir !== 'RIGHT' && end.dir !== 'LEFT') {
      // draw a circle if we the line has a vertical connection
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(end.x, end.y, CIRCLE_RADIUS, 0, 2 * Math.PI);
      ctx.closePath();
      ctx.fill();
    } else if (hasArrowHead) {
      this.drawArrowHead(end, ctx, color);
    }
  }

  private drawArrowHead(
      end: {x: number; y: number; dir: LineDirection},
      ctx: CanvasRenderingContext2D, color: string) {
    const dx = this.getDeltaX(end.dir, TRIANGLE_SIZE);
    const dy = this.getDeltaY(end.dir, TRIANGLE_SIZE);
    // draw small triangle
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.moveTo(end.x, end.y);
    ctx.lineTo(end.x - dx - dy, end.y + dx - dy);
    ctx.lineTo(end.x - dx + dy, end.y - dx - dy);
    ctx.closePath();
    ctx.fill();
  }
}

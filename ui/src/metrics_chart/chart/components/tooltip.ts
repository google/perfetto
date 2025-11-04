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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {select, Selection} from 'd3';
import {IChartConfig} from '../../config';
import {TOOLTIP_CLASS_NAME} from '../const';
import {ENodeType, TNodeData} from '../types';

type TTooltipNode = {
  nodeData: TNodeData;
  nodeType: ENodeType;
};

export type TTooltipHeaderData = {
  headerName: string;
  nodes: TTooltipNode[];
};

export type TTooltipNodeData = TTooltipNode;

export type TTooltipGroupData = {
  groupName: string;
  nodes: TTooltipNode[];
};

export type TTooltipThreadData = {
  threadName: string;
  nodes: TTooltipNode[];
};

type TTooltipData =
  | {
      type: 'header';
      data: TTooltipHeaderData;
    }
  | {
      type: 'node';
      data: TTooltipNodeData;
    }
  | {
      type: 'group';
      data: TTooltipGroupData;
    }
  | {
      type: 'thread';
      data: TTooltipThreadData;
    };

export class Tooltip {
  private config: IChartConfig;

  readonly selection: Selection<HTMLDivElement, unknown, null, undefined>;

  constructor(config: IChartConfig) {
    this.config = config;
    this.selection = select(document.createElement('div')).call(
      this.createTooltip,
    );
    const selectionNode = this.selection.node();
    if (selectionNode != null) {
      window.document.body.appendChild(selectionNode);
    }
  }

  private createTooltip = (
    selection: Selection<HTMLDivElement, unknown, null, undefined>,
  ) => {
    selection
      .style('position', 'absolute')
      .style('visibility', 'hidden')
      .style('pointer-events', 'none')
      .style('line-height', '1.3')
      .style('font', '11px sans-serif')
      .style('z-index', '99999')
      .attr('class', TOOLTIP_CLASS_NAME);
  };

  private createTooltipPosition(
    pageX: number,
    _pageY: number,
    offsetX: number,
    offsetY: number,
    width: number,
    height: number,
  ) {
    let style =
      offsetX < width / 2
        ? offsetY < height / 2
          ? 'transform: translate(0, 0); left: 20px; top: 20px;'
          : 'transform: translate(0, -100%); left: 20px; bottom: 20px;'
        : offsetY < height / 2
          ? 'transform: translate(-100%, 0); right: 20px; top: 20px;'
          : 'transform: translate(-100%, -100%); right: 20px; bottom: 20px;';

    style += `max-width: ${Math.max(width / 2, pageX)}px`;

    return style;
  }

  htmlText(style: string, content: string) {
    return `<div style="
      position: relative; 
      background: white; 
      box-shadow: 0 0 10px rgba(0,0,0,.25); 
      border-radius: 6px; 
      padding: 10px; 
      white-space: normal;
      overflow-wrap: break-word;
      word-break: break-word;
      pointer-events: none;
      ${style}">
      ${content}
    <div>`;
  }

  show(
    val: TTooltipData,
    pos: {pageX: number; pageY: number; offsetX: number; offsetY: number},
    canvasWidth: number,
    canvasHeight: number,
  ) {
    let content: string | undefined = '';
    const {type, data} = val;

    const style = this.createTooltipPosition(
      pos.pageX,
      pos.pageY,
      pos.offsetX,
      pos.offsetY,
      canvasWidth,
      canvasHeight,
    );

    if (type === 'node') {
      content = this.config.tooltip?.nodeFormatter?.(data, this.config);
    }

    if (!content) {
      console.warn('[chart] no content for tooltip');
      this.hide();
      return;
    }

    this.selection
      .html(this.htmlText(style, content))
      .style('left', `${pos.pageX}px`)
      .style('top', `${pos.pageY}px`)
      .style('visibility', 'visible');
  }

  hide = () => {
    this.selection.style('visibility', 'hidden');
  };
}

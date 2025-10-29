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

import m from 'mithril';
import {createRoot, Root} from 'react-dom/client';
import React, {Component} from 'react';
import {FocusLynxViewDetailPanel} from './focus_lynxview_panel';
import {lynxPerfGlobals} from '../../../lynx_perf/lynx_perf_globals';
import {RightSidebarTab} from '../../../lynx_perf/types';
import {TraceAssistantPanel} from './assistant_panel';
import { RIGHT_SIDEBAR_MAX_WIDTH, RIGHT_SIDEBAR_MIN_WIDTH } from '../../../lynx_perf/constants';

export class SidebarDetailView implements m.ClassComponent {
  private root: Root | undefined;

  oncreate(vnode: m.CVnodeDOM) {
    this.onRender(vnode);
  }

  onupdate(vnode: m.CVnodeDOM) {
    this.onRender(vnode);
  }

  onRender(vnode: m.CVnodeDOM) {
    if (!this.root) {
      this.root = createRoot(vnode.dom);
    }
    this.root.render(<DetailViewPanel />);
  }

  view() {
    return m('.rightbar-page');
  }
}

export class DetailViewPanel extends Component {
  private startX = 0;
  private startWidth = 0;
  private resizerRef = React.createRef<HTMLDivElement>();
  private containerRef = React.createRef<HTMLDivElement>();

  constructor() {
    super({});
    this.handleMouseDown = this.handleMouseDown.bind(this);
    this.handleMouseMove = this.handleMouseMove.bind(this);
    this.handleMouseUp = this.handleMouseUp.bind(this);
  }

  componentDidMount() {
    document.addEventListener('mousemove', this.handleMouseMove);
    document.addEventListener('mouseup', this.handleMouseUp);
  }

  componentWillUnmount() {
    document.removeEventListener('mousemove', this.handleMouseMove);
    document.removeEventListener('mouseup', this.handleMouseUp);
  }

  handleMouseDown(e: React.MouseEvent) {
    this.startX = e.clientX;
    this.startWidth = parseInt(
      document.documentElement.style.getPropertyValue(
        '--right-sidebar-width',
      ) || `${RIGHT_SIDEBAR_MIN_WIDTH}`,
      10,
    );
    document.documentElement.classList.add('dragging');
  }

  handleMouseMove(e: MouseEvent) {
    if (!document.documentElement.classList.contains('dragging')) return;

    const dx = e.clientX - this.startX;
    const newWidth = Math.max(RIGHT_SIDEBAR_MIN_WIDTH, Math.min(RIGHT_SIDEBAR_MAX_WIDTH, this.startWidth - dx));
    document.documentElement.style.setProperty(
      '--right-sidebar-width',
      `${newWidth}px`,
    );
    lynxPerfGlobals.changeRightSidebarWidth(newWidth);
  }

  handleMouseUp() {
    document.documentElement.classList.remove('dragging');
  }

  render() {
    return (
      <div
        className="rightbar-container"
        ref={this.containerRef}
        style={{height: '100%', display: 'flex'}}>
        <div
          className="rightbar-container-resizer"
          ref={this.resizerRef}
          onMouseDown={this.handleMouseDown}
          style={{width: '2px', flexShrink: 0}}>
        </div>
        <div style={{flex: 1}}>
          {lynxPerfGlobals.state.rightSidebarTab === RightSidebarTab.LynxView && (
          <FocusLynxViewDetailPanel />
          )}
          {lynxPerfGlobals.state.rightSidebarTab ===
          RightSidebarTab.TraceAssistant && <TraceAssistantPanel />}
        </div>
      </div>
    );
  }
}

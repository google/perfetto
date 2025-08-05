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
import {Drawer} from 'construct-ui';
import {Editor} from '../widgets/editor';
import {DragGestureHandler} from '../base/drag_gesture_handler';
import {DisposableStack} from '../base/disposable_stack';
import {sourceMapState} from './source_map_state';
import {raf} from '../core/raf_scheduler';
import {eventLoggerState} from '../event_logger';

function getFullScreenWidth() {
  const page = document.querySelector('.page') as HTMLElement;
  if (page === null) {
    return 800;
  } else {
    return page.clientWidth;
  }
}

export interface DragHandleAttrs {
  // The current height of the panel.
  width: number;
  // Called when the panel is dragged.
  resize: (width: number) => void;
  onClose?: () => void;
}

class SourceFileDrawerHandler implements m.ClassComponent<DragHandleAttrs> {
  private dragStartX = 0;
  private width = 0;
  private resize: (height: number) => void = () => {};
  private gesture?: DragGestureHandler;
  private trash = new DisposableStack();

  view() {
    return m('.source-file-handle');
  }

  oncreate({dom, attrs}: m.CVnodeDOM<DragHandleAttrs>) {
    if (dom !== null) {
      this.gesture = new DragGestureHandler(
        dom as HTMLElement,
        this.onDrag.bind(this),
        this.onDragStart.bind(this),
        this.onDragEnd.bind(this),
      );
      this.trash.use(this.gesture);
    }
    this.resize = attrs.resize;
    this.width = attrs.width;
  }

  onremove() {
    this.trash.dispose();
  }

  onupdate({dom, attrs}: m.VnodeDOM<DragHandleAttrs>) {
    if (dom !== null && !this.gesture) {
      this.gesture = new DragGestureHandler(
        dom as HTMLElement,
        this.onDrag.bind(this),
        this.onDragStart.bind(this),
        this.onDragEnd.bind(this),
      );
      this.trash.use(this.gesture);
    }
    this.width = attrs.width;
  }

  private onDrag(x: number) {
    const newWidth = Math.floor(this.dragStartX - x);
    this.resize(newWidth);
  }

  private onDragStart(_x: number) {
    this.dragStartX = this.width;
  }

  private onDragEnd() {}
}

export class SourceFileDrawer implements m.ClassComponent<{}> {
  private fillScreenWidth = getFullScreenWidth();
  private readonly defaultWidth = this.fillScreenWidth * 0.5;
  private currentWidth = this.defaultWidth;
  private generation = 0;

  private onClose() {
    sourceMapState.edit((draft) => {
      draft.currentSourceFile = undefined;
      draft.sourceFileDrawerVisible = false;
    });
    raf.scheduleFullRedraw();
  }

  onupdate() {
    this.generation = this.generation + 1;
  }

  public view() {
    let isOpen = false;
    let content: string | undefined;
    let line = -1;
    let column = -1;
    if (sourceMapState.state.currentSourceFile) {
      eventLoggerState.state.eventLogger.logEvent('lynx_feature_usage', {
        type: 'SourceFile',
      });
      const sourceFiles = sourceMapState.state.currentSourceFile.split(':');
      if (sourceFiles.length >= 3) {
        const file = sourceFiles.slice(0, sourceFiles.length - 2).join(':');
        content = sourceMapState.state.sourceFile[file]?.content;
        line = parseInt(sourceFiles[sourceFiles.length - 2] ?? '-1');
        column = parseInt(sourceFiles[sourceFiles.length - 2] ?? '-1');
        if (column !== -1 && line !== -1 && content !== undefined) {
          isOpen = true;
        }
      }
    }
    if (!isOpen) {
      return null;
    }
    sourceMapState.edit((draft) => {
      draft.sourceFileDrawerVisible = true;
    });
    return m(Drawer, {
      closeOnEscapeKey: true,
      closeOnOutsideClick: false,
      content: [
        m(
          '.drawer-content',
          m(SourceFileDrawerHandler, {
            width: this.currentWidth,
            resize: (width: number) => {
              if (width > this.fillScreenWidth) {
                width = this.fillScreenWidth;
              }
              if (width < 100) {
                this.currentWidth = this.defaultWidth;
                this.onClose();
              } else {
                this.currentWidth = width;
              }
              raf.scheduleFullRedraw();
            },
          }),
          m(Editor, {
            initialText: content,
            line: line,
            generation: this.generation + 1,
            readonly: true,
          }),
        ),
      ],
      inline: true,
      hasBackdrop: false,
      position: 'right',
      isOpen: isOpen,
      onClose: () => {
        this.onClose();
      },
      style: {
        width: this.currentWidth + 'px',
        overflow: 'hidden',
      },
    });
  }
}

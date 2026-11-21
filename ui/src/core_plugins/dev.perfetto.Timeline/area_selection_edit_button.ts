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

import m from 'mithril';
import {DisposableStack} from '../../base/disposable_stack';
import {bindEventListener, toHTMLElement} from '../../base/dom_utils';
import type {Rect2D} from '../../base/geom';
import {Icons} from '../../base/semantic_icons';
import type {TraceImpl} from '../../core/trace_impl';
import {Button} from '../../widgets/button';
import './area_selection_edit_button.scss';

interface Point {
  readonly x: number;
  readonly y: number;
}

/**
 * A translucent button shown while hovering over the area selection, which
 * toggles edit mode.
 *
 * The button is rendered as DOM (rather than drawn on the canvas) so that it
 * intercepts clicks before the tracks beneath it do. It must be rendered as a
 * direct child of the VirtualOverlayCanvas content, so that its position can
 * be expressed in canvas coordinates.
 */
export class AreaSelectionEditButton {
  private el?: HTMLElement;
  private readonly trash = new DisposableStack();
  private mouse?: Point;

  constructor(private readonly trace: TraceImpl) {}

  render(): m.Children {
    const editor = this.trace.areaSelectionEditor;
    return m(
      '.pf-area-selection-edit-button',
      {
        oncreate: ({dom}: m.VnodeDOM) => this.onCreate(toHTMLElement(dom)),
        onremove: () => this.onRemove(),
      },
      m(Button, {
        icon: Icons.Edit,
        active: editor.isEditing,
        title: editor.isEditing
          ? 'Stop editing selection'
          : 'Edit selection (WASD to move and resize)',
        onclick: () => {
          editor.toggleEditing();
          this.trace.raf.scheduleFullRedraw();
        },
      }),
    );
  }

  update(areas: ReadonlyArray<Rect2D>): void {
    const el = this.el;
    if (el === undefined) return;
    const mouse = this.mouse;
    const area = mouse && areas.find((a) => a.containsPoint(mouse));
    if (mouse !== undefined && area !== undefined) {
      el.style.display = 'flex';
      el.style.left = `${(area.left + area.right) / 2}px`;
      el.style.top = `${mouse.y}px`;
    } else if (!el.matches(':hover')) {
      el.style.display = 'none';
    }
  }

  private onCreate(el: HTMLElement) {
    this.el = el;
    const canvasContent = el.parentElement;
    if (canvasContent === null) return;
    this.trash.use(
      bindEventListener(canvasContent, 'mousemove', (e) => {
        // Get out of the way of other mouse interactions, like drag-selecting.
        const isDragging = e.buttons !== 0 && !el.contains(e.target as Node);
        this.mouse = isDragging ? undefined : this.toCanvasCoords(e);
        this.trace.raf.scheduleCanvasRedraw();
      }),
    );
    this.trash.use(
      bindEventListener(canvasContent, 'mouseleave', () => {
        this.mouse = undefined;
        this.trace.raf.scheduleCanvasRedraw();
      }),
    );
  }

  private onRemove() {
    this.trash.dispose();
    this.el = undefined;
    this.mouse = undefined;
  }

  private toCanvasCoords(e: MouseEvent): Point | undefined {
    const canvasContent = this.el?.parentElement;
    if (!canvasContent) return undefined;
    const rect = canvasContent.getBoundingClientRect();
    return {x: e.clientX - rect.left, y: e.clientY - rect.top};
  }
}

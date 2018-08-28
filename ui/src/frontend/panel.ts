// Copyright (C) 2018 The Android Open Source Project
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

let nextPanelId = 0;

export abstract class Panel {
  // Each panel has a unique string id. This is suitable for use as a mithril
  // component key.
  readonly id: string;

  constructor() {
    this.id = 'panel-id-' + (nextPanelId++).toString();
  }

  abstract renderCanvas(ctx: CanvasRenderingContext2D): void;
  abstract updateDom(dom: HTMLElement): void;

  // TODO: If a panel changes its height, we need to call m.redraw. Instead of
  // getHeight, we can have an setHeight method in the abstract class that does
  // that redraw call.
  abstract getHeight(): number;
}

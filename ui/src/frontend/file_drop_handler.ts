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

import {Actions} from '../common/actions';
import {globals} from './globals';

let lastDragTarget: EventTarget|null = null;

export function installFileDropHandler() {
  window.ondragenter = (evt: DragEvent) => {
    evt.preventDefault();
    evt.stopPropagation();
    lastDragTarget = evt.target;
    if (dragEventHasFiles(evt)) {
      document.body.classList.add('filedrag');
    }
  };

  window.ondragleave = (evt: DragEvent) => {
    evt.preventDefault();
    evt.stopPropagation();
    if (evt.target === lastDragTarget) {
      document.body.classList.remove('filedrag');
    }
  };

  window.ondrop = (evt: DragEvent) => {
    evt.preventDefault();
    evt.stopPropagation();
    document.body.classList.remove('filedrag');
    if (evt.dataTransfer && dragEventHasFiles(evt)) {
      const file = evt.dataTransfer.files[0];
      if (file) {
        globals.dispatch(Actions.openTraceFromFile({file}));
      }
    }
    evt.preventDefault();
  };

  window.ondragover = (evt: DragEvent) => {
    evt.preventDefault();
    evt.stopPropagation();
  };
}

function dragEventHasFiles(event: DragEvent): boolean {
  if (event.dataTransfer && event.dataTransfer.types) {
    for (const type of event.dataTransfer.types) {
      if (type === 'Files') return true;
    }
  }
  return false;
}

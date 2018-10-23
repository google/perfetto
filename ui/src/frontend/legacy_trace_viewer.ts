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

import {assertTrue} from '../base/logging';
import {globals} from './globals';

export function isLegacyTrace(fileName: string): boolean {
  fileName = fileName.toLowerCase();
  return (
      fileName.endsWith('.json') || fileName.endsWith('.json.gz') ||
      fileName.endsWith('.zip') || fileName.endsWith('.ctrace'));
}

export function openFileWithLegacyTraceViewer(file: File) {
  const reader = new FileReader();
  reader.onload = () => {
    if (reader.result instanceof ArrayBuffer) {
      return openBufferWithLegacyTraceViewer(
          file.name, reader.result, reader.result.byteLength);
    } else {
      const str = reader.result as string;
      return openBufferWithLegacyTraceViewer(file.name, str, str.length);
    }
  };
  reader.onerror = err => {
    console.error(err);
  };
  if (file.name.endsWith('.gz') || file.name.endsWith('.zip')) {
    reader.readAsArrayBuffer(file);
  } else {
    reader.readAsText(file);
  }
}

export function openBufferWithLegacyTraceViewer(
    name: string, data: ArrayBuffer|string, size: number) {
  if (data instanceof ArrayBuffer) {
    assertTrue(size <= data.byteLength);
    if (size !== data.byteLength) {
      data = data.slice(0, size);
    }
  }
  document.body.style.transition =
      'filter 1s ease, transform 1s cubic-bezier(0.985, 0.005, 1.000, 0.225)';
  document.body.style.filter = 'grayscale(1) blur(10px) opacity(0)';
  document.body.style.transform = 'scale(0)';
  const transitionPromise = new Promise(resolve => {
    document.body.addEventListener('transitionend', (e: TransitionEvent) => {
      if (e.propertyName === 'transform') {
        resolve();
      }
    });
  });

  const loadPromise = new Promise(resolve => {
    fetch('/assets/catapult_trace_viewer.html').then(resp => {
      resp.text().then(content => {
        resolve(content);
      });
    });
  });

  Promise.all([loadPromise, transitionPromise]).then(args => {
    const fetchResult = args[0] as string;
    replaceWindowWithTraceViewer(name, data, fetchResult);
  });
}

// Replaces the contents of the current window with the Catapult's legacy
// trace viewer HTML, passed in |htmlContent|.
// This is in its own function to avoid leaking variables from the current
// document we are about to destroy.
function replaceWindowWithTraceViewer(
    name: string, data: ArrayBuffer|string, htmlContent: string) {
  globals.shutdown();
  const newWin = window.open('', '_self') as Window;
  newWin.document.open('text/html', 'replace');
  newWin.document.addEventListener('readystatechange', () => {
    const doc = newWin.document;
    if (doc.readyState !== 'complete') return;
    const ctl = doc.querySelector('x-profiling-view') as TraceViewerAPI;
    ctl.setActiveTrace(name, data);
  });
  newWin.document.write(htmlContent);
  newWin.document.close();
}

// TraceViewer method that we wire up to trigger the file load.
interface TraceViewerAPI extends Element {
  setActiveTrace(name: string, data: ArrayBuffer|string): void;
}
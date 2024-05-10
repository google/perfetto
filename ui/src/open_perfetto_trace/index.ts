// Copyright (C) 2024 The Android Open Source Project
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

// open-perfetto-trace is a standalone JS/TS library that can be used in other
// projects to facilitate the deep linking into perfetto. It allows opening
// trace files or URL with ui.perfetto.dev, handling all the handshake with it.

const PERFETTO_UI_URL = 'https://ui.perfetto.dev';

interface OpenTraceOptions {
  // If true (default) shows a popup dialog with a progress bar that informs
  // about the status of the fetch. This is only relevant when the trace source
  // is a url.
  statusDialog?: boolean;

  // Opens the trace in a new tab.
  newTab?: boolean;

  // Override the referrer. Useful for scripts such as
  // record_android_trace to record where the trace is coming from.
  referrer?: string;

  // For the 'mode' of the UI. For example when the mode is 'embedded'
  // some features are disabled.
  mode: 'embedded' | undefined;

  // Hides the sidebar in the opened perfetto UI.
  hideSidebar?: boolean;

  ts?: string;

  dur?: string;
  tid?: string;
  pid?: string;
  query?: string;
  visStart?: string;
  visEnd?: string;

  // Used to override ui.perfetto.dev with a custom hosted URL.
  // Useful for testing.
  uiUrl?: string;
}

// Opens a trace in the Perfetto UI.
// `source` can be either:
// - A blob (e.g. a File).
// - A URL.
export default function openPerfettoTrace(
  source: Blob | string,
  opts?: OpenTraceOptions,
) {
  if (source instanceof Blob) {
    return openTraceBlob(source, opts);
  } else if (typeof source === 'string') {
    return fetchAndOpenTrace(source, opts);
  }
}

function openTraceBlob(blob: Blob, opts?: OpenTraceOptions) {
  const form = document.createElement('form');
  form.method = 'POST';
  form.style.visibility = 'hidden';
  form.enctype = 'multipart/form-data';
  const uiUrl = opts?.uiUrl ?? PERFETTO_UI_URL;
  form.action = `${uiUrl}/_open_trace/${Date.now()}`;
  if (opts?.newTab === true) {
    form.target = '_blank';
  }
  const fileInput = document.createElement('input');
  fileInput.name = 'trace';
  fileInput.type = 'file';
  const dataTransfer = new DataTransfer();
  dataTransfer.items.add(new File([blob], 'trace.file'));
  fileInput.files = dataTransfer.files;
  form.appendChild(fileInput);
  for (const [key, value] of Object.entries(opts ?? {})) {
    const varInput = document.createElement('input');
    varInput.type = 'hidden';
    varInput.name = key;
    varInput.value = value;
    form.appendChild(varInput);
  }
  document.body.appendChild(form);
  form.submit();
}

function fetchAndOpenTrace(url: string, opts?: OpenTraceOptions) {
  updateProgressDiv({status: 'Fetching trace'}, opts);
  const xhr = new XMLHttpRequest();
  xhr.addEventListener('progress', (event) => {
    if (event.lengthComputable) {
      updateProgressDiv(
        {
          status: `Fetching trace (${Math.round(event.loaded / 1000)} KB)`,
          progress: event.loaded / event.total,
        },
        opts,
      );
    }
  });
  xhr.addEventListener('loadend', () => {
    if (xhr.readyState === 4 && xhr.status === 200) {
      const blob = xhr.response as Blob;
      updateProgressDiv({status: 'Opening trace'}, opts);
      openTraceBlob(blob, opts);
      updateProgressDiv({close: true}, opts);
    }
  });
  xhr.addEventListener('error', () => {
    updateProgressDiv({status: 'Failed to fetch trace'}, opts);
  });
  xhr.responseType = 'blob';
  xhr.overrideMimeType('application/octet-stream');
  xhr.open('GET', url);
  xhr.send();
}

interface ProgressDivOpts {
  status?: string;
  progress?: number;
  close?: boolean;
}

function updateProgressDiv(progress: ProgressDivOpts, opts?: OpenTraceOptions) {
  if (opts?.statusDialog === false) return;

  const kDivId = 'open_perfetto_trace';
  let div = document.getElementById(kDivId);
  if (!div) {
    div = document.createElement('div');
    div.id = kDivId;
    div.style.all = 'initial';
    div.style.position = 'fixed';
    div.style.bottom = '10px';
    div.style.left = '0';
    div.style.right = '0';
    div.style.width = 'fit-content';
    div.style.height = '20px';
    div.style.padding = '10px';
    div.style.zIndex = '99';
    div.style.margin = 'auto';
    div.style.backgroundColor = '#fff';
    div.style.color = '#333';
    div.style.fontFamily = 'monospace';
    div.style.fontSize = '12px';
    div.style.border = '1px solid #eee';
    div.style.boxShadow = '0 0 20px #aaa';
    div.style.display = 'flex';
    div.style.flexDirection = 'column';

    const title = document.createElement('div');
    title.className = 'perfetto-open-title';
    title.innerText = 'Opening perfetto trace';
    title.style.fontWeight = '12px';
    title.style.textAlign = 'center';
    div.appendChild(title);

    const progressbar = document.createElement('progress');
    progressbar.className = 'perfetto-open-progress';
    progressbar.style.width = '200px';
    progressbar.value = 0;
    div.appendChild(progressbar);

    document.body.appendChild(div);
  }
  const title = div.querySelector('.perfetto-open-title') as HTMLElement;
  if (progress.status !== undefined) {
    title.innerText = progress.status;
  }
  const bar = div.querySelector('.perfetto-open-progress') as HTMLInputElement;
  if (progress.progress === undefined) {
    bar.style.visibility = 'hidden';
  } else {
    bar.style.visibility = 'visible';
    bar.value = `${progress.progress}`;
  }
  if (progress.close === true) {
    div.remove();
  }
}

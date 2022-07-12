// Copyright (C) 2022 The Android Open Source Project
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

// Helper function to create DOM elements faster: takes a Mithril-style
// "selector" of the form "tag.class1.class2" and a list of child objects that
// can be either strings or DOM elements.
function m(selector, ...children) {
  const parts = selector.split('.');
  if (parts.length === 0) {
    throw new Error(
        'Selector passed to element should be of a form tag.class1.class2');
  }

  const result = document.createElement(parts[0]);
  for (let i = 1; i < parts.length; i++) {
    result.classList.add(parts[i]);
  }
  for (const child of children) {
    if (typeof child === 'string') {
      const childNode = document.createTextNode(child);
      result.appendChild(childNode);
    } else {
      result.appendChild(child);
    }
  }
  return result;
}

function getCiRun() {
  const url = new URL(window.location.href);
  const parts = url.pathname.split('/');

  // Example report URL:
  // https://storage.googleapis.com/perfetto-ci-artifacts/20220711123401--cls-2149676-1--ui-clang-x86_64-release/ui-test-artifacts/index.html
  // Parts would contain ['', 'perfetto-ci-artifacts',
  // '20220711123401--cls-2149676-1--ui-clang-x86_64-release', ...] in this
  // case, which means that we need to check length of the array and get third
  // element out of it.
  if (parts.length >= 3) {
    return parts[2];
  }
  return null;
}

function imageLinkElement(path) {
  const img = m('img');
  img.src = path;
  const link = m('a');
  link.appendChild(img);
  link.href = path;
  link.target = '_blank';
  return link;
}

function processLines(lines) {
  const container = document.querySelector('.container');
  container.innerHTML = '';
  const children = [];

  // report.txt is a text file with a pair of file names on each line, separated
  // by semicolon. E.g. "screenshot.png;screenshot-diff.png"
  for (const line of lines) {
    // Skip empty lines (happens when the file is completely empty).
    if (line.length === 0) {
      continue;
    }

    const parts = line.split(';');
    if (parts.length !== 2) {
      console.warn(
          `Malformed line (expected two files separated via semicolon) ${
              line}!`);
      continue;
    }

    const [output, diff] = parts;
    children.push(m(
        'div.row',
        m('div.cell', output, m('div.image-wrapper', imageLinkElement(output))),
        m('div.cell', diff, m('div.image-wrapper', imageLinkElement(diff)))));
  }

  if (children.length === 0) {
    container.appendChild(m('div', 'All good!'));
    return;
  }

  const run = getCiRun();

  if (run !== null) {
    const cmd = `tools/download_changed_screenshots.py ${run}`;
    const button = m('button', 'Copy');
    button.addEventListener('click', async () => {
      await navigator.clipboard.writeText(cmd);
      button.innerText = 'Copied!';
    });

    container.appendChild(m(
        'div.message',
        'Use following command from Perfetto checkout directory to apply the ' +
            'changes: ',
        m('span.cmd', cmd),
        button));
  }

  for (const child of children) {
    container.appendChild(child);
  }
}

async function loadDiffs() {
  try {
    const report = await fetch('report.txt');
    const response = await report.text();
    processLines(response.split('\n'));
  } catch (e) {
    // report.txt is not available when all tests have succeeded, treat fetching
    // error as absence of failures
    processLines([]);
  }
}

document.addEventListener('DOMContentLoaded', () => {
  loadDiffs();
});

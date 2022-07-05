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

function processLines(lines) {
  const container = document.querySelector('.container');
  container.innerHTML = '';

  // report.txt is a text file with a pair of file names on each line, separated
  // by semicolon. E.g. "screenshot.png;screenshot-diff.png"
  for (const line of lines) {
    const parts = line.split(';');
    if (parts.length !== 2) {
      console.warn(
          `Malformed line (expected two files separated via semicolon) ${
              line}!`);
      continue;
    }

    const [output, diff] = parts;
    const outputImage = m('img');
    outputImage.src = output;
    const diffImage = m('img');
    diffImage.src = diff;

    container.appendChild(
        m('div.row',
          m('div.cell', output, m('div.image-wrapper', outputImage)),
          m('div.cell', diff, m('div.image-wrapper', diffImage))));
  }

  if (lines.length === 0) {
    container.appendChild(m('div', 'All good!'));
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

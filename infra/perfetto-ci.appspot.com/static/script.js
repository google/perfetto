/**
 * Copyright (c) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License. You may
 * obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

'use strict';

const REPO_URL = 'https://android.googlesource.com/platform/external/perfetto/';
const GERRIT_REVIEW_URL = 'https://android-review.googlesource.com/c/platform/external/perfetto';
const CHANGES_URL = '/changes/?q=project:platform/external/perfetto+-age:7days+-is:abandoned&o=DETAILED_ACCOUNTS';
const REPO = 'catapult-project/perfetto';

let botIndex = {};

// Builds a map of bot name -> column index, e.g.:
// {'linux-clang-x86_64-relese' -> 1, 'android-clang-arm-debug' -> 2}.
function GetColumnIndexes() {
  const cols = document.getElementById('cls_header').children;
  for (let i = 0; i < cols.length; i++) {
    const id = cols[i].id;
    if (id)
      botIndex[id] = i + 4 /* 4 = subject...updated columns */;
  }
}

function GetTravisStatusForJob(jobId, div) {
  fetch('https://api.travis-ci.org/jobs/' + jobId)
    .then(response => {
      if (response.status != 200)
        throw 'Unable to make request to Travis';
      return response.json();
    })
    .then(resp => {
      let jobName = resp.config.env.split(' ')[0];
      if (jobName.startsWith('CFG='))
        jobName = jobName.substring(4);
      if (!(jobName in botIndex))
        return;
      let link = document.createElement('a');
      link.href = 'https://travis-ci.org/' + REPO + '/jobs/' + jobId;
      link.title = resp.state + ' [' + jobName + ']';
      let jobState = resp.state;
      if (resp.state == 'finished' && resp.result !== 0)
        jobState = 'errored';
      link.classList.add(jobState);
      if (jobState == 'finished')
        link.innerHTML = '<i class="material-icons">check_circle</i>';
      else if (jobState == 'created')
        link.innerHTML = '<i class="material-icons">autorenew</i>';
      else if (jobState == 'errored' || jobState == 'cancelled')
        link.innerHTML = '<i class="material-icons">bug_report</i>';
      else
        link.innerHTML = '<i class="material-icons">hourglass_full</i>';
      let td = div.children[botIndex[jobName]];
      td.innerHTML = '';
      td.appendChild(link);
    });
}

function GetTravisStatusForBranch(branch, div) {
  fetch('https://api.travis-ci.org/repos/' + REPO + '/branches/' + branch)
    .then(response => {
      if (response.status != 200)
        throw 'Unable to make request to Travis';
      return response.json()
    })
    .then(resp => {
      for (const jobId of resp.branch.job_ids)
        GetTravisStatusForJob(jobId, div);
    });
}

function CreateRowForBranch(branch, href, subject, status, author, updated) {
  let table = document.getElementById('cls');
  let tr = document.createElement('tr');
  tr.classList.add(status);

  let link = document.createElement('a');
  link.href = href;
  link.innerText = subject;
  let td = document.createElement('td');
  td.appendChild(link);
  tr.appendChild(td);

  td = document.createElement('td');
  td.innerText = status;
  tr.appendChild(td);

  td = document.createElement('td');
  td.innerText = author;
  tr.appendChild(td);

  td = document.createElement('td');
  td.innerText = updated;
  tr.appendChild(td);

  for (let _ in botIndex) {
    td = document.createElement('td');
    td.classList.add('job');
    tr.appendChild(td);
  }
  table.appendChild(tr);
  GetTravisStatusForBranch(branch, tr);
}

function LoadGerritCLs() {
  fetch(CHANGES_URL)
    .then(response => {
      if (response.status != 200)
        throw 'Unable to make request to Travis';
      return response.text();
    })
    .then(text => {
      let json;
      if (text.startsWith(')]}\''))
        json = text.substring(4);
      else
        json = text;
      let resp = JSON.parse(json);
      for (const cl of resp) {
        const branch = 'changes/' + cl._number;
        const href = GERRIT_REVIEW_URL + '/+/' + cl._number;;
        const lastUpdate = new Date(cl.updated + ' UTC');
        const lastUpdateMins = Math.ceil((Date.now() - lastUpdate) / 60000);
        let lastUpdateText = '';
        if (lastUpdateMins < 60)
          lastUpdateText = lastUpdateMins + ' mins ago';
        else if (lastUpdateMins < 60 * 24)
          lastUpdateText = Math.ceil(lastUpdateMins / 60) + ' hours ago';
        else
          lastUpdateText = lastUpdate.toLocaleDateString();
        CreateRowForBranch(branch, href, cl.subject, cl.status,
            cl.owner.email.replace('@google.com', '@'), lastUpdateText);
      }
    });
}

// Register the service worker to cache job requests.
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('/service_worker.js', { scope: '/' });
}

// Fetch the CLs and the corresponding status for the Travis jobs.
GetColumnIndexes();
CreateRowForBranch('master', REPO_URL, '*** master branch ***', 'MASTER', '', '');
LoadGerritCLs();

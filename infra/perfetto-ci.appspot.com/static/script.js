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
const CHANGES_URL = '/changes/?q=project:platform/external/perfetto+-age:7days&o=DETAILED_ACCOUNTS';
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
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState != 4 || this.status != 200)
      return;
    let resp = JSON.parse(this.responseText);
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
  };
  xhr.open('GET', 'https://api.travis-ci.org/jobs/' + jobId, true);
  xhr.send();
}

function GetTravisStatusForBranch(branch, div) {
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 404) {
      return;
    }
    if (this.readyState != 4 || this.status != 200)
      return;
    let resp = JSON.parse(this.responseText);
    for (const jobId of resp.branch.job_ids)
      GetTravisStatusForJob(jobId, div);
  };
  let url = ('https://api.travis-ci.org/repos/' + REPO + '/branches/' + branch);
  xhr.open('GET', url, true);
  xhr.send();
}


function CreateRowForBranch(branch, href, subject, status, author, updated) {
    let table = document.getElementById('cls');
    let tr = document.createElement('tr');

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
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState != 4 || this.status != 200)
      return;
    let json = this.responseText;
    if (json.startsWith(')]}\''))
      json = json.substring(4);
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
  };
  xhr.open('GET', CHANGES_URL, true);
  xhr.send();
}

GetColumnIndexes();
CreateRowForBranch('master', REPO_URL, '*** master branch ***', 'MASTER', '', '');
LoadGerritCLs();

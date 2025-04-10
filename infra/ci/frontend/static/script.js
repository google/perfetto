/**
 * Copyright (c) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the 'License'); you
 * may not use this file except in compliance with the License. You may
 * obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an 'AS IS' BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

"use strict";

// If you add or remove job types, do not forget to fix the colspans below.
const JOB_TYPES = [
  { id: "linux/gcc8-x86_64-release", label: "rel" },
  { id: "linux/clang-x86_64-debug", label: "dbg" },
  { id: "linux/clang-x86_64-tsan", label: "tsan" },
  { id: "linux/clang-x86_64-msan", label: "msan" },
  { id: "linux/clang-x86_64-asan_lsan", label: "{a,l}san" },
  { id: "linux/clang-x86-release", label: "x86 rel" },
  { id: "fuzzer", label: "fuzzer" },
  { id: "bazel", label: "bazel" },
  { id: "ui", label: "rel" },
  { id: "android", label: "rel" },
  { id: "repo-checks", label: "RC" },
];

const STATS_LINK =
  "https://app.google.stackdriver.com/dashboards/5008687313278081798?project=perfetto-ci";

const state = {
  // An array of recent CL objects retrieved from GitHub.
  pullReqs: [],

  // pullReqs.id -> {JOB_TYPES.id -> {status, url}}
  checks: {},

  // pullReqs.id -> [{id, title}]
  patchsets: {},

  // An array similar to pullReqs, but for commits in main.
  mainCommits: [],

  // Action Runners.
  runners: [],

  // Workflows runs.
  workflowRuns: [],

  // Maps 'CL number' -> true|false. Retains the collapsed/expanded information
  // for each row in the CLs table.
  expandCl: {},

  limitPostsubmit: 3,

  redrawPending: false,
};

let term = undefined;
let fitAddon = undefined;
let searchAddon = undefined;

function main() {
  m.route(document.body, "/cls", {
    "/cls": CLsPageRenderer,
    "/cls/:cl": CLsPageRenderer,
    "/jobs": JobsPageRenderer,
    "/jobs/:jobId": JobsPageRenderer,
  });

  setInterval(fetchPullRequests, 30000);
  fetchPullRequests();
  getMainCommits();
}

// -----------------------------------------------------------------------------
// Rendering functions
// -----------------------------------------------------------------------------

function renderHeader() {
  const active = (id) => (m.route.get().startsWith(`/${id}`) ? ".active" : "");
  const logUrl = "https://goto.google.com/perfetto-ci-logs-";
  const docsUrl =
    "https://perfetto.dev/docs/design-docs/continuous-integration";
  return m(
    "header",
    m("a[href=/#!/cls]", m("h1", "Perfetto ", m("span", "CI"))),
    m(
      "nav",
      m(`div${active("cls")}`, m("a[href=/#!/cls]", "CLs")),
      m(`div${active("jobs")}`, m("a[href=/#!/jobs]", "Jobs")),
      m(
        `div${active("stats")}`,
        m(`a[href=${STATS_LINK}][target=_blank]`, "Stats"),
      ),
      m(`div`, m(`a[href=${docsUrl}][target=_blank]`, "Docs")),
      m(
        `div.logs`,
        "Logs",
        m("div", m(`a[href=${logUrl}workers][target=_blank]`, "Workers")),
        m("div", m(`a[href=${logUrl}frontend][target=_blank]`, "Frontend")),
      ),
    ),
  );
}

var CLsPageRenderer = {
  view: function (vnode) {
    const allCols = 4 + JOB_TYPES.length;
    const postsubmitHeader = m(
      "tr",
      m(`td.header[colspan=${allCols}]`, "Post-submit"),
    );

    const postsubmitLoadMore = m(
      "tr",
      m(
        `td[colspan=${allCols}]`,
        m(
          "a[href=#]",
          {
            onclick: () => {
              state.limitPostsubmit += 10;
              getMainCommits();
            },
          },
          "Load more",
        ),
      ),
    );

    const presubmitHeader = m(
      "tr",
      m(`td.header[colspan=${allCols}]`, "Pre-submit"),
    );

    let branchRows = [];
    for (
      let i = 0;
      i < Math.min(state.mainCommits.length, state.limitPostsubmit);
      i++
    ) {
      const commit = state.mainCommits[i];
      branchRows = branchRows.concat(renderCLRow(commit));
    }

    let clRows = [];
    for (const gerritCl of state.pullReqs) {
      if (vnode.attrs.cl && gerritCl.num != vnode.attrs.cl) continue;
      clRows = clRows.concat(renderCLRow(gerritCl));
    }

    let footer = [];
    if (vnode.attrs.cl) {
      footer = m(
        "footer",
        `Showing only CL ${vnode.attrs.cl} - `,
        m(`a[href=#!/cls]`, "Click here to see all CLs"),
      );
    }

    return [
      renderHeader(),
      m(
        "main#cls",
        m(
          "table.main-table",
          m(
            "thead",
            m(
              "tr",
              m("td[rowspan=4]", "Subject"),
              m("td[rowspan=4]", "Status"),
              m("td[rowspan=4]", "Owner"),
              m("td[rowspan=4]", "Updated"),
              m("td[colspan=11]", "Bots"),
            ),
            m(
              "tr",
              m("td[colspan=9]", "linux"),
              m("td[colspan=1]", "android"),
              m("td", "RC"),
            ),
            m(
              "tr",
              m("td", "gcc8"),
              m("td[colspan=7]", "clang"),
              m("td[colspan=1]", "ui"),
              m("td[colspan=1]", "clang-arm"),
              m("td[colspan=1]", "RC"),
            ),
            m(
              "tr#cls_header",
              JOB_TYPES.map((job) => m(`td#${job.id}`, job.label)),
            ),
          ),
          m(
            "tbody",
            postsubmitHeader,
            branchRows,
            postsubmitLoadMore,
            presubmitHeader,
            clRows,
          ),
        ),
        footer,
      ),
    ];
  },
};

function getLastUpdate(lastUpdate) {
  if (lastUpdate === undefined) return "";
  const lastUpdateMins = Math.ceil((Date.now() - lastUpdate) / 60000);
  if (lastUpdateMins < 60) return lastUpdateMins + " mins ago";
  if (lastUpdateMins < 60 * 24)
    return Math.ceil(lastUpdateMins / 60) + " hours ago";
  return lastUpdate.toISOString().substr(0, 10);
}

function renderCLRow(cl) {
  const expanded = !!state.expandCl[cl.id];
  const toggleExpand = () => {
    state.expandCl[cl.id] ^= 1;
    if (state.expandCl[cl.id]) {
      fetchAllPatchsetsForPr(cl);
    }
  };
  const rows = [];

  // Create the row for the latest patchset (as fetched by Gerrit).
  rows.push(
    m(
      `tr.${cl.status}`,
      m(
        "td",
        m(
          `i.material-icons.expand${expanded ? ".expanded" : ""}`,
          { onclick: toggleExpand },
          "arrow_right",
        ),
        m(
          `a[href=${cl.url}]`,
          `${cl.subject}`,
          cl.num && m("span.ps", `#${cl.num}`),
        ),
      ),
      m("td", cl.status),
      m("td", cl.owner),
      m("td", getLastUpdate(cl.lastUpdate)),
      JOB_TYPES.map((x) => renderClJobCell(cl.id, x.id)),
    ),
  );

  // If the usere clicked on the expand button, show also the other patchsets.
  if (state.expandCl[cl.id]) {
    for (const ps of state.patchsets[cl.id] ?? []) {
      rows.push(
        m(
          `tr.nested`,
          m("td", m(`a[href=${ps.url}]`, ps.title)),
          m("td", ""),
          m("td", ""),
          m("td", ""),
          JOB_TYPES.map((x) => renderClJobCell(ps.id, x.id)),
        ),
      );
    }
  }
  return rows;
}

function renderJobLink(jobStatus, url) {
  const ICON_MAP = {
    queued: "schedule",
    success: "check_circle",
    failure: "bug_report",
    skipped: "clear",
    in_progress: "hourglass_full",
    cancelled: "cancel",
    timed_out: "notification_important",
  };
  const icon = ICON_MAP[jobStatus] || "clear";
  return m(
    `a.${jobStatus}[href=${url}][title=${jobStatus}][target=_blank]`,
    m(`i.material-icons`, icon),
  );
}

function renderClJobCell(id, botName) {
  const check = (state.checks[id] ?? {})[botName];
  if (check === undefined) {
    return m("td.job", renderJobLink("unknown", ""));
  } else {
    return m("td.job", renderJobLink(check.status, check.url));
  }
}

const JobsPageRenderer = {
  oncreate: function (vnode) {
    fetchWorkers();
    fetchWorkflows();
  },

  createWorkerTable: function () {
    const makeWokerRow = (runner) => {
      return m(
        "tr",
        { className: runner.status },
        m("td", runner.id),
        m("td", runner.name),
        m("td", runner.status),
      );
    };

    return m(
      "table.main-table",
      m(
        "thead",
        m("tr", m("td[colspan=3]", "Workers")),
        m("tr", m("td", "ID"), m("td", "Worker"), m("td", "Status")),
      ),
      m("tbody", state.runners.map(makeWokerRow)),
    );
  },

  createJobsTable: function () {
    const makeJobRow = (job) => {
      return m(
        "tr",
        { class: `workflow ${job.status}` },
        m("td", { colspan: 2 }),
        m("td", m("a", { href: job.html_url }, job.id)),
        m("td", job.status, job.conclusion && ` [${job.conclusion}]`),
        m("td", `${job.runner_id} (${job.runner_name})`),
        m("td", job.name),
        m("td", getLastUpdate(parseGhTime(job.created_at))),
        m("td", getLastUpdate(parseGhTime(job.updated_at))),
        m("td", getLastUpdate(parseGhTime(job.completed_at))),
      );
    };

    const makeWorkflowRow = (wkf) => {
      return [
        m(
          "tr",
          { class: `workflow ${wkf.status}` },
          m("td", m("a", { href: wkf.html_url }, wkf.id)),
          m("td", wkf.event),
          m("td", ""),
          m("td", wkf.status, wkf.conclusion && ` [${wkf.conclusion}]`),
          m("td", wkf.actor.login),
          m("td", wkf.display_title),
          m("td", getLastUpdate(parseGhTime(wkf.created_at))),
          m("td", getLastUpdate(parseGhTime(wkf.updated_at))),
          m("td", ""),
        ),
      ].concat(wkf.jobs.map(makeJobRow));
    };

    return m(
      "table.main-table",
      m(
        "thead",
        m("tr", m("td[colspan=9]", "Workflow runs")),
        m(
          "tr",
          m("td", "WKF ID"),
          m("td", "Trigger"),
          m("td", "Job ID"),
          m("td", "Status"),
          m("td", "Author"),
          m("td", "Title"),
          m("td", "Created"),
          m("td", "Updated"),
          m("td", "Completed"),
        ),
      ),
      m(
        "tbody",
        state.workflowRuns.sort((a, b) => b.id - a.id).map(makeWorkflowRow),
      ),
    );
  },

  view: function (vnode) {
    return [
      renderHeader(),
      m(
        "main",
        m(".jobs-list", this.createWorkerTable(), this.createJobsTable()),
      ),
    ];
  },
};

// -----------------------------------------------------------------------------
// Business logic (handles fetching from GitHub).
// -----------------------------------------------------------------------------

function parseGhTime(str) {
  if (str === null || str === undefined) return undefined;
  // Gerrit timestamps are UTC (as per public docs) but obviously they are not
  // encoded in ISO format.
  return new Date(str);
}

async function getMainCommits() {
  console.log("Fetching commits from GitHub");
  const uri = "/gh/commits/main";
  const response = await fetch(uri);
  const commits = JSON.parse(await response.text());
  state.mainCommits = [];
  for (let i = 0; i < Math.min(commits.length, state.limitPostsubmit); i++) {
    const c = commits[i];
    const id = `main/${c.sha}`;
    const pr = {
      id: id,
      url: c.html_url,
      subject: c.commit.message.split("\n")[0],
      revHash: c.sha,
      lastUpdate: parseGhTime(c.commit.committer.date),
      owner: c.author.login,
    };
    state.mainCommits.push(pr);
    fetchChecksForPR(pr.id, pr.revHash);
  }
  console.log(`Got ${state.pullReqs.length} commits`);
  scheduleRedraw();
}

// Fetches the list of CLs from gerrit and updates the state.
async function fetchPullRequests() {
  console.log("Fetching PRs from GitHub");
  const uri = "/gh/pulls";
  const response = await fetch(uri);
  state.pullReqs = [];
  if (response.status !== 200) {
    setTimeout(fetchPullRequests, 3000); // Retry.
    return;
  }

  const pulls = JSON.parse(await response.text());
  state.pullReqs = [];
  for (const p of pulls) {
    const id = `${p.number}/${p.head.sha}`;
    const pr = {
      id: id,
      url: p.html_url,
      subject: p.title,
      status: p.state,
      num: p.number,
      revHash: p.head.sha,
      lastUpdate: parseGhTime(p.updated_at),
      owner: p.user.login,
    };
    state.pullReqs.push(pr);
    fetchChecksForPR(pr.id, pr.revHash);
  }
  console.log(`Got ${state.pullReqs.length} PRs`);
  scheduleRedraw();
}

async function fetchChecksForPR(id, commitHash) {
  state.checks[id] ??= {};
  const prChecks = state.checks[id];
  const response = await fetch(`/gh/checks/${commitHash}`);
  const json = JSON.parse(await response.text());
  for (const check of json.check_runs) {
    // status is either: 'queued', 'in_progress', 'success', 'failure', or other
    // values we don't really bother supporting.
    let status = check.status;
    if (status === "completed") {
      status = check.conclusion;
    }

    // Extract the job ID from the long concatenated Github Actions string.
    // The input can be either
    // linux / linux (gcc8-x86_64-release, is_debug=false  (when using matrix)
    // or just
    // bazel / bazel
    // We want in output: 'linux/gcc8-x86_64-release' or 'bazel'.
    const m = check.name.match(
      /^([\w-]+)\s*\/\s*[\w-]+(?:\s*\(\s*([^,\s)]+))?/,
    );
    if (!m) continue;
    const name = m[1] + (m[2] ? `/${m[2]}` : "");
    prChecks[name] = {
      status: status,
      url: check.details_url,
    };
  }
  scheduleRedraw();
}

async function fetchAllPatchsetsForPr(pr) {
  const patchsets = (state.patchsets[pr.id] = []);
  const response = await fetch(`/gh/patchsets/${pr.num}`);
  const json = JSON.parse(await response.text());
  for (const commit of json) {
    const psId = `${pr.num}/${commit.sha}`;
    patchsets.push({
      id: psId,
      url: `${pr.url}/commits/${commit.sha}`,
      sha: commit.sha,
      title: commit.commit.message.split("\n")[0],
    });
    fetchChecksForPR(psId, commit.sha);
  }
  scheduleRedraw();
}

async function fetchWorkers() {
  const uri = "/gh/runners";
  const response = await fetch(uri);
  state.runners = JSON.parse(await response.text()).runners;
  scheduleRedraw();
}

async function fetchWorkflows() {
  const uri = "/gh/workflows";
  const response = await fetch(uri);
  const resp = await response.text();
  state.workflowRuns = JSON.parse(resp);
  scheduleRedraw();
}

function scheduleRedraw() {
  if (state.redrawPending) return;
  state.redrawPending = true;
  window.requestAnimationFrame(() => {
    state.redrawPending = false;
    m.redraw();
  });
}

main();

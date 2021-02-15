// Copyright (C) 2021 The Android Open Source Project
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

'use strict';

// This script builds the perfetto.dev docs website.

const argparse = require('argparse');
const child_process = require('child_process');
const fs = require('fs');
const http = require('http');
const path = require('path');
const fswatch = require('node-watch');  // Like fs.watch(), but works on Linux.
const pjoin = path.join;

const ROOT_DIR = path.dirname(path.dirname(__dirname));  // The repo root.

const cfg = {
  watch: false,
  verbose: false,
  startHttpServer: false,

  outDir: pjoin(ROOT_DIR, 'out/perfetto.dev'),
};

const RULES = [
  {r: /infra\/perfetto.dev\/src\/assets\/((.*)\.png)/, f: copyAssets},
  {r: /infra\/perfetto.dev\/src\/assets\/((.*)\.js)/, f: copyAssets},
  {r: /infra\/perfetto.dev\/node_modules\/.*\/(.*\.css|.*\.js)/, f: copyAssets},
  {r: /infra\/perfetto.dev\/src\/assets\/.+\.scss/, f: compileScss},
  {
    r: /protos\/perfetto\/config\/trace_config\.proto/,
    f: s => genProtoReference(s, 'perfetto.protos.TraceConfig')
  },
  {
    r: /protos\/perfetto\/trace\/trace_packet\.proto/,
    f: s => genProtoReference(s, 'perfetto.protos.TracePacket')
  },
  {r: /src\/trace_processor\/storage\/stats\.h/, f: genSqlStatsReference},
  {r: /src\/trace_processor\/tables\/.*\.h/, f: s => sqlTables.add(s)},
  {r: /docs\/toc[.]md/, f: genNav},
  {r: /docs\/.*[.]md/, f: renderDoc},
];

let sqlTables = new Set();
let tasks = [];
let tasksTot = 0, tasksRan = 0;
let tStart = Date.now();

function main() {
  const parser = new argparse.ArgumentParser();
  parser.add_argument('--out', {help: 'Output directory'});
  parser.add_argument('--watch', '-w', {action: 'store_true'});
  parser.add_argument('--serve', '-s', {action: 'store_true'});
  parser.add_argument('--verbose', '-v', {action: 'store_true'});

  const args = parser.parse_args();
  cfg.outDir = path.resolve(ensureDir(args.out || cfg.outDir, /*clean=*/ true));
  cfg.watch = !!args.watch;
  cfg.verbose = !!args.verbose;
  cfg.startHttpServer = args.serve;

  // Check that deps are current before starting.
  const installBuildDeps = pjoin(ROOT_DIR, 'tools/install-build-deps');
  const depsArgs = ['--check-only', '/dev/null', '--ui'];
  exec(installBuildDeps, depsArgs);

  console.log('Entering', cfg.outDir);
  process.chdir(cfg.outDir);

  scanDir('infra/perfetto.dev/src/assets');
  scanFile(
      'infra/perfetto.dev/node_modules/highlight.js/styles/tomorrow-night.css');
  scanFile('infra/perfetto.dev/node_modules/mermaid/dist/mermaid.min.js');
  scanFile('docs/toc.md');
  genIndex();
  scanFile('src/trace_processor/storage/stats.h');
  scanDir('src/trace_processor/tables');
  scanDir('protos');
  genSqlTableReference();
  scanDir('docs');
  if (args.serve) {
    addTask(startServer);
  }
}

// -----------
// Build rules
// -----------

function copyAssets(src, dst) {
  addTask(cp, [src, pjoin(cfg.outDir, 'assets', dst)]);
}

function compileScss() {
  const src = pjoin(__dirname, 'src/assets/style.scss');
  const dst = pjoin(cfg.outDir, 'assets/style.css');
  // In watch mode, don't exit(1) if scss fails. It can easily happen by
  // having a typo in the css. It will still print an errror.
  const noErrCheck = !!cfg.watch;
  addTask(
      execNode,
      ['node_modules/.bin/node-sass', ['--quiet', src, dst], {noErrCheck}]);
}

function md2html(src, dst, template) {
  const script = pjoin(__dirname, 'src/markdown_render.js');
  const args = ['-i', src, '--odir', cfg.outDir, '-o', dst];
  ensureDir(path.dirname(dst));
  if (template) args.push('-t', pjoin(__dirname, 'src', template));
  execNode(script, args);
}

function proto2md(src, dst, protoRootType) {
  const script = pjoin(__dirname, 'src/gen_proto_reference.js');
  const args = ['-i', src, '-p', protoRootType, '-o', dst];
  ensureDir(path.dirname(dst));
  execNode(script, args);
}

function genNav(src) {
  const dst = pjoin(cfg.outDir, 'docs', '_nav.html');
  addTask(md2html, [src, dst]);
}

function genIndex() {
  const dst = pjoin(cfg.outDir, 'index.html');
  addTask(md2html, ['/dev/null', dst, 'template_index.html']);
}

function renderDoc(src) {
  let dstRel = path.relative(ROOT_DIR, src);
  dstRel = dstRel.replace('.md', '').replace(/\bREADME$/, 'index.html');
  const dst = pjoin(cfg.outDir, dstRel);
  addTask(md2html, [src, dst, 'template_markdown.html']);
}

function genProtoReference(src, protoRootType) {
  const fname = path.basename(src);
  const dstFname = fname.replace(/[._]/g, '-');
  const dstHtml = pjoin(cfg.outDir, 'docs/reference', dstFname);
  const dstMd = dstHtml + '.md';
  addTask(proto2md, [src, dstMd, protoRootType]);
  addTask(md2html, [dstMd, dstHtml, 'template_markdown.html']);
  addTask(exec, ['rm', [dstMd]]);
}

function genSqlStatsReference(src) {
  const dstDir = ensureDir(pjoin(cfg.outDir, 'docs/analysis'));
  const dstHtml = pjoin(dstDir, 'sql-stats');
  const dstMd = dstHtml + '.md';
  const script = pjoin(__dirname, 'src/gen_stats_reference.js');
  const args = ['-i', src, '-o', dstMd];
  addTask(execNode, [script, args]);
  addTask(md2html, [dstMd, dstHtml, 'template_markdown.html']);
  addTask(exec, ['rm', [dstMd]]);
}

function genSqlTableReference() {
  const dstDir = ensureDir(pjoin(cfg.outDir, 'docs/analysis'));
  const dstHtml = pjoin(dstDir, 'sql-tables');
  const dstMd = dstHtml + '.md';
  const script = pjoin(__dirname, 'src/gen_sql_tables_reference.js');
  const args = ['-o', dstMd];
  sqlTables.forEach(f => args.push('-i', f));
  addTask(execNode, [script, args]);
  addTask(md2html, [dstMd, dstHtml, 'template_markdown.html']);
  addTask(exec, ['rm', [dstMd]]);
}

function startServer() {
  const port = 8082;
  console.log(`Starting HTTP server on http://localhost:${port}`)
  http.createServer(function(req, res) {
        console.debug(req.method, req.url);
        let uri = req.url.split('?', 1)[0];
        uri += uri.endsWith('/') ? 'index.html' : '';

        const absPath = path.normalize(path.join(cfg.outDir, uri));
        fs.readFile(absPath, function(err, data) {
          if (err) {
            res.writeHead(404);
            res.end(JSON.stringify(err));
            return;
          }
          const mimeMap = {
            'css': 'text/css',
            'svg': 'image/svg+xml',
            'js': 'application/javascript',
          };
          const contentType = mimeMap[uri.split('.').pop()] || 'text/html';
          const head = {
            'Content-Type': contentType,
            'Content-Length': data.length,
            'Cache-Control': 'no-cache',
          };
          res.writeHead(200, head);
          res.end(data);
        });
      })
      .listen(port);
}


// -----------------------
// Task chaining functions
// -----------------------

function addTask(func, args) {
  const task = new Task(func, args);
  for (const t of tasks) {
    if (t.identity === task.identity) {
      return;
    }
  }
  tasks.push(task);
  setTimeout(runTasks, 0);
}

function runTasks() {
  const snapTasks = tasks.splice(0);  // snap = std::move(tasks).
  tasksTot += snapTasks.length;
  for (const task of snapTasks) {
    const DIM = '\u001b[2m';
    const BRT = '\u001b[37m';
    const RST = '\u001b[0m';
    const ms = (new Date(Date.now() - tStart)).toISOString().slice(17, -1);
    const ts = `[${DIM}${ms}${RST}]`;
    const descr = task.description.substr(0, 80);
    console.log(`${ts} ${BRT}${++tasksRan}/${tasksTot}${RST}\t${descr}`);
    task.func.apply(/*this=*/ undefined, task.args);
  }
}

// Executes the first rule in RULES that match the given |absPath|.
function scanFile(file) {
  const absPath = path.isAbsolute(file) ? file : pjoin(ROOT_DIR, file);
  console.assert(fs.existsSync(absPath));
  const normPath = path.relative(ROOT_DIR, absPath);
  for (const rule of RULES) {
    const match = rule.r.exec(normPath);
    if (!match || match[0] !== normPath) continue;
    const captureGroup = match.length > 1 ? match[1] : undefined;
    rule.f(absPath, captureGroup);
    return;
  }
}

// Walks the passed |dir| recursively and, for each file, invokes the matching
// RULES. If --watch is used, it also installs a fswatch() and re-triggers the
// matching RULES on each file change.
function scanDir(dir, regex) {
  const filterFn = regex ? absPath => regex.test(absPath) : () => true;
  const absDir = path.isAbsolute(dir) ? dir : pjoin(ROOT_DIR, dir);
  // Add a fs watch if in watch mode.
  if (cfg.watch) {
    fswatch(absDir, {recursive: true}, (_eventType, filePath) => {
      if (!filterFn(filePath)) return;
      if (cfg.verbose) {
        console.log('File change detected', _eventType, filePath);
      }
      if (fs.existsSync(filePath)) {
        scanFile(filePath, filterFn);
      }
    });
  }
  walk(absDir, f => {
    if (filterFn(f)) scanFile(f);
  });
}

function exec(cmd, args, opts) {
  opts = opts || {};
  opts.stdout = opts.stdout || 'inherit';
  if (cfg.verbose) console.log(`${cmd} ${args.join(' ')}\n`);
  const spwOpts = {cwd: cfg.outDir, stdio: ['ignore', opts.stdout, 'inherit']};
  const checkExitCode = (code, signal) => {
    if (signal === 'SIGINT' || signal === 'SIGTERM') return;
    if (code !== 0 && !opts.noErrCheck) {
      console.error(`${cmd} ${args.join(' ')} failed with code ${code}`);
      process.exit(1);
    }
  };
  const spawnRes = child_process.spawnSync(cmd, args, spwOpts);
  checkExitCode(spawnRes.status, spawnRes.signal);
  return spawnRes;
}

function execNode(script, args, opts) {
  const modPath = path.isAbsolute(script) ? script : pjoin(__dirname, script);
  const nodeBin = pjoin(ROOT_DIR, 'tools/node');
  args = [modPath].concat(args || []);
  const argsJson = JSON.stringify(args);
  return exec(nodeBin, args, opts);
}

// ------------------------------------------
// File system & subprocess utility functions
// ------------------------------------------

class Task {
  constructor(func, args) {
    this.func = func;
    this.args = args || [];
    // |identity| is used to dedupe identical tasks in the queue.
    this.identity = JSON.stringify([this.func.name, this.args]);
  }

  get description() {
    const ret = this.func.name.startsWith('exec') ? [] : [this.func.name];
    const flattenedArgs = [].concat.apply([], this.args);
    for (const arg of flattenedArgs) {
      const argStr = `${arg}`;
      if (argStr.startsWith('/')) {
        ret.push(path.relative(cfg.outDir, arg));
      } else {
        ret.push(argStr);
      }
    }
    return ret.join(' ');
  }
}

function walk(dir, callback, skipRegex) {
  for (const child of fs.readdirSync(dir)) {
    const childPath = pjoin(dir, child);
    const stat = fs.lstatSync(childPath);
    if (skipRegex !== undefined && skipRegex.test(child)) continue;
    if (stat.isDirectory()) {
      walk(childPath, callback, skipRegex);
    } else if (!stat.isSymbolicLink()) {
      callback(childPath);
    }
  }
}

function ensureDir(dirPath, clean) {
  const exists = fs.existsSync(dirPath);
  if (exists && clean) {
    if (cfg.verbose) console.log('rm', dirPath);
    fs.rmSync(dirPath, {recursive: true});
  }
  if (!exists || clean) fs.mkdirSync(dirPath, {recursive: true});
  return dirPath;
}

function cp(src, dst) {
  ensureDir(path.dirname(dst));
  if (cfg.verbose) {
    console.log(
        'cp', path.relative(ROOT_DIR, src), '->', path.relative(ROOT_DIR, dst));
  }
  fs.copyFileSync(src, dst);
}

main();

// Copyright (C) 2026 The Android Open Source Project
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

// Step framing helpers. Each "step" prints:
//   ┌─ label
//   │  <indented stdout/stderr>
//   └─ ✓ label (Xs)   (or ✗ on failure)
//
// Two flavours:
//   runStep(label, cmd, args, opts)    — spawns a subprocess
//   runInProcStep(label, fn)           — wraps an async fn; patches
//                                        process.stdout/stderr.write so any
//                                        output gets the same │ prefix.

import {spawn} from 'node:child_process';

// ANSI helpers. Colours degrade to plain text on non-TTY.
const tty = () => process.stdout.isTTY;
const c = (code, s) => (tty() ? `\x1b[${code}m${s}\x1b[0m` : s);

// makeLineStream returns a writer that buffers partial lines and emits each
// completed line through |out| with the │ prefix.
function makeLineStream(out) {
  let buf = '';
  const PREFIX = c(90, '│ '); // dim grey
  return {
    write(chunk) {
      buf += chunk.toString('utf8');
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        out.write(PREFIX + buf.slice(0, nl) + '\n');
        buf = buf.slice(nl + 1);
      }
    },
    flush() {
      if (buf.length) {
        out.write(PREFIX + buf + '\n');
        buf = '';
      }
    },
  };
}

function stepHead(label) {
  process.stdout.write(c(36, '┌─ ') + label + '\n');
}

function stepTail(label, t0, ok, extra = '') {
  const secs = ((performance.now() - t0) / 1000).toFixed(2);
  const mark = ok ? c(32, '✓') : c(31, '✗');
  const tail = c(36, '└─ ') + `${mark} ${label}${extra} (${secs}s)\n`;
  (ok ? process.stdout : process.stderr).write(tail);
}

// Runs |cmd args| with framed, indented streaming of stdout/stderr. Exits the
// process on failure.
export async function runStep(label, cmd, args, opts = {}) {
  stepHead(label);
  const t0 = performance.now();
  const proc = spawn(cmd, args, {
    stdio: ['ignore', 'pipe', 'pipe'],
    ...opts,
  });
  const outLs = makeLineStream(process.stdout);
  const errLs = makeLineStream(process.stderr);
  proc.stdout.on('data', (b) => outLs.write(b));
  proc.stderr.on('data', (b) => errLs.write(b));
  const status = await new Promise((resolve) => proc.on('exit', resolve));
  outLs.flush();
  errLs.flush();
  if (status === 0) {
    stepTail(label, t0, true);
    return;
  }
  stepTail(label, t0, false, ` (exit ${status})`);
  process.exit(status ?? 1);
}

// Like runStep but for in-process work. Patches process.stdout.write and
// process.stderr.write while |fn| runs so output gets the same │ prefix.
// Bubbles thrown errors up after printing a failure footer.
export async function runInProcStep(label, fn) {
  stepHead(label);
  const t0 = performance.now();
  const realOut = process.stdout.write.bind(process.stdout);
  const realErr = process.stderr.write.bind(process.stderr);
  const outLs = makeLineStream({write: realOut});
  const errLs = makeLineStream({write: realErr});
  process.stdout.write = (chunk, ...rest) => {
    outLs.write(chunk);
    const cb = rest[rest.length - 1];
    if (typeof cb === 'function') cb();
    return true;
  };
  process.stderr.write = (chunk, ...rest) => {
    errLs.write(chunk);
    const cb = rest[rest.length - 1];
    if (typeof cb === 'function') cb();
    return true;
  };
  try {
    await fn();
  } catch (err) {
    outLs.flush();
    errLs.flush();
    process.stdout.write = realOut;
    process.stderr.write = realErr;
    stepTail(label, t0, false);
    throw err;
  }
  outLs.flush();
  errLs.flush();
  process.stdout.write = realOut;
  process.stderr.write = realErr;
  stepTail(label, t0, true);
}

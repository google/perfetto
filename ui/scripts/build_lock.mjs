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

// Cooperative build lock. The risk this guards against: a long-running `dev`
// or `preview` is serving files out of outDir while a second process runs
// `build`/`pre` and wipes outDir mid-flight — the live server starts 404'ing.
//
// The lock file lives one level above outDir so it survives prebuild's
// `rm -rf outDir`. Anyone who needs outDir to stay stable (prebuild, dev,
// preview, test) acquires it for their whole lifetime. --no-build runs only
// peek at the lock and print a warning.

import fs from 'node:fs';
import path from 'node:path';

function lockFilePath(outDir) {
  return path.join(path.dirname(outDir), '.build.lock');
}

// Reads the lock file and returns the PID of the holder if it's still alive,
// or null if the file is missing / the PID is dead.
function readLiveHolder(lockFile) {
  if (!fs.existsSync(lockFile)) return null;
  const pid = parseInt(fs.readFileSync(lockFile, 'utf8').trim(), 10);
  if (!pid) return null;
  try {
    process.kill(pid, 0); // signal 0 = liveness check only.
    return pid;
  } catch {
    return null; // stale.
  }
}

// Acquires the lock or exits with a clear message. The lock is released on
// process exit via an exit handler.
export function acquireBuildLock({outDir}) {
  const lockFile = lockFilePath(outDir);
  const holder = readLiveHolder(lockFile);
  if (holder !== null && holder !== process.pid) {
    console.error(
      `Error: another ui build process (PID ${holder}) is using ${outDir}.`,
    );
    console.error(
      'Hint: stop it first, or pass --no-build to skip the build step.',
    );
    process.exit(1);
  }
  if (holder === null && fs.existsSync(lockFile)) {
    // Stale.
    fs.unlinkSync(lockFile);
  }
  fs.mkdirSync(path.dirname(lockFile), {recursive: true});
  fs.writeFileSync(lockFile, String(process.pid));
  const release = () => {
    try {
      const pid = parseInt(fs.readFileSync(lockFile, 'utf8').trim(), 10);
      if (pid === process.pid) fs.unlinkSync(lockFile);
    } catch {
      // Already gone — fine.
    }
  };
  process.on('exit', release);
  for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
    process.once(sig, () => {
      release();
      process.kill(process.pid, sig);
    });
  }
}

// Read-only check. Prints a warning if the lock is held by someone else.
// Used by --no-build callers that read outDir without modifying it.
export function warnIfBuildLockHeld({outDir}) {
  const holder = readLiveHolder(lockFilePath(outDir));
  if (holder !== null && holder !== process.pid) {
    console.warn(
      `Warning: another ui build process (PID ${holder}) is using ${outDir}.`,
    );
    console.warn(
      "         Its output may change underneath us; --no-build doesn't take the lock.",
    );
  }
}

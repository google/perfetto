#!/usr/bin/env python3
# Copyright (C) 2021 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import atexit
import argparse
import datetime
import hashlib
import http.server
import os
import re
import shutil
import socketserver
import subprocess
import sys
import time
import webbrowser

from perfetto.prebuilts.manifests.tracebox import *
from perfetto.prebuilts.perfetto_prebuilts import *
from perfetto.common.repo_utils import *

# This is not required. It's only used as a fallback if no adb is found on the
# PATH. It's fine if it doesn't exist so this script can be copied elsewhere.
HERMETIC_ADB_PATH = repo_dir('buildtools/android_sdk/platform-tools/adb')

# Translates the Android ro.product.cpu.abi into the GN's target_cpu.
ABI_TO_ARCH = {
    'armeabi-v7a': 'arm',
    'arm64-v8a': 'arm64',
    'x86': 'x86',
    'x86_64': 'x64',
}

MAX_ADB_FAILURES = 15  # 2 seconds between retries, 30 seconds total.

devnull = open(os.devnull, 'rb')
adb_path = None
procs = []


class ANSI:
  END = '\033[0m'
  BOLD = '\033[1m'
  RED = '\033[91m'
  BLACK = '\033[30m'
  BLUE = '\033[94m'
  BG_YELLOW = '\033[43m'
  BG_BLUE = '\033[44m'


# HTTP Server used to open the trace in the browser.
class HttpHandler(http.server.SimpleHTTPRequestHandler):

  def end_headers(self):
    self.send_header('Access-Control-Allow-Origin', '*')
    return super().end_headers()

  def do_GET(self):
    self.server.last_request = self.path
    return super().do_GET()

  def do_POST(self):
    self.send_error(404, "File not found")


def main():
  atexit.register(kill_all_subprocs_on_exit)
  default_out_dir_str = '~/traces/'
  default_out_dir = os.path.expanduser(default_out_dir_str)

  examples = '\n'.join([
      ANSI.BOLD + 'Examples' + ANSI.END, '  -t 10s -b 32mb sched gfx wm -a*',
      '  -t 5s sched/sched_switch raw_syscalls/sys_enter raw_syscalls/sys_exit',
      '  -c /path/to/full-textual-trace.config', '',
      ANSI.BOLD + 'Long traces' + ANSI.END,
      'If you want to record a hours long trace and stream it into a file ',
      'you need to pass a full trace config and set write_into_file = true.',
      'See https://perfetto.dev/docs/concepts/config#long-traces .'
  ])
  parser = argparse.ArgumentParser(
      epilog=examples, formatter_class=argparse.RawTextHelpFormatter)

  help = 'Output file or directory (default: %s)' % default_out_dir_str
  parser.add_argument('-o', '--out', default=default_out_dir, help=help)

  help = 'Don\'t open in the browser'
  parser.add_argument('-n', '--no-open', action='store_true', help=help)

  help = 'Force the use of the sideloaded binaries rather than system daemons'
  parser.add_argument('--sideload', action='store_true', help=help)

  help = ('Sideload the given binary rather than downloading it. ' +
          'Implies --sideload')
  parser.add_argument('--sideload-path', default=None, help=help)

  help = 'Don\'t run `adb root` run as user (only when sideloading)'
  parser.add_argument('-u', '--user', action='store_true', help=help)

  help = 'Specify the ADB device serial'
  parser.add_argument('--serial', '-s', default=None, help=help)

  grp = parser.add_argument_group(
      'Short options: (only when not using -c/--config)')

  help = 'Trace duration N[s,m,h] (default: trace until stopped)'
  grp.add_argument('-t', '--time', default='0s', help=help)

  help = 'Ring buffer size N[mb,gb] (default: 32mb)'
  grp.add_argument('-b', '--buffer', default='32mb', help=help)

  help = ('Android (atrace) app names. Can be specified multiple times.\n-a*' +
          'for all apps (without space between a and * or bash will expand it)')
  grp.add_argument(
      '-a',
      '--app',
      metavar='com.myapp',
      action='append',
      default=[],
      help=help)

  help = 'sched, gfx, am, wm (see --list)'
  grp.add_argument('events', metavar='Atrace events', nargs='*', help=help)

  help = 'sched/sched_switch kmem/kmem (see --list-ftrace)'
  grp.add_argument('_', metavar='Ftrace events', nargs='*', help=help)

  help = 'Lists all the categories available'
  grp.add_argument('--list', action='store_true', help=help)

  help = 'Lists all the ftrace events available'
  grp.add_argument('--list-ftrace', action='store_true', help=help)

  section = ('Full trace config (only when not using short options)')
  grp = parser.add_argument_group(section)

  help = 'Can be generated with https://ui.perfetto.dev/#!/record'
  grp.add_argument('-c', '--config', default=None, help=help)

  args = parser.parse_args()
  args.sideload = args.sideload or args.sideload_path is not None

  if args.serial:
    os.environ["ANDROID_SERIAL"] = args.serial

  find_adb()

  if args.list:
    adb('shell', 'atrace', '--list_categories').wait()
    sys.exit(0)

  if args.list_ftrace:
    adb('shell', 'cat /d/tracing/available_events | tr : /').wait()
    sys.exit(0)

  if args.config is not None and not os.path.exists(args.config):
    prt('Config file not found: %s' % args.config, ANSI.RED)
    sys.exit(1)

  if len(args.events) == 0 and args.config is None:
    prt('Must either pass short options (e.g. -t 10s sched) or a --config file',
        ANSI.RED)
    parser.print_help()
    sys.exit(1)

  if args.config is None and args.events and os.path.exists(args.events[0]):
    prt(('The passed event name "%s" is a local file. ' % args.events[0] +
         'Did you mean to pass -c / --config ?'), ANSI.RED)
    sys.exit(1)

  perfetto_cmd = 'perfetto'
  device_dir = '/data/misc/perfetto-traces/'

  # Check the version of android. If too old (< Q) sideload tracebox. Also use
  # use /data/local/tmp as /data/misc/perfetto-traces was introduced only later.
  probe_cmd = 'getprop ro.build.version.sdk; getprop ro.product.cpu.abi; whoami'
  probe = adb('shell', probe_cmd, stdout=subprocess.PIPE)
  lines = probe.communicate()[0].decode().strip().split('\n')
  lines = [x.strip() for x in lines]  # To strip \r(s) on Windows.
  if probe.returncode != 0:
    prt('ADB connection failed', ANSI.RED)
    sys.exit(1)
  api_level = int(lines[0])
  abi = lines[1]
  arch = ABI_TO_ARCH.get(abi)
  if arch is None:
    prt('Unsupported ABI: ' + abi)
    sys.exit(1)
  shell_user = lines[2]
  if api_level < 29 or args.sideload:  # 29: Android Q.
    tracebox_bin = args.sideload_path
    if tracebox_bin is None:
      tracebox_bin = get_perfetto_prebuilt(
          TRACEBOX_MANIFEST, arch='android-' + arch)
    perfetto_cmd = '/data/local/tmp/tracebox'
    exit_code = adb('push', '--sync', tracebox_bin, perfetto_cmd).wait()
    exit_code |= adb('shell', 'chmod 755 ' + perfetto_cmd).wait()
    if exit_code != 0:
      prt('ADB push failed', ANSI.RED)
      sys.exit(1)
    device_dir = '/data/local/tmp/'
    if shell_user != 'root' and not args.user:
      # Run as root if possible as that will give access to more tracing
      # capabilities. Non-root still works, but some ftrace events might not be
      # available.
      adb('root').wait()

  tstamp = datetime.datetime.now().strftime('%Y-%m-%d_%H-%M')
  fname = '%s-%s.pftrace' % (tstamp, os.urandom(3).hex())
  device_file = device_dir + fname

  cmd = [perfetto_cmd, '--background', '--txt', '-o', device_file]
  on_device_config = None
  on_host_config = None
  if args.config is not None:
    cmd += ['-c', '-']
    if api_level < 24:
      # adb shell does not redirect stdin. Push the config on a temporary file
      # on the device.
      mktmp = adb(
          'shell',
          'mktemp',
          '--tmpdir',
          '/data/local/tmp',
          stdout=subprocess.PIPE)
      on_device_config = mktmp.communicate()[0].decode().strip().strip()
      if mktmp.returncode != 0:
        prt('Failed to create config on device', ANSI.RED)
        sys.exit(1)
      exit_code = adb('push', '--sync', args.config, on_device_config).wait()
      if exit_code != 0:
        prt('Failed to push config on device', ANSI.RED)
        sys.exit(1)
      cmd = ['cat', on_device_config, '|'] + cmd
    else:
      on_host_config = args.config
  else:
    cmd += ['-t', args.time, '-b', args.buffer]
    for app in args.app:
      cmd += ['--app', '\'' + app + '\'']
    cmd += args.events

  # Perfetto will error out with a proper message if both a config file and
  # short options are specified. No need to replicate that logic.

  # Work out the output file or directory.
  if args.out.endswith('/') or os.path.isdir(args.out):
    host_dir = args.out
    host_file = os.path.join(args.out, fname)
  else:
    host_file = args.out
    host_dir = os.path.dirname(host_file)
    if host_dir == '':
      host_dir = '.'
      host_file = './' + host_file
  if not os.path.exists(host_dir):
    shutil.os.makedirs(host_dir)

  with open(on_host_config or os.devnull, 'rb') as f:
    print('Running ' + ' '.join(cmd))
    proc = adb('shell', *cmd, stdin=f, stdout=subprocess.PIPE)
    proc_out = proc.communicate()[0].decode().strip()
    if on_device_config is not None:
      adb('shell', 'rm', on_device_config).wait()
    # On older versions of Android (x86_64 emulator running API 22) the output
    # looks like:
    #   WARNING: linker: /data/local/tmp/tracebox: unused DT entry: ...
    #   WARNING: ... (other 2 WARNING: linker: lines)
    #   1234  <-- The actual pid we want.
    match = re.search(r'^(\d+)$', proc_out, re.M)
    if match is None:
      prt('Failed to read the pid from perfetto --background', ANSI.RED)
      prt(proc_out)
      sys.exit(1)
    bg_pid = match.group(1)
    exit_code = proc.wait()

  if exit_code != 0:
    prt('Perfetto invocation failed', ANSI.RED)
    sys.exit(1)

  prt('Trace started. Press CTRL+C to stop', ANSI.BLACK + ANSI.BG_BLUE)
  logcat = adb('logcat', '-v', 'brief', '-s', 'perfetto', '-b', 'main', '-T',
               '1')

  ctrl_c_count = 0
  adb_failure_count = 0
  while ctrl_c_count < 2:
    try:
      # On older Android devices adbd doesn't propagate the exit code. Hence
      # the RUN/TERM parts.
      poll = adb(
          'shell',
          'test -d /proc/%s && echo RUN || echo TERM' % bg_pid,
          stdout=subprocess.PIPE)
      poll_res = poll.communicate()[0].decode().strip()
      if poll_res == 'TERM':
        break  # Process terminated
      if poll_res == 'RUN':
        # The 'perfetto' cmdline client is still running. If previously we had
        # an ADB error, tell the user now it's all right again.
        if adb_failure_count > 0:
          adb_failure_count = 0
          prt('ADB connection re-established, the trace is still ongoing',
              ANSI.BLUE)
        time.sleep(0.5)
        continue
      # Some ADB error happened. This can happen when tracing soon after boot,
      # before logging in, when adb gets restarted.
      adb_failure_count += 1
      if adb_failure_count >= MAX_ADB_FAILURES:
        prt('Too many unrecoverable ADB failures, bailing out', ANSI.RED)
        sys.exit(1)
      time.sleep(2)
    except KeyboardInterrupt:
      sig = 'TERM' if ctrl_c_count == 0 else 'KILL'
      ctrl_c_count += 1
      prt('Stopping the trace (SIG%s)' % sig, ANSI.BLACK + ANSI.BG_YELLOW)
      adb('shell', 'kill -%s %s' % (sig, bg_pid)).wait()

  logcat.kill()
  logcat.wait()

  prt('\n')
  prt('Pulling into %s' % host_file, ANSI.BOLD)
  adb('pull', device_file, host_file).wait()
  adb('shell', 'rm -f ' + device_file).wait()

  if not args.no_open:
    prt('\n')
    prt('Opening the trace (%s) in the browser' % host_file)
    open_trace_in_browser(host_file)


def prt(msg, colors=ANSI.END):
  print(colors + msg + ANSI.END)


def find_adb():
  """ Locate the "right" adb path

  If adb is in the PATH use that (likely what the user wants) otherwise use the
  hermetic one in our SDK copy.
  """
  global adb_path
  for path in ['adb', HERMETIC_ADB_PATH]:
    try:
      subprocess.call([path, '--version'], stdout=devnull, stderr=devnull)
      adb_path = path
      break
    except OSError:
      continue
  if adb_path is None:
    sdk_url = 'https://developer.android.com/studio/releases/platform-tools'
    prt('Could not find a suitable adb binary in the PATH. ', ANSI.RED)
    prt('You can download adb from %s' % sdk_url, ANSI.RED)
    sys.exit(1)


def open_trace_in_browser(path):
  # We reuse the HTTP+RPC port because it's the only one allowed by the CSP.
  PORT = 9001
  os.chdir(os.path.dirname(path))
  fname = os.path.basename(path)
  socketserver.TCPServer.allow_reuse_address = True
  with socketserver.TCPServer(('127.0.0.1', PORT), HttpHandler) as httpd:
    webbrowser.open_new_tab(
        'https://ui.perfetto.dev/#!/?url=http://127.0.0.1:%d/%s' %
        (PORT, fname))
    while httpd.__dict__.get('last_request') != '/' + fname:
      httpd.handle_request()


def adb(*args, stdin=devnull, stdout=None):
  cmd = [adb_path, *args]
  setpgrp = None
  if os.name != 'nt':
    # On Linux/Mac, start a new process group so all child processes are killed
    # on exit. Unsupported on Windows.
    setpgrp = lambda: os.setpgrp()
  proc = subprocess.Popen(cmd, stdin=stdin, stdout=stdout, preexec_fn=setpgrp)
  procs.append(proc)
  return proc


def kill_all_subprocs_on_exit():
  for p in [p for p in procs if p.poll() is None]:
    p.kill()


def check_hash(file_name, sha_value):
  with open(file_name, 'rb') as fd:
    file_hash = hashlib.sha1(fd.read()).hexdigest()
    return file_hash == sha_value


if __name__ == '__main__':
  sys.exit(main())

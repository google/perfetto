#!/usr/bin/env python3
# Copyright (C) 2017 The Android Open Source Project
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

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import atexit
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

from perfetto.prebuilts.manifests.traceconv import *
from perfetto.prebuilts.perfetto_prebuilts import *

NULL = open(os.devnull)
NOOUT = {
    'stdout': NULL,
    'stderr': NULL,
}

UUID = str(uuid.uuid4())[-6:]

PACKAGES_LIST_CFG = '''data_sources {
  config {
    name: "android.packages_list"
  }
}
'''

CFG_INDENT = '      '
CFG = '''buffers {{
  size_kb: 63488
}}

data_sources {{
  config {{
    name: "android.heapprofd"
    heapprofd_config {{
      shmem_size_bytes: {shmem_size}
      sampling_interval_bytes: {interval}
{target_cfg}
    }}
  }}
}}

duration_ms: {duration}
write_into_file: true
flush_timeout_ms: 30000
flush_period_ms: 604800000
'''

# flush_period_ms of 1 week to suppress trace_processor_shell warning.

CONTINUOUS_DUMP = """
      continuous_dump_config {{
        dump_phase_ms: 0
        dump_interval_ms: {dump_interval}
      }}
"""

PROFILE_LOCAL_PATH = os.path.join(tempfile.gettempdir(), UUID)

IS_INTERRUPTED = False


def sigint_handler(sig, frame):
  global IS_INTERRUPTED
  IS_INTERRUPTED = True


def print_no_profile_error():
  print("No profiles generated", file=sys.stderr)
  print(
      "If this is unexpected, check "
      "https://perfetto.dev/docs/data-sources/native-heap-profiler#troubleshooting.",
      file=sys.stderr)


def known_issues_url(number):
  return ('https://perfetto.dev/docs/data-sources/native-heap-profiler'
          '#known-issues-android{}'.format(number))


KNOWN_ISSUES = {
    '10': known_issues_url(10),
    'Q': known_issues_url(10),
    '11': known_issues_url(11),
    'R': known_issues_url(11),
}


def maybe_known_issues():
  release_or_codename = subprocess.check_output(
      ['adb', 'shell', 'getprop',
       'ro.build.version.release_or_codename']).decode('utf-8').strip()
  return KNOWN_ISSUES.get(release_or_codename, None)


SDK = {
    'R': 30,
}


def release_or_newer(release):
  sdk = int(
      subprocess.check_output(
          ['adb', 'shell', 'getprop',
           'ro.system.build.version.sdk']).decode('utf-8').strip())
  if sdk >= SDK[release]:
    return True
  codename = subprocess.check_output(
      ['adb', 'shell', 'getprop',
       'ro.build.version.codename']).decode('utf-8').strip()
  return codename == release


ORDER = ['-n', '-p', '-i', '-o']


def arg_order(action):
  result = len(ORDER)
  for opt in action.option_strings:
    if opt in ORDER:
      result = min(ORDER.index(opt), result)
  return result, action.option_strings[0].strip('-')


def print_options(parser):
  for action in sorted(parser._actions, key=arg_order):
    if action.help is argparse.SUPPRESS:
      continue
    opts = ', '.join('`' + x + '`' for x in action.option_strings)
    metavar = '' if action.metavar is None else ' _' + action.metavar + '_'
    print('{}{}'.format(opts, metavar))
    print(':    {}'.format(action.help))
    print()


def main(argv):
  parser = argparse.ArgumentParser(description="""Collect a heap profile

  The PERFETTO_PROGUARD_MAP=packagename=map_filename.txt[:packagename=map_filename.txt...] environment variable can be used to pass proguard deobfuscation maps for different packages""", formatter_class=argparse.RawDescriptionHelpFormatter)

  parser.add_argument(
      "-i",
      "--interval",
      help="Sampling interval. "
      "Default 4096 (4KiB)",
      type=int,
      default=4096)
  parser.add_argument(
      "-d",
      "--duration",
      help="Duration of profile (ms). 0 to run until interrupted. "
      "Default: until interrupted by user.",
      type=int,
      default=0)
  # This flag is a no-op now. We never start heapprofd explicitly using system
  # properties.
  parser.add_argument(
      "--no-start", help="Do not start heapprofd.", action='store_true')
  parser.add_argument(
      "-p",
      "--pid",
      help="Comma-separated list of PIDs to "
      "profile.",
      metavar="PIDS")
  parser.add_argument(
      "-n",
      "--name",
      help="Comma-separated list of process "
      "names to profile.",
      metavar="NAMES")
  parser.add_argument(
      "-c",
      "--continuous-dump",
      help="Dump interval in ms. 0 to disable continuous dump.",
      type=int,
      default=0)
  parser.add_argument(
      "--heaps",
      help="Comma-separated list of heaps to collect, e.g: malloc,art. "
      "Requires Android 12.",
      metavar="HEAPS")
  parser.add_argument(
      "--all-heaps",
      action="store_true",
      help="Collect allocations from all heaps registered by target.")
  parser.add_argument(
      "--no-android-tree-symbolization",
      action="store_true",
      help="Do not symbolize using currently lunched target in the "
      "Android tree.")
  parser.add_argument(
      "--disable-selinux",
      action="store_true",
      help="Disable SELinux enforcement for duration of "
      "profile.")
  parser.add_argument(
      "--no-versions",
      action="store_true",
      help="Do not get version information about APKs.")
  parser.add_argument(
      "--no-running",
      action="store_true",
      help="Do not target already running processes. Requires Android 11.")
  parser.add_argument(
      "--no-startup",
      action="store_true",
      help="Do not target processes that start during "
      "the profile. Requires Android 11.")
  parser.add_argument(
      "--shmem-size",
      help="Size of buffer between client and "
      "heapprofd. Default 8MiB. Needs to be a power of two "
      "multiple of 4096, at least 8192.",
      type=int,
      default=8 * 1048576)
  parser.add_argument(
      "--block-client",
      help="When buffer is full, block the "
      "client to wait for buffer space. Use with caution as "
      "this can significantly slow down the client. "
      "This is the default",
      action="store_true")
  parser.add_argument(
      "--block-client-timeout",
      help="If --block-client is given, do not block any allocation for "
      "longer than this timeout (us).",
      type=int)
  parser.add_argument(
      "--no-block-client",
      help="When buffer is full, stop the "
      "profile early.",
      action="store_true")
  parser.add_argument(
      "--idle-allocations",
      help="Keep track of how many "
      "bytes were unused since the last dump, per "
      "callstack",
      action="store_true")
  parser.add_argument(
      "--dump-at-max",
      help="Dump the maximum memory usage "
      "rather than at the time of the dump.",
      action="store_true")
  parser.add_argument(
      "--disable-fork-teardown",
      help="Do not tear down client in forks. This can be useful for programs "
      "that use vfork. Android 11+ only.",
      action="store_true")
  parser.add_argument(
      "--simpleperf",
      action="store_true",
      help="Get simpleperf profile of heapprofd. This is "
      "only for heapprofd development.")
  parser.add_argument(
      "--traceconv-binary", help="Path to local trace to text. For debugging.")
  parser.add_argument(
      "--no-annotations",
      help="Do not suffix the pprof function names with Android ART mode "
      "annotations such as [jit].",
      action="store_true")
  parser.add_argument(
      "--print-config",
      action="store_true",
      help="Print config instead of running. For debugging.")
  parser.add_argument(
      "-o",
      "--output",
      help="Output directory.",
      metavar="DIRECTORY",
      default=None)
  parser.add_argument(
      "--print-options", action="store_true", help=argparse.SUPPRESS)

  args = parser.parse_args()
  if args.print_options:
    print_options(parser)
    return 0
  fail = False
  if args.block_client and args.no_block_client:
    print(
        "FATAL: Both block-client and no-block-client given.", file=sys.stderr)
    fail = True
  if args.pid is None and args.name is None:
    print("FATAL: Neither PID nor NAME given.", file=sys.stderr)
    fail = True
  if args.duration is None:
    print("FATAL: No duration given.", file=sys.stderr)
    fail = True
  if args.interval is None:
    print("FATAL: No interval given.", file=sys.stderr)
    fail = True
  if args.shmem_size % 4096:
    print("FATAL: shmem-size is not a multiple of 4096.", file=sys.stderr)
    fail = True
  if args.shmem_size < 8192:
    print("FATAL: shmem-size is less than 8192.", file=sys.stderr)
    fail = True
  if args.shmem_size & (args.shmem_size - 1):
    print("FATAL: shmem-size is not a power of two.", file=sys.stderr)
    fail = True

  target_cfg = ""
  if not args.no_block_client:
    target_cfg += CFG_INDENT + "block_client: true\n"
  if args.block_client_timeout:
    target_cfg += (
        CFG_INDENT +
        "block_client_timeout_us: %s\n" % args.block_client_timeout)
  if args.no_startup:
    target_cfg += CFG_INDENT + "no_startup: true\n"
  if args.no_running:
    target_cfg += CFG_INDENT + "no_running: true\n"
  if args.dump_at_max:
    target_cfg += CFG_INDENT + "dump_at_max: true\n"
  if args.disable_fork_teardown:
    target_cfg += CFG_INDENT + "disable_fork_teardown: true\n"
  if args.all_heaps:
    target_cfg += CFG_INDENT + "all_heaps: true\n"
  if args.pid:
    for pid in args.pid.split(','):
      try:
        pid = int(pid)
      except ValueError:
        print("FATAL: invalid PID %s" % pid, file=sys.stderr)
        fail = True
      target_cfg += CFG_INDENT + 'pid: {}\n'.format(pid)
  if args.name:
    for name in args.name.split(','):
      target_cfg += CFG_INDENT + 'process_cmdline: "{}"\n'.format(name)
  if args.heaps:
    for heap in args.heaps.split(','):
      target_cfg += CFG_INDENT + 'heaps: "{}"\n'.format(heap)

  if fail:
    parser.print_help()
    return 1

  traceconv_binary = args.traceconv_binary

  if args.continuous_dump:
    target_cfg += CONTINUOUS_DUMP.format(dump_interval=args.continuous_dump)
  cfg = CFG.format(
      interval=args.interval,
      duration=args.duration,
      target_cfg=target_cfg,
      shmem_size=args.shmem_size)
  if not args.no_versions:
    cfg += PACKAGES_LIST_CFG

  if args.print_config:
    print(cfg)
    return 0

  # Do this AFTER print_config so we do not download traceconv only to
  # print out the config.
  if traceconv_binary is None:
    traceconv_binary = get_perfetto_prebuilt(TRACECONV_MANIFEST, soft_fail=True)

  known_issues = maybe_known_issues()
  if known_issues:
    print('If you are experiencing problems, please see the known issues for '
          'your release: {}.'.format(known_issues))

  # TODO(fmayer): Maybe feature detect whether we can remove traces instead of
  # this.
  uuid_trace = release_or_newer('R')
  if uuid_trace:
    profile_device_path = '/data/misc/perfetto-traces/profile-' + UUID
  else:
    user = subprocess.check_output(['adb', 'shell',
                                    'whoami']).decode('utf-8').strip()
    profile_device_path = '/data/misc/perfetto-traces/profile-' + user

  perfetto_cmd = ('CFG=\'{cfg}\'; echo ${{CFG}} | '
                  'perfetto --txt -c - -o ' + profile_device_path + ' -d')

  if args.disable_selinux:
    enforcing = subprocess.check_output(['adb', 'shell',
                                         'getenforce']).decode('utf-8').strip()
    atexit.register(
        subprocess.check_call,
        ['adb', 'shell', 'su root setenforce %s' % enforcing])
    subprocess.check_call(['adb', 'shell', 'su root setenforce 0'])

  if args.simpleperf:
    subprocess.check_call([
        'adb', 'shell', 'mkdir -p /data/local/tmp/heapprofd_profile && '
        'cd /data/local/tmp/heapprofd_profile &&'
        '(nohup simpleperf record -g -p $(pidof heapprofd) 2>&1 &) '
        '> /dev/null'
    ])

  profile_target = PROFILE_LOCAL_PATH
  if args.output is not None:
    profile_target = args.output
  else:
    os.mkdir(profile_target)

  if not os.path.isdir(profile_target):
    print(
        "Output directory {} not found".format(profile_target), file=sys.stderr)
    return 1

  if os.listdir(profile_target):
    print(
        "Output directory {} not empty".format(profile_target), file=sys.stderr)
    return 1

  perfetto_pid = subprocess.check_output(
      ['adb', 'exec-out', perfetto_cmd.format(cfg=cfg)]).strip()
  try:
    perfetto_pid = int(perfetto_pid.strip())
  except ValueError:
    print("Failed to invoke perfetto: {}".format(perfetto_pid), file=sys.stderr)
    return 1

  old_handler = signal.signal(signal.SIGINT, sigint_handler)
  print("Profiling active. Press Ctrl+C to terminate.")
  print("You may disconnect your device.")
  print()
  exists = True
  device_connected = True
  while not device_connected or (exists and not IS_INTERRUPTED):
    exists = subprocess.call(
        ['adb', 'shell', '[ -d /proc/{} ]'.format(perfetto_pid)], **NOOUT) == 0
    device_connected = subprocess.call(['adb', 'shell', 'true'], **NOOUT) == 0
    time.sleep(1)
  print("Waiting for profiler shutdown...")
  signal.signal(signal.SIGINT, old_handler)
  if IS_INTERRUPTED:
    # Not check_call because it could have existed in the meantime.
    subprocess.call(['adb', 'shell', 'kill', '-INT', str(perfetto_pid)])
  if args.simpleperf:
    subprocess.check_call(['adb', 'shell', 'killall', '-INT', 'simpleperf'])
    print("Waiting for simpleperf to exit.")
    while subprocess.call(
        ['adb', 'shell', '[ -f /proc/$(pidof simpleperf)/exe ]'], **NOOUT) == 0:
      time.sleep(1)
    subprocess.check_call(
        ['adb', 'pull', '/data/local/tmp/heapprofd_profile', profile_target])
    print("Pulled simpleperf profile to " + profile_target +
          "/heapprofd_profile")

  # Wait for perfetto cmd to return.
  while exists:
    exists = subprocess.call(
        ['adb', 'shell', '[ -d /proc/{} ]'.format(perfetto_pid)]) == 0
    time.sleep(1)

  profile_host_path = os.path.join(profile_target, 'raw-trace')
  subprocess.check_call(['adb', 'pull', profile_device_path, profile_host_path],
                        stdout=NULL)
  if uuid_trace:
    subprocess.check_call(['adb', 'shell', 'rm', profile_device_path],
                          stdout=NULL)

  if traceconv_binary is None:
    print('Wrote profile to {}'.format(profile_host_path))
    print(
        'This file can be opened using the Perfetto UI, https://ui.perfetto.dev'
    )
    return 0

  binary_path = os.getenv('PERFETTO_BINARY_PATH')
  if not args.no_android_tree_symbolization:
    product_out = os.getenv('ANDROID_PRODUCT_OUT')
    if product_out:
      product_out_symbols = product_out + '/symbols'
    else:
      product_out_symbols = None

    if binary_path is None:
      binary_path = product_out_symbols
    elif product_out_symbols is not None:
      binary_path += os.pathsep + product_out_symbols

  trace_file = os.path.join(profile_target, 'raw-trace')
  concat_files = [trace_file]

  if binary_path is not None:
    with open(os.path.join(profile_target, 'symbols'), 'w') as fd:
      ret = subprocess.call([
          traceconv_binary, 'symbolize',
          os.path.join(profile_target, 'raw-trace')
      ],
                            env=dict(
                                os.environ, PERFETTO_BINARY_PATH=binary_path),
                            stdout=fd)
    if ret == 0:
      concat_files.append(os.path.join(profile_target, 'symbols'))
    else:
      print("Failed to symbolize. Continuing without symbols.", file=sys.stderr)

  proguard_map = os.getenv('PERFETTO_PROGUARD_MAP')
  if proguard_map is not None:
    with open(os.path.join(profile_target, 'deobfuscation-packets'), 'w') as fd:
      ret = subprocess.call([
          traceconv_binary, 'deobfuscate',
          os.path.join(profile_target, 'raw-trace')
      ],
                            env=dict(
                                os.environ, PERFETTO_PROGUARD_MAP=proguard_map),
                            stdout=fd)
    if ret == 0:
      concat_files.append(os.path.join(profile_target, 'deobfuscation-packets'))
    else:
      print(
          "Failed to deobfuscate. Continuing without deobfuscated.",
          file=sys.stderr)

  if len(concat_files) > 1:
    with open(os.path.join(profile_target, 'symbolized-trace'), 'wb') as out:
      for fn in concat_files:
        with open(fn, 'rb') as inp:
          while True:
            buf = inp.read(4096)
            if not buf:
              break
            out.write(buf)
    trace_file = os.path.join(profile_target, 'symbolized-trace')

  conversion_args = [traceconv_binary, 'profile'] + (
      ['--no-annotations'] if args.no_annotations else []) + [trace_file]
  traceconv_output = subprocess.check_output(conversion_args)
  profile_path = None
  for word in traceconv_output.decode('utf-8').split():
    if 'heap_profile-' in word:
      profile_path = word
  if profile_path is None:
    print_no_profile_error()
    return 1

  profile_files = os.listdir(profile_path)
  if not profile_files:
    print_no_profile_error()
    return 1

  for profile_file in profile_files:
    shutil.copy(os.path.join(profile_path, profile_file), profile_target)

  symlink_path = None
  if not sys.platform.startswith('win'):
    subprocess.check_call(
        ['gzip'] + [os.path.join(profile_target, x) for x in profile_files])
    if args.output is None:
      symlink_path = os.path.join(
          os.path.dirname(profile_target), "heap_profile-latest")
      if os.path.lexists(symlink_path):
        os.unlink(symlink_path)
      os.symlink(profile_target, symlink_path)

  if symlink_path is not None:
    print("Wrote profiles to {} (symlink {})".format(profile_target,
                                                     symlink_path))
  else:
    print("Wrote profiles to {}".format(profile_target))

  print("The raw-trace file can be viewed using https://ui.perfetto.dev.")
  print("The heap_dump.* files can be viewed using pprof/ (Googlers only) " +
        "or https://www.speedscope.app/.")
  print("The two above are equivalent. The raw-trace contains the union of " +
        "all the heap dumps.")


if __name__ == '__main__':
  sys.exit(main(sys.argv))

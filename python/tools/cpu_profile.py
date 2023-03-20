#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
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
"""Runs tracing with CPU profiling enabled, and symbolizes traces if requested.

For usage instructions, please see:
https://perfetto.dev/docs/quickstart/callstack-sampling

Adapted in large part from `heap_profile`.
"""

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import uuid

from perfetto.prebuilts.manifests.traceconv import *
from perfetto.prebuilts.perfetto_prebuilts import *

# Used for creating directories, etc.
UUID = str(uuid.uuid4())[-6:]

# See `sigint_handler` below.
IS_INTERRUPTED = False


def sigint_handler(signal, frame):
  """Useful for cleanly interrupting tracing."""
  global IS_INTERRUPTED
  IS_INTERRUPTED = True


def exit_with_no_profile():
  sys.exit("No profiles generated.")


def exit_with_bug_report(error):
  sys.exit(
      "{}\n\n If this is unexpected, please consider filing a bug at: \n"
      "https://perfetto.dev/docs/contributing/getting-started#bugs.".format(
          error))


def adb_check_output(command):
  """Runs an `adb` command and returns its output."""
  try:
    return subprocess.check_output(command).decode('utf-8')
  except FileNotFoundError:
    sys.exit("`adb` not found: Is it installed or on PATH?")
  except subprocess.CalledProcessError as error:
    sys.exit("`adb` error: Are any (or multiple) devices connected?\n"
             "If multiple devices are connected, please select one by "
             "setting `ANDROID_SERIAL=device_id`.\n"
             "{}".format(error))
  except Exception as error:
    exit_with_bug_report(error)


def parse_and_validate_args():
  """Parses, validates, and returns command-line arguments for this script."""
  DESCRIPTION = """Runs tracing with CPU profiling enabled, and symbolizes
  traces if requested.

  For usage instructions, please see:
  https://perfetto.dev/docs/quickstart/callstack-sampling
  """
  parser = argparse.ArgumentParser(description=DESCRIPTION)
  parser.add_argument(
      "-f",
      "--frequency",
      help="Sampling frequency (Hz). "
      "Default: 100 Hz.",
      metavar="FREQUENCY",
      type=int,
      default=100)
  parser.add_argument(
      "-d",
      "--duration",
      help="Duration of profile (ms). 0 to run until interrupted. "
      "Default: until interrupted by user.",
      metavar="DURATION",
      type=int,
      default=0)
  # Profiling using hardware counters.
  parser.add_argument(
      "-e",
      "--event",
      help="Use the specified hardware counter event for sampling.",
      metavar="EVENT",
      action="append",
      # See: '//perfetto/protos/perfetto/trace/perfetto_trace.proto'.
      choices=['HW_CPU_CYCLES', 'HW_INSTRUCTIONS', 'HW_CACHE_REFERENCES',
               'HW_CACHE_MISSES', 'HW_BRANCH_INSTRUCTIONS', 'HW_BRANCH_MISSES',
               'HW_BUS_CYCLES', 'HW_STALLED_CYCLES_FRONTEND',
               'HW_STALLED_CYCLES_BACKEND'],
      default=[])
  parser.add_argument(
      "-k",
      "--kernel-frames",
      help="Collect kernel frames.  Default: false.",
      action="store_true",
      default=False)
  parser.add_argument(
      "-n",
      "--name",
      help="Comma-separated list of names of processes to be profiled.",
      metavar="NAMES",
      default=None)
  parser.add_argument(
      "-p",
      "--partial-matching",
      help="If set, enables \"partial matching\" on the strings in --names/-n."
      "Processes that are already running when profiling is started, and whose "
      "names include any of the values in --names/-n as substrings will be "
      "profiled.",
      action="store_true")
  parser.add_argument(
      "-c",
      "--config",
      help="A custom configuration file, if any, to be used for profiling. "
      "If provided, --frequency/-f, --duration/-d, and --name/-n are not used.",
      metavar="CONFIG",
      default=None)
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
      help="Output directory for recorded trace.",
      metavar="DIRECTORY",
      default=None)

  args = parser.parse_args()
  if args.config is not None:
    if args.name is not None:
      sys.exit("--name/-n should not be specified with --config/-c.")
    elif args.event:
      sys.exit("-e/--event should not be specified with --config/-c.")
  elif args.config is None and args.name is None:
    sys.exit("One of --names/-n or --config/-c is required.")

  return args


def get_matching_processes(args, names_to_match):
  """Returns a list of currently-running processes whose names match
  `names_to_match`.

  Args:
    args: The command-line arguments provided to this script.
    names_to_match: The list of process names provided by the user.
  """
  # Returns names as they are.
  if not args.partial_matching:
    return names_to_match

  # Attempt to match names to names of currently running processes.
  PS_PROCESS_OFFSET = 8
  matching_processes = []
  for line in adb_check_output(['adb', 'shell', 'ps', '-A']).splitlines():
    line_split = line.split()
    if len(line_split) <= PS_PROCESS_OFFSET:
      continue
    process = line_split[PS_PROCESS_OFFSET]
    for name in names_to_match:
      if name in process:
        matching_processes.append(process)
        break

  return matching_processes


def get_perfetto_config(args):
  """Returns a Perfetto config with CPU profiling enabled for the selected
  processes.

  Args:
    args: The command-line arguments provided to this script.
  """
  if args.config is not None:
    try:
      with open(args.config, 'r') as config_file:
        return config_file.read()
    except IOError as error:
      sys.exit("Unable to read config file: {}".format(error))

  CONFIG_INDENT = '          '
  CONFIG = textwrap.dedent('''\
  buffers {{
    size_kb: 2048
  }}

  buffers {{
    size_kb: 63488
  }}

  data_sources {{
    config {{
      name: "linux.process_stats"
      target_buffer: 0
      process_stats_config {{
        proc_stats_poll_ms: 100
      }}
    }}
  }}

  duration_ms: {duration}
  write_into_file: true
  flush_timeout_ms: 30000
  flush_period_ms: 604800000
  ''')

  matching_processes = []
  if args.name is not None:
    names_to_match = [name.strip() for name in args.name.split(',')]
    matching_processes = get_matching_processes(args, names_to_match)

  if not matching_processes:
    sys.exit("No running processes matched for profiling.")

  target_config = "\n".join(
      [f'{CONFIG_INDENT}target_cmdline: "{p}"' for p in matching_processes])

  events = args.event or ['SW_CPU_CLOCK']
  for event in events:
    CONFIG += (textwrap.dedent('''
    data_sources {{
      config {{
        name: "linux.perf"
        target_buffer: 1
        perf_event_config {{
          timebase {{
            counter: %s
            frequency: {frequency}
            timestamp_clock: PERF_CLOCK_MONOTONIC
          }}
          callstack_sampling {{
            scope {{
    {target_config}
            }}
            kernel_frames: {kernel_config}
          }}
        }}
      }}
    }}
    ''') % (event))

  if args.kernel_frames:
    kernel_config = "true"
  else:
    kernel_config = "false"

  if not args.print_config:
    print("Configured profiling for these processes:\n")
    for matching_process in matching_processes:
      print(matching_process)
    print()

  config = CONFIG.format(
      frequency=args.frequency,
      duration=args.duration,
      target_config=target_config,
      kernel_config=kernel_config)

  return config


def release_or_newer(release):
  """Returns whether a new enough Android release is being used."""
  SDK = {'R': 30}
  sdk = int(
      adb_check_output(
          ['adb', 'shell', 'getprop', 'ro.system.build.version.sdk']).strip())
  if sdk >= SDK[release]:
    return True

  codename = adb_check_output(
      ['adb', 'shell', 'getprop', 'ro.build.version.codename']).strip()
  return codename == release


def get_and_prepare_profile_target(args):
  """Returns the target where the trace/profile will be output.  Creates a
  new directory if necessary.

  Args:
    args: The command-line arguments provided to this script.
  """
  profile_target = os.path.join(tempfile.gettempdir(), UUID)
  if args.output is not None:
    profile_target = args.output
  else:
    os.makedirs(profile_target, exist_ok=True)
  if not os.path.isdir(profile_target):
    sys.exit("Output directory {} not found.".format(profile_target))
  if os.listdir(profile_target):
    sys.exit("Output directory {} not empty.".format(profile_target))

  return profile_target


def record_trace(config, profile_target):
  """Runs Perfetto with the provided configuration to record a trace.

  Args:
    config: The Perfetto config to be used for tracing/profiling.
    profile_target: The directory where the recorded trace is output.
  """
  NULL = open(os.devnull)
  NO_OUT = {
      'stdout': NULL,
      'stderr': NULL,
  }
  if not release_or_newer('R'):
    sys.exit("This tool requires Android R+ to run.")

  # Push configuration to the device.
  tf = tempfile.NamedTemporaryFile()
  tf.file.write(config.encode('utf-8'))
  tf.file.flush()
  profile_config_path = '/data/misc/perfetto-configs/config-' + UUID
  adb_check_output(['adb', 'push', tf.name, profile_config_path])
  tf.close()


  profile_device_path = '/data/misc/perfetto-traces/profile-' + UUID
  perfetto_command = ('perfetto --txt -c {} -o {} -d')
  try:
    perfetto_pid = int(
        adb_check_output([
            'adb', 'exec-out',
            perfetto_command.format(profile_config_path, profile_device_path)
        ]).strip())
  except ValueError as error:
    sys.exit("Unable to start profiling: {}".format(error))

  print("Profiling active. Press Ctrl+C to terminate.")

  old_handler = signal.signal(signal.SIGINT, sigint_handler)

  perfetto_alive = True
  while perfetto_alive and not IS_INTERRUPTED:
    perfetto_alive = subprocess.call(
        ['adb', 'shell', '[ -d /proc/{} ]'.format(perfetto_pid)], **NO_OUT) == 0
    time.sleep(0.25)

  print("Finishing profiling and symbolization...")

  if IS_INTERRUPTED:
    adb_check_output(['adb', 'shell', 'kill', '-INT', str(perfetto_pid)])

  # Restore old handler.
  signal.signal(signal.SIGINT, old_handler)

  while perfetto_alive:
    perfetto_alive = subprocess.call(
        ['adb', 'shell', '[ -d /proc/{} ]'.format(perfetto_pid)]) == 0
    time.sleep(0.25)

  profile_host_path = os.path.join(profile_target, 'raw-trace')
  adb_check_output(['adb', 'pull', profile_device_path, profile_host_path])
  adb_check_output(['adb', 'shell', 'rm', profile_config_path])
  adb_check_output(['adb', 'shell', 'rm', profile_device_path])


def get_traceconv():
  """Sets up and returns the path to `traceconv`."""
  try:
    traceconv = get_perfetto_prebuilt(TRACECONV_MANIFEST, soft_fail=True)
  except Exception as error:
    exit_with_bug_report(error)
  if traceconv is None:
    exit_with_bug_report(
        "Unable to download `traceconv` for symbolizing profiles.")

  return traceconv


def concatenate_files(files_to_concatenate, output_file):
  """Concatenates files.

  Args:
    files_to_concatenate: Paths for input files to concatenate.
    output_file: Path to the resultant output file.
  """
  with open(output_file, 'wb') as output:
    for file in files_to_concatenate:
      with open(file, 'rb') as input:
        shutil.copyfileobj(input, output)


def symbolize_trace(traceconv, profile_target):
  """Attempts symbolization of the recorded trace/profile, if symbols are
  available.

  Args:
    traceconv: The path to the `traceconv` binary used for symbolization.
    profile_target: The directory where the recorded trace was output.

  Returns:
    The path to the symbolized trace file if symbolization was completed,
    and the original trace file, if it was not.
  """
  binary_path = os.getenv('PERFETTO_BINARY_PATH')
  trace_file = os.path.join(profile_target, 'raw-trace')
  files_to_concatenate = [trace_file]

  if binary_path is not None:
    try:
      with open(os.path.join(profile_target, 'symbols'), 'w') as symbols_file:
        return_code = subprocess.call([traceconv, 'symbolize', trace_file],
                                      env=dict(
                                          os.environ,
                                          PERFETTO_BINARY_PATH=binary_path),
                                      stdout=symbols_file)
    except IOError as error:
      sys.exit("Unable to write symbols to disk: {}".format(error))
    if return_code == 0:
      files_to_concatenate.append(os.path.join(profile_target, 'symbols'))
    else:
      print("Failed to symbolize. Continuing without symbols.", file=sys.stderr)

  if len(files_to_concatenate) > 1:
    trace_file = os.path.join(profile_target, 'symbolized-trace')
    try:
      concatenate_files(files_to_concatenate, trace_file)
    except Exception as error:
      sys.exit("Unable to write symbolized profile to disk: {}".format(error))

  return trace_file


def generate_pprof_profiles(traceconv, trace_file, args):
  """Generates pprof profiles from the recorded trace.

  Args:
    traceconv: The path to the `traceconv` binary used for generating profiles.
    trace_file: The oath to the recorded and potentially symbolized trace file.

  Returns:
    The directory where pprof profiles are output.
  """
  try:
    conversion_args = [traceconv, 'profile', '--perf'] + (
        ['--no-annotations'] if args.no_annotations else []) + [trace_file]
    traceconv_output = subprocess.check_output(conversion_args)
  except Exception as error:
    exit_with_bug_report(
        "Unable to extract profiles from trace: {}".format(error))

  profiles_output_directory = None
  for word in traceconv_output.decode('utf-8').split():
    if 'perf_profile-' in word:
      profiles_output_directory = word
  if profiles_output_directory is None:
    exit_with_no_profile()
  return profiles_output_directory


def copy_profiles_to_destination(profile_target, profile_path):
  """Copies recorded profiles to `profile_target` from `profile_path`."""
  profile_files = os.listdir(profile_path)
  if not profile_files:
    exit_with_no_profile()

  try:
    for profile_file in profile_files:
      shutil.copy(os.path.join(profile_path, profile_file), profile_target)
  except Exception as error:
    sys.exit("Unable to copy profiles to {}: {}".format(profile_target, error))

  print("Wrote profiles to {}".format(profile_target))


def main(argv):
  args = parse_and_validate_args()
  profile_target = get_and_prepare_profile_target(args)
  trace_config = get_perfetto_config(args)
  if args.print_config:
    print(trace_config)
    return 0
  record_trace(trace_config, profile_target)
  traceconv = get_traceconv()
  trace_file = symbolize_trace(traceconv, profile_target)
  copy_profiles_to_destination(
      profile_target, generate_pprof_profiles(traceconv, trace_file, args))
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv))

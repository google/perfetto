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

from __future__ import print_function
import itertools
import subprocess
import time

USE_PYTHON3 = True


def RunAndReportIfLong(func, *args, **kargs):
  start = time.time()
  results = func(*args, **kargs)
  end = time.time()
  limit = 0.5  # seconds
  name = func.__name__
  runtime = end - start
  if runtime > limit:
    print("{} took >{:.2}s ({:.2}s)".format(name, limit, runtime))
  return results


def CheckChange(input, output):
  # There apparently is no way to wrap strings in blueprints, so ignore long
  # lines in them.
  def long_line_sources(x):
    return input.FilterSourceFile(
        x,
        files_to_check='.*',
        files_to_skip=[
            'Android[.]bp',
            "buildtools/grpc/BUILD.gn",
            '.*[.]json$',
            '.*[.]sql$',
            '.*[.]out$',
            'test/trace_processor/.*/tests.*$',
            '(.*/)?BUILD$',
            'WORKSPACE',
            '.*/Makefile$',
            '/perfetto_build_flags.h$',
            "infra/luci/.*",
        ])

  results = []
  results += RunAndReportIfLong(input.canned_checks.CheckDoNotSubmit, input,
                                output)
  results += RunAndReportIfLong(input.canned_checks.CheckChangeHasNoTabs, input,
                                output)
  results += RunAndReportIfLong(
      input.canned_checks.CheckLongLines,
      input,
      output,
      80,
      source_file_filter=long_line_sources)
  results += RunAndReportIfLong(
      input.canned_checks.CheckPatchFormatted, input, output, check_js=True)
  results += RunAndReportIfLong(input.canned_checks.CheckGNFormatted, input,
                                output)
  results += RunAndReportIfLong(CheckIncludeGuards, input, output)
  results += RunAndReportIfLong(CheckIncludeViolations, input, output)
  results += RunAndReportIfLong(CheckProtoComments, input, output)
  results += RunAndReportIfLong(CheckBuild, input, output)
  results += RunAndReportIfLong(CheckAndroidBlueprint, input, output)
  results += RunAndReportIfLong(CheckBinaryDescriptors, input, output)
  results += RunAndReportIfLong(CheckMergedTraceConfigProto, input, output)
  results += RunAndReportIfLong(CheckProtoEventList, input, output)
  results += RunAndReportIfLong(CheckBannedCpp, input, output)
  results += RunAndReportIfLong(CheckSqlModules, input, output)
  results += RunAndReportIfLong(CheckSqlMetrics, input, output)
  results += RunAndReportIfLong(CheckTestData, input, output)
  results += RunAndReportIfLong(CheckAmalgamatedPythonTools, input, output)
  return results


def CheckChangeOnUpload(input_api, output_api):
  return CheckChange(input_api, output_api)


def CheckChangeOnCommit(input_api, output_api):
  return CheckChange(input_api, output_api)


def CheckBuild(input_api, output_api):
  tool = 'tools/gen_bazel'

  # If no GN files were modified, bail out.
  def build_file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=('.*BUILD[.]gn$', '.*[.]gni$', 'BUILD\.extras', tool))

  if not input_api.AffectedSourceFiles(build_file_filter):
    return []
  if subprocess.call([tool, '--check-only']):
    return [
        output_api.PresubmitError('Bazel BUILD(s) are out of date. Run ' +
                                  tool + ' to update them.')
    ]
  return []


def CheckAndroidBlueprint(input_api, output_api):
  tool = 'tools/gen_android_bp'

  # If no GN files were modified, bail out.
  def build_file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=('.*BUILD[.]gn$', '.*[.]gni$', tool))

  if not input_api.AffectedSourceFiles(build_file_filter):
    return []
  if subprocess.call([tool, '--check-only']):
    return [
        output_api.PresubmitError('Android build files are out of date. ' +
                                  'Run ' + tool + ' to update them.')
    ]
  return []


def CheckIncludeGuards(input_api, output_api):
  tool = 'tools/fix_include_guards'

  def file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=['.*[.]cc$', '.*[.]h$', tool])

  if not input_api.AffectedSourceFiles(file_filter):
    return []
  if subprocess.call([tool, '--check-only']):
    return [
        output_api.PresubmitError('Please run ' + tool +
                                  ' to fix include guards.')
    ]
  return []


def CheckBannedCpp(input_api, output_api):
  bad_cpp = [
      (r'\bstd::stoi\b',
       'std::stoi throws exceptions prefer base::StringToInt32()'),
      (r'\bstd::stol\b',
       'std::stoull throws exceptions prefer base::StringToInt32()'),
      (r'\bstd::stoul\b',
       'std::stoull throws exceptions prefer base::StringToUint32()'),
      (r'\bstd::stoll\b',
       'std::stoull throws exceptions prefer base::StringToInt64()'),
      (r'\bstd::stoull\b',
       'std::stoull throws exceptions prefer base::StringToUint64()'),
      (r'\bstd::stof\b',
       'std::stof throws exceptions prefer base::StringToDouble()'),
      (r'\bstd::stod\b',
       'std::stod throws exceptions prefer base::StringToDouble()'),
      (r'\bstd::stold\b',
       'std::stold throws exceptions prefer base::StringToDouble()'),
      (r'\bstrncpy\b',
       'strncpy does not null-terminate if src > dst. Use base::StringCopy'),
      (r'[(=]\s*snprintf\(',
       'snprintf can return > dst_size. Use base::SprintfTrunc'),
      (r'//.*\bDNS\b',
       '// DNS (Do Not Ship) found. Did you mean to remove some testing code?'),
      (r'\bPERFETTO_EINTR\(close\(',
       'close(2) must not be retried on EINTR on Linux and other OSes '
       'that we run on, as the fd will be closed.'),
      (r'^#include <inttypes.h>', 'Use <cinttypes> rather than <inttypes.h>. ' +
       'See https://github.com/google/perfetto/issues/146'),
  ]

  def file_filter(x):
    return input_api.FilterSourceFile(x, files_to_check=[r'.*\.h$', r'.*\.cc$'])

  errors = []
  for f in input_api.AffectedSourceFiles(file_filter):
    for line_number, line in f.ChangedContents():
      if input_api.re.search(r'^\s*//', line):
        continue  # Skip comments
      for regex, message in bad_cpp:
        if input_api.re.search(regex, line):
          errors.append(
              output_api.PresubmitError('Banned pattern:\n  {}:{} {}'.format(
                  f.LocalPath(), line_number, message)))
  return errors


def CheckIncludeViolations(input_api, output_api):
  tool = 'tools/check_include_violations'

  def file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=['include/.*[.]h$', tool])

  if not input_api.AffectedSourceFiles(file_filter):
    return []
  if subprocess.call([tool]):
    return [output_api.PresubmitError(tool + ' failed.')]
  return []


def CheckBinaryDescriptors(input_api, output_api):
  tool = 'tools/gen_binary_descriptors'

  def file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=['protos/perfetto/.*[.]proto$', '.*[.]h', tool])

  if not input_api.AffectedSourceFiles(file_filter):
    return []
  if subprocess.call([tool, '--check-only']):
    return [
        output_api.PresubmitError('Please run ' + tool +
                                  ' to update binary descriptors.')
    ]
  return []


def CheckMergedTraceConfigProto(input_api, output_api):
  tool = 'tools/gen_merged_protos'

  def build_file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=['protos/perfetto/.*[.]proto$', tool])

  if not input_api.AffectedSourceFiles(build_file_filter):
    return []
  if subprocess.call([tool, '--check-only']):
    return [
        output_api.PresubmitError(
            'perfetto_config.proto or perfetto_trace.proto is out of ' +
            'date. Please run ' + tool + ' to update it.')
    ]
  return []


# Prevent removing or changing lines in event_list.
def CheckProtoEventList(input_api, output_api):
  for f in input_api.AffectedFiles():
    if f.LocalPath() != 'src/tools/ftrace_proto_gen/event_list':
      continue
    if any((not new_line.startswith('removed')) and new_line != old_line
           for old_line, new_line in zip(f.OldContents(), f.NewContents())):
      return [
          output_api.PresubmitError(
              'event_list only has two supported changes: '
              'appending a new line, and replacing a line with removed.')
      ]
  return []


def CheckProtoComments(input_api, output_api):
  tool = 'tools/check_proto_comments'

  def file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=['protos/perfetto/.*[.]proto$', tool])

  if not input_api.AffectedSourceFiles(file_filter):
    return []
  if subprocess.call([tool]):
    return [output_api.PresubmitError(tool + ' failed')]
  return []


def CheckSqlModules(input_api, output_api):
  tool = 'tools/check_sql_modules.py'

  def file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=['src/trace_processor/stdlib/.*[.]sql$', tool])

  if not input_api.AffectedSourceFiles(file_filter):
    return []
  if subprocess.call([tool]):
    return [output_api.PresubmitError(tool + ' failed')]
  return []


def CheckSqlMetrics(input_api, output_api):
  tool = 'tools/check_sql_metrics.py'

  def file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=['src/trace_processor/metrics/.*[.]sql$', tool])

  if not input_api.AffectedSourceFiles(file_filter):
    return []
  if subprocess.call([tool]):
    return [output_api.PresubmitError(tool + ' failed')]
  return []


def CheckTestData(input_api, output_api):
  tool = 'tools/test_data'
  if subprocess.call([tool, 'status', '--quiet']):
    return [
        output_api.PresubmitError(
            '//test/data is out of sync. Run ' + tool + ' status for more. \n'
            'If you rebaselined UI tests or added a new test trace, run:'
            '`tools/test_data upload`. Otherwise run `tools/install-build-deps`'
            ' or `tools/test_data download --overwrite` to sync local test_data'
        )
    ]
  return []


def CheckAmalgamatedPythonTools(input_api, output_api):
  tool = 'tools/gen_amalgamated_python_tools'

  # If no GN files were modified, bail out.
  def build_file_filter(x):
    return input_api.FilterSourceFile(x, files_to_check=('python/.*$', tool))

  if not input_api.AffectedSourceFiles(build_file_filter):
    return []
  if subprocess.call([tool, '--check-only']):
    return [
        output_api.PresubmitError(
            'amalgamated python tools/ are out of date. ' + 'Run ' + tool +
            ' to update them.')
    ]
  return []

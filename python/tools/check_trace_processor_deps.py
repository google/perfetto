#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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
"""Prevents code outside Trace Processor from depending on its internals.

Targets under src/trace_processor/util are intentionally reusable. All other
src/trace_processor targets are implementation details and must not be direct
dependencies of targets outside that directory.
"""

import argparse
import os
import sys

# gn_utils remains infrastructure for interrogating GN build descriptions.
ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT_DIR, 'tools'))

import gn_utils

TP_PREFIX = '//src/trace_processor'
ALLOWED_TP_PREFIX = '//src/trace_processor/util'

# These root executables aggregate Trace Processor's own tests and benchmarks.
TP_TEST_AGGREGATORS = {
    '//:all',
    '//:perfetto_benchmarks',
    '//:perfetto_integrationtests',
    '//:perfetto_trace_processor_unittests',
    '//:perfetto_unittests',
    '//:trace_processor_minimal_smoke_tests',
}

# Existing boundary violations. Do not add entries: move the shared code out of
# Trace Processor instead.
LEGACY_ALLOWED_EDGES = {
    ('//src/profiling/memory:end_to_end_tests', '//src/trace_processor:lib'),
    ('//src/protozero/text_to_proto:unittests',
     '//src/trace_processor:gen_cc_test_messages_descriptor'),
    ('//src/tools/protoprofile:common', '//src/trace_processor:lib'),
    ('//src/tools/protoprofile:common',
     '//src/trace_processor/importers/proto:gen_cc_android_extension_descriptor'
    ),
    ('//src/tools/protoprofile:common',
     '//src/trace_processor/importers/proto:gen_cc_trace_descriptor'),
    ('//src/traceconv:lib', '//src/trace_processor:export_json'),
    ('//src/traceconv:lib', '//src/trace_processor:lib'),
    ('//src/traceconv:lib', '//src/trace_processor:storage_minimal'),
    ('//src/traceconv:pprofbuilder',
     '//src/trace_processor/containers:containers'),
    ('//test:perfetto_end_to_end_integrationtests',
     '//src/trace_processor:trace_processor_shell'),
    ('//ui:ui', '//src/trace_processor:trace_processor.wasm'),
    ('//ui:ui', '//src/trace_processor:trace_processor_memory64.wasm'),
}


def is_in(label, prefix):
  return label == prefix or label.startswith((prefix + '/', prefix + ':'))


def check(desc):
  errors = []
  for label, target in desc.items():
    source = gn_utils.label_without_toolchain(label)
    if is_in(source, TP_PREFIX) or source in TP_TEST_AGGREGATORS:
      continue
    for dependency in target.get('deps', []):
      dependency = gn_utils.label_without_toolchain(dependency)
      edge = (source, dependency)
      if (is_in(dependency, TP_PREFIX) and
          not is_in(dependency, ALLOWED_TP_PREFIX) and
          edge not in LEGACY_ALLOWED_EDGES):
        errors.append(
            f'target "{source}" depends on Trace Processor internal target '
            f'"{dependency}"; only {ALLOWED_TP_PREFIX} targets are reusable.')
  return sorted(set(errors))


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      '--out',
      help='use an existing GN out directory instead of a temporary one')
  args = parser.parse_args()
  if args.out:
    errors = check(gn_utils.load_build_description(args.out))
  else:
    with gn_utils.BuildDescription('') as build:
      errors = check(build.desc)
  for error in errors:
    print(error, file=sys.stderr)
  return 1 if errors else 0


if __name__ == '__main__':
  sys.exit(main())

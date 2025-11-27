#!/usr/bin/env python3
#
# Copyright (C) 2023 The Android Open Source Project
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

import argparse
import os
import re
import subprocess
import sys

# Color codes
RED = '\033[0;31m'
GREEN = '\033[0;32m'
YELLOW = '\033[0;33m'
NC = '\033[0m'  # No Color


def info(msg):
  print(f"{GREEN}INFO:{NC} {msg}")


def warn(msg):
  print(f"{YELLOW}WARN:{NC} {msg}")


def error(msg):
  print(f"{RED}ERROR:{NC} {msg}", file=sys.stderr)
  sys.exit(1)


def prompt(msg):
  print(f"{YELLOW}ACTION:{NC} {msg}")
  input("Press Enter to continue...")


def confirm(msg):
  reply = input(f"{msg} [y/N] ").lower().strip()
  if reply not in ['y', 'yes']:
    error("Aborted by user.")


def run_cmd(*args, check=True):
  info(f"Running command: {' '.join(args)}")
  subprocess.run(args, check=check)


def run_cmd_output(*args, check=True):
  """Runs a command and returns its stdout."""
  info(f"Running command: {' '.join(args)}")
  # Using text=True to get stdout/stderr as strings
  result = subprocess.run(args, check=check, capture_output=True, text=True)
  return result.stdout.strip()


def cmd_succeeded(*args):
  """Runs a command and returns True if it succeeds."""
  return subprocess.run(args, capture_output=True).returncode == 0


def get_current_branch():
  """Returns the current git branch."""
  return run_cmd_output('git', 'rev-parse', '--abbrev-ref', 'HEAD')


def local_branch_exists(branch):
  """Checks if a local git branch exists."""
  return cmd_succeeded('git', 'show-ref', '--verify', '--quiet',
                       f'refs/heads/{branch}')


def remote_branch_exists(branch):
  """Checks if a remote git branch exists."""
  info(f"Checking for remote branch '{branch}'...")
  return cmd_succeeded('git', 'ls-remote', '--exit-code', '--heads', 'origin',
                       branch)


def tag_exists(tag):
  """Checks if a local git tag exists."""
  return cmd_succeeded('git', 'show-ref', '--verify', '--quiet',
                       f'refs/tags/{tag}')


def remote_tag_exists(tag):
  """Checks if a remote git tag exists."""
  info(f"Checking for remote tag '{tag}'...")
  return cmd_succeeded('git', 'ls-remote', '--exit-code', '--tags', 'origin',
                       tag)


def create_major_release(version, major_version):
  info(f"--- Creating a new major version: {version} ---")

  prompt("Please check that no release-blockers are open: "
         "http://b/savedsearches/5776355")
  prompt("Please trigger all builds on LUCI and wait for their success: "
         "https://luci-scheduler.appspot.com/jobs/perfetto")

  prompt(f"Please update the CHANGELOG: rename the 'Unreleased' entry to "
         f"'{version}'.")

  info("Building to test CHANGELOG parsing...")
  run_cmd('tools/ninja', '-C', 'out/linux_clang_release')

  info("Checking 'perfetto --version' output...")
  version_output = subprocess.check_output(
      ['out/linux_clang_release/perfetto', '--version']).decode('utf-8')
  info(f"Output: {version_output}")
  if version not in version_output:
    error(f"Version check failed. Expected '{version}' to be in the output.")
  info("Version check successful.")

  prompt("Please upload the CHANGELOG change for review and submit it on the "
         "main branch.")

  branch_name = f"releases/v{major_version}.x"
  info(f"Setting up release branch '{branch_name}'...")
  run_cmd('git', 'fetch', 'origin')

  info(f"Creating/updating remote branch '{branch_name}' from 'origin/main'...")
  run_cmd('git', 'push', 'origin', '--no-verify',
          f'origin/main:refs/heads/{branch_name}')
  run_cmd('git', 'fetch', 'origin')

  if get_current_branch() == branch_name:
    warn(f"Already on branch '{branch_name}'.")
  elif local_branch_exists(branch_name):
    warn(f"Local branch '{branch_name}' already exists. Checking it out.")
    run_cmd('git', 'checkout', branch_name)
  else:
    info(f"Creating and checking out local branch '{branch_name}'.")
    run_cmd('git', 'checkout', '-b', branch_name, '-t', f'origin/{branch_name}')

  info("Major release setup complete.")


def create_minor_release(version, major_version):
  info(f"--- Bumping to a new minor version: {version} ---")

  branch_name = f"releases/v{major_version}.x"
  info(f"Checking out branch '{branch_name}'...")
  run_cmd('git', 'fetch', 'origin')
  if get_current_branch() == branch_name:
    warn(f"Already on branch '{branch_name}'.")
  elif local_branch_exists(branch_name):
    warn(f"Local branch '{branch_name}' already exists. Checking it out.")
    run_cmd('git', 'checkout', branch_name)
  else:
    info(f"Creating and checking out local branch '{branch_name}'.")
    run_cmd('git', 'checkout', '-b', branch_name, '-t', f'origin/{branch_name}')

  prompt("Please merge or cherry-pick the desired commits for the new release.")

  prompt(f"Please update the CHANGELOG with a dedicated entry for '{version}'.")

  info("Minor release setup complete.")


def build_and_tag_release(version, major_version):
  info(f"--- Tagging the release: {version} ---")

  confirm("Have all changes for the release been merged into the release "
          "branch?")

  info("Pulling latest changes...")
  run_cmd('git', 'pull')

  info("Checking git status...")
  run_cmd('git', 'status')
  prompt("Please verify that your branch is up to date with "
         f"'origin/releases/v{major_version}.x' and has no divergence.")

  if tag_exists(version):
    warn(f"Tag '{version}' already exists locally.")
  else:
    run_cmd('git', 'tag', '-a', '-m', f'Perfetto {version}', version)

  if remote_tag_exists(version):
    warn(f"Tag '{version}' already exists on remote.")
  else:
    run_cmd('git', 'push', '--no-verify', 'origin', version)

  info("Build and tag complete.")


def create_github_release(version):
  info(
      f"--- Creating GitHub release with prebuilts and SDK sources: {version} ---"
  )

  prompt("Wait for LUCI builds to complete successfully: "
         "https://luci-scheduler.appspot.com/jobs/perfetto")

  info("Packaging prebuilts and SDK sources for GitHub release...")
  run_cmd('tools/release/package-github-release-artifacts', version)

  prompt(f"Please check that all 12 artifact zips are present in "
         f"'/tmp/perfetto-{version}-github-release/' "
         f"(10 prebuilt binaries + 2 SDK source zips).")

  prompt(f"""
    Please create a new GitHub release:
    1. Open https://github.com/google/perfetto/releases/new
    2. Choose Tag: {version}
    3. Release title: Perfetto {version}
    4. Describe release: Copy the CHANGELOG, wrapping it in triple backticks.
    5. Attach binaries: Attach all twelve .zip files from the previous step
       (10 prebuilt binaries + 2 SDK source zips).
    """)

  info("Rolling prebuilts...")
  run_cmd('tools/release/roll-prebuilts', version)
  prompt("Please upload the prebuilt roll CL for review.")

  info("Phew, you're done!")


def main():
  parser = argparse.ArgumentParser(
      description="Automates the Perfetto SDK release process.")
  parser.add_argument(
      'version',
      metavar='vX.Y',
      help="The new version number for the release (e.g., v16.0 or v16.1).")
  args = parser.parse_args()

  version = args.version
  match = re.match(r'v(\d+)\.(\d+)', version)
  if not match:
    error("Invalid version format. Please use 'vX.Y' (e.g., v16.0 or v16.1).")
    return

  major_version, minor_version = map(int, match.groups())

  if minor_version == 0:
    create_major_release(version, major_version)
  else:
    create_minor_release(version, major_version)

  build_and_tag_release(version, major_version)
  create_github_release(version)


if __name__ == '__main__':
  main()

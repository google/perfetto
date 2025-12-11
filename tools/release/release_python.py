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
import shutil
from typing import Optional

# Color codes
RED = '\033[0;31m'
GREEN = '\033[0;32m'
YELLOW = '\033[0;33m'
NC = '\033[0m'  # No Color

# Constants for paths. Assumes the script is run from the repository root.
SETUP_PY_PATH = os.path.join('python', 'setup.py')
VENV_PYTHON = (
    os.path.abspath(os.path.join('.venv', 'bin', 'python')) if sys.platform
    != 'win32' else os.path.join('.venv', 'Scripts', 'python.exe'))


def info(msg: str) -> None:
  print(f"{GREEN}INFO:{NC} {msg}")


def warn(msg: str) -> None:
  print(f"{YELLOW}WARN:{NC} {msg}")


def error(msg: str) -> None:
  print(f"{RED}ERROR:{NC} {msg}", file=sys.stderr)
  sys.exit(1)


def prompt(msg: str) -> str:
  return input(f"{YELLOW}ACTION:{NC} {msg}")


def confirm(msg: str) -> None:
  reply = input(f"{msg} [y/N] ").lower().strip()
  if reply not in ['y', 'yes']:
    error("Aborted by user.")


def run_cmd(*args: str, check: bool = True, cwd: Optional[str] = None) -> None:
  info(f"Running command: {' '.join(args)}")
  subprocess.run(args, check=check, cwd=cwd)


def check_git_clean() -> None:
  """Checks if the git working directory is clean."""
  info("Checking for clean git working directory...")
  try:
    subprocess.check_call(['git', 'diff', '--quiet'])
    subprocess.check_call(['git', 'diff', '--cached', '--quiet'])
  except subprocess.CalledProcessError:
    error("Git working directory is not clean. Please commit or stash changes.")
  info("Git working directory is clean.")


def get_current_branch() -> str:
  """Returns the current git branch."""
  return subprocess.check_output(['git', 'rev-parse', '--abbrev-ref',
                                  'HEAD']).decode('utf-8').strip()


def get_setup_py_content() -> str:
  with open(SETUP_PY_PATH, 'r') as f:
    return f.read()


def write_setup_py_content(content: str) -> None:
  with open(SETUP_PY_PATH, 'w') as f:
    f.write(content)


def get_current_version(content: str) -> str:
  """Extracts version from setup.py using a robust regex."""
  match = re.search(r"version\s*=\s*'([^']*)'", content)
  if not match:
    error(f"Could not find version in {SETUP_PY_PATH}")
    sys.exit(1)  # Unreachable, but satisfies type checker
  return match.group(1)


def bump_version() -> None:
  """Stage 1: Creates a commit with a bumped version number."""
  info("--- Stage 1: Bumping version ---")
  content = get_setup_py_content()
  current_version = get_current_version(content)
  info(f"Current version is {current_version}")

  new_version = prompt("Enter the new version (e.g., X.Y.Z): ")
  if not re.match(r'\d+\.\d+\.\d+', new_version):
    error("Invalid version format. Please use 'X.Y.Z'.")

  branch_name = prompt("Enter a name for the new release branch: ")
  run_cmd('git', 'checkout', '-b', branch_name)

  info(f"Updating version in {SETUP_PY_PATH} to {new_version}...")
  new_content = re.sub(r"version\s*=\s*'[^']*'", f"version='{new_version}'",
                       content)
  write_setup_py_content(new_content)

  run_cmd('git', 'add', SETUP_PY_PATH)
  run_cmd('git', 'commit', '-m',
          f'perfetto(python): Bump version to {new_version}')

  info(f"Version bump commit created on branch '{branch_name}'.")
  info("Please push this branch, create a pull request, and wait for it to "
       "be landed.")


def publish(commit: str) -> None:
  """Stage 2: Publishes the package and creates a commit to update the URL."""
  info(f"--- Stage 2: Publishing release for commit {commit} ---")

  original_branch = get_current_branch()
  info(f"Checking out commit {commit}...")
  run_cmd('git', 'checkout', commit)

  # Read the original content of setup.py at the release commit.
  content_at_commit = get_setup_py_content()
  new_version = get_current_version(content_at_commit)
  download_url = f"'https://github.com/google/perfetto/archive/{commit}.zip'"

  # Temporarily update download_url just for building the package.
  info(f"Temporarily setting download_url to {download_url}")
  temp_content = re.sub(r"download_url\s*=\s*'[^']*'",
                        f"download_url={download_url}", content_at_commit)
  write_setup_py_content(temp_content)

  try:
    info("Installing build dependencies into the virtual environment...")
    run_cmd(VENV_PYTHON, '-m', 'pip', 'install', 'build', 'twine')

    info("Building python package...")
    run_cmd(VENV_PYTHON, '-m', 'build', cwd='python')

    confirm("Ready to upload to PyPI. This is not reversible. Continue?")
    run_cmd(
        VENV_PYTHON,
        '-m',
        'twine',
        'upload',
        'dist/*',
        '--verbose',
        cwd='python')
    info("Successfully published to PyPI.")

  finally:
    # Always clean up and restore the repository to its original state.
    info("Cleaning up build artifacts...")
    shutil.rmtree('python/dist', ignore_errors=True)
    shutil.rmtree('python/perfetto.egg-info', ignore_errors=True)

    info(f"Restoring {SETUP_PY_PATH} to its state at commit {commit}...")
    write_setup_py_content(content_at_commit)

    info(f"Returning to original branch '{original_branch}'...")
    run_cmd('git', 'checkout', original_branch)

  # After successful publishing, create the final commit with the download_url.
  info("--- Creating final commit to update download_url ---")
  final_branch_name = prompt(
      "Enter a name for the final download_url update branch: ")
  run_cmd('git', 'checkout', '-b', final_branch_name)

  info(f"Updating download_url permanently in {SETUP_PY_PATH}...")
  final_content = get_setup_py_content()
  final_content = re.sub(r"download_url\s*=\s*'[^']*'",
                         f"download_url={download_url}", final_content)
  write_setup_py_content(final_content)

  run_cmd('git', 'add', SETUP_PY_PATH)
  run_cmd('git', 'commit', '-m',
          f'perfetto(python): Update download_url for v{new_version}')

  info(f"Commit for download_url created on branch '{final_branch_name}'.")
  info("Please push this branch and create a pull request to complete the "
       "release.")


def main() -> None:
  parser = argparse.ArgumentParser(
      description="Automates the Perfetto Python library release process.")
  parser.add_argument(
      '--bump-version',
      action='store_true',
      help="Stage 1: Bump version and create a release CL.")
  parser.add_argument(
      '--publish',
      action='store_true',
      help="Stage 2: Publish the release to PyPI.")
  parser.add_argument(
      '--commit',
      metavar='HASH',
      help="The landed commit hash of the version bump CL (for --publish).")

  args = parser.parse_args()

  # Ensure script is run from the repository root.
  if not os.path.exists(SETUP_PY_PATH):
    error(f"This script must be run from the root of the "
          f"perfetto repository.")

  # Ensure the virtual environment exists.
  if not os.path.exists(VENV_PYTHON):
    error(f"This script requires a virtual environment at '{VENV_PYTHON}'.")

  check_git_clean()

  if args.bump_version:
    if args.publish or args.commit:
      parser.error("--bump-version cannot be used with --publish or --commit.")
    bump_version()
  elif args.publish:
    if not args.commit:
      parser.error("--publish requires --commit.")
    publish(args.commit)
  else:
    parser.print_help()


if __name__ == '__main__':
  main()

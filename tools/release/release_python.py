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
CHANGELOG_PATH = 'CHANGELOG'
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


def version_from_changelog() -> str:
  """Returns the PyPI version, e.g. '0.56.0'.

  The version comes from the top 'vX.Y' entry of the CHANGELOG, same as
  python/setup.py.
  """
  with open(CHANGELOG_PATH) as f:
    for line in f:
      m = re.match(r'^v(\d+)[.](\d+)\s', line)
      if m:
        return '0.%s.%s' % (m.group(1), m.group(2))
  error(f"No vX.Y entry found in {CHANGELOG_PATH}")
  sys.exit(1)  # Unreachable, but satisfies type checker


def publish(commit: str) -> None:
  """Stage 2: Publishes the package and creates a commit to update the URL."""
  info(f"--- Stage 2: Publishing release for commit {commit} ---")

  original_branch = get_current_branch()
  info(f"Checking out commit {commit}...")
  run_cmd('git', 'checkout', commit)

  # setup.py content at the release commit, used below to rewrite download_url.
  content_at_commit = get_setup_py_content()
  new_version = version_from_changelog()
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
      description="Publishes the Perfetto Python library to PyPI. The version "
      "comes from the CHANGELOG, so there is nothing to bump.")
  parser.add_argument(
      '--publish',
      action='store_true',
      help="Publish the release to PyPI and create the download_url CL.")
  parser.add_argument(
      '--commit',
      metavar='HASH',
      help="The release commit to publish from, e.g. the vX.Y tag commit "
      "(for --publish).")

  args = parser.parse_args()

  # Ensure script is run from the repository root.
  if not os.path.exists(SETUP_PY_PATH):
    error(f"This script must be run from the root of the "
          f"perfetto repository.")

  # Ensure the virtual environment exists.
  if not os.path.exists(VENV_PYTHON):
    error(f"This script requires a virtual environment at '{VENV_PYTHON}'.")

  check_git_clean()

  if args.publish:
    if not args.commit:
      parser.error("--publish requires --commit.")
    publish(args.commit)
  else:
    parser.print_help()


if __name__ == '__main__':
  main()

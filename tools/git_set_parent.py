#!/usr/bin/env python3
import argparse
import sys
import os

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.tools.git_utils import (
    run_git_command,
    get_current_branch,
    get_all_branches,
    MAINLINE_BRANCHES,
)
#pylint: enable=wrong-import-position


def main():
  parser = argparse.ArgumentParser(
      description='Set parent branch config for target branch.')
  parser.add_argument('parent_branch', help='Name of the parent branch')
  parser.add_argument(
      '-t',
      '--target',
      metavar='branch_name',
      default=None,
      help='Target branch (default: current)')
  args = parser.parse_args()

  target_branch = args.target or get_current_branch()
  if not target_branch:
    print("Error: Cannot determine target branch.", file=sys.stderr)
    sys.exit(1)

  parent_branch = args.parent_branch

  all_branches = get_all_branches()
  if parent_branch not in all_branches and parent_branch not in MAINLINE_BRANCHES:
    print(f"Error: Branch '{parent_branch}' does not exist.", file=sys.stderr)
    sys.exit(1)

  run_git_command(['config', f'branch.{target_branch}.parent', parent_branch])
  print(f"Set parent of '{target_branch}' to '{parent_branch}'.")


if __name__ == "__main__":
  main()

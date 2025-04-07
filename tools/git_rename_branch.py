#!/usr/bin/env python3
import argparse
import sys
import os
from typing import List

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.tools.git_utils import (
    get_all_branches,
    get_branch_children,
    get_current_branch,
    run_git_command,
)
#pylint: enable=wrong-import-position


def main():
  parser = argparse.ArgumentParser(
      description='Renames local branch and updates parent config of its children (local only).',
      formatter_class=argparse.RawTextHelpFormatter)
  parser.add_argument(
      '-n',
      '--new',
      metavar='new_name',
      required=True,
      help='The desired new name for the branch.')
  parser.add_argument(
      '-t',
      '--target',
      metavar='old_name',
      default=None,
      help='Branch to rename (default: current branch).')
  args = parser.parse_args()

  new_name = args.new
  old_name = args.target or get_current_branch()

  if not old_name:
    print("Error: Cannot determine branch to rename.", file=sys.stderr)
    sys.exit(1)

  all_local_branches = get_all_branches()
  if old_name not in all_local_branches:
    print(f"Error: Branch '{old_name}' does not exist.", file=sys.stderr)
    sys.exit(1)
  if new_name == old_name:
    print(f"Error: New name is the same as the old name.", file=sys.stderr)
    sys.exit(1)
  if new_name in all_local_branches:
    print(f"Error: Branch '{new_name}' already exists.", file=sys.stderr)
    sys.exit(1)

  children = get_branch_children(old_name, all_local_branches)

  # Let run_git_command handle exit on failure for git operations
  print(f"Renaming local branch '{old_name}' to '{new_name}'...")
  run_git_command(['branch', '-m', old_name, new_name])
  print("Local rename successful.")

  if children:
    print(f"Updating parent config for children: {', '.join(children)}")
    for child in children:
      print(f" - Setting parent of '{child}' to '{new_name}'...")
      run_git_command(['config', f'branch.{child}.parent', new_name])

  print(f"\n--- Finished renaming to '{new_name}' ---")


if __name__ == "__main__":
  main()

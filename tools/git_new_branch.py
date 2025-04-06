#!/usr/bin/env python3
import argparse
import sys
import os

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.tools.git_utils import (
    get_current_branch,
    run_git_command,
)
#pylint: enable=wrong-import-position


def main():
  parser = argparse.ArgumentParser(
      description='Create new branch and set its parent config.')
  parser.add_argument('new_branch_name', help='Name for the new branch')
  parser.add_argument(
      '-p',
      '--parent',
      metavar='parent_branch',
      help='Parent branch (default: current)')
  args = parser.parse_args()

  parent_branch = args.parent
  if not parent_branch:
    parent_branch = get_current_branch()
    if not parent_branch:
      print("Error: Cannot determine default parent.", file=sys.stderr)
      sys.exit(1)
    print(f"Using current branch '{parent_branch}' as parent.")

  run_git_command(['checkout', '-b', args.new_branch_name, parent_branch])
  run_git_command(
      ['config', f'branch.{args.new_branch_name}.parent', parent_branch])
  print(f"Set parent of '{args.new_branch_name}' to '{parent_branch}'.")
  print(f"Created and checked out branch '{args.new_branch_name}'.")


if __name__ == "__main__":
  main()

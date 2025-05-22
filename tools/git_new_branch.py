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
      help='Parent branch (default: origin/main)')
  parser.add_argument(
      '--current-parent',
      help='Sets the parent branch to the current branch. '
      'Mutually exclusive with --parent.',
      action='store_true',
  )
  args = parser.parse_args()

  if args.current_parent and args.parent:
    print('Error: --current-parent and --parent are mutually exclusive.')
    return 1

  parent_branch = 'origin/main'
  if args.parent:
    parent_branch = args.parent
  if args.current_parent:
    parent_branch = get_current_branch()
    if not parent_branch:
      print('Error: Unable to determine the current branch.')
      return 1

  run_git_command(
      ['checkout', '--no-track', '-b', args.new_branch_name, parent_branch])
  run_git_command(
      ['config', f'branch.{args.new_branch_name}.parent', parent_branch])


if __name__ == "__main__":
  sys.exit(main())

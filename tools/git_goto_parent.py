#!/usr/bin/env python3
import argparse
import sys
import os

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.tools.git_utils import run_git_command, get_current_branch, get_branch_parent
#pylint: enable=wrong-import-position


def main():
  parser = argparse.ArgumentParser(
      description='Checks out the configured parent of target branch.')
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

  parent_branch = get_branch_parent(target_branch)
  if not parent_branch:
    print(
        f"Error: No parent configured for '{target_branch}'.", file=sys.stderr)
    sys.exit(1)

  print(f"Checking out parent '{parent_branch}'...")
  run_git_command(['checkout', parent_branch])


if __name__ == "__main__":
  main()

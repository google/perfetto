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
    get_branch_children,
    get_all_branches,
)
#pylint: enable=wrong-import-position


def main():
  parser = argparse.ArgumentParser(
      description='Finds and checks out a child branch of target branch.')
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

  all_local_branches = get_all_branches()
  children = get_branch_children(target_branch, all_local_branches)

  if not children:
    print(f"No child branches found for '{target_branch}'.")
    sys.exit(0)

  if len(children) == 1:
    child = children[0]
    print(f"Found one child: '{child}'. Checking out...")
    run_git_command(['checkout', child])
  else:
    print(f"Found multiple child branches for '{target_branch}':")
    for i, child in enumerate(children):
      print(f"  {i+1}) {child}")
    while True:  # Loop for interactive prompt
      try:
        choice = input(
            f"Select child number (1-{len(children)}), or 0 to cancel: ")
        choice_num = int(choice)
        if choice_num == 0:
          print("Cancelled.")
          sys.exit(0)
        elif 1 <= choice_num <= len(children):
          selected_child = children[choice_num - 1]
          print(f"Checking out '{selected_child}'...")
          run_git_command(['checkout', selected_child])
          break
        else:
          print(f"Invalid choice.")
      except ValueError:
        print("Invalid input.")
      except EOFError:
        print("\nCancelled.")
        sys.exit(1)


if __name__ == "__main__":
  main()

#!/usr/bin/env python3
import argparse
import json
import sys
from typing import Dict, Optional, Set, List, Deque
from collections import deque
import os

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.tools.git_utils import (
    get_current_branch,
    get_all_branches,
    get_stack_branches_ordered,
    get_branch_parent,
    get_upstream_branch_name,
    run_command,
    run_git_command,
)
#pylint: enable=wrong-import-position


def get_repo_default_branch() -> str:
  """Gets default branch name via 'gh'. Assumes CWD is repo."""
  try:
    result = run_command([
        'gh', 'repo', 'view', '--json', 'defaultBranchRef', '--jq',
        '.defaultBranchRef.name'
    ])
    default_branch = result.stdout.strip()
    if not default_branch:
      raise ValueError("gh returned empty default branch.")
    return default_branch
  except Exception as e:
    print(
        f"Warning: Could not get default branch via 'gh'. Assuming 'main'. Error: {e}",
        file=sys.stderr)
    return "main"


def get_existing_pr_info(branch_name: str) -> Optional[Dict]:
  """Checks for existing open PR via 'gh'. Returns PR info dict or None."""
  try:
    # check=False allows non-zero exit if PR not found
    result = run_command([
        'gh', 'pr', 'list', '--head', branch_name, '--state', 'open', '--limit',
        '1', '--json', 'number,baseRefName'
    ],
                         check=False)
    if result.returncode == 0 and result.stdout.strip():
      pr_list = json.loads(result.stdout)
      return pr_list[0] if pr_list else None
    return None
  except Exception as e:
    print(
        f"Warning: Failed check for existing PR for '{branch_name}'. Error: {e}",
        file=sys.stderr)
    return None


def main():
  parser = argparse.ArgumentParser(
      description='Syncs full stack (target + ancestors + descendants) with remote and GitHub PRs, using tracking branches.',
      formatter_class=argparse.RawTextHelpFormatter)
  parser.add_argument(
      '-t',
      '--target',
      metavar='branch_name',
      default=None,
      help='A branch within the stack (default: current)')
  parser.add_argument(
      '-r',
      '--remote',
      default='origin',
      help='Default remote repository name (default: origin)')
  parser.add_argument(
      '--draft', action='store_true', help='Create PRs as drafts')
  parser.add_argument(
      '-f',
      '--force',
      action='store_true',
      help='Use --force-with-lease when pushing')
  args = parser.parse_args()

  start_branch = args.target or get_current_branch()
  if not start_branch:
    print("Error: Cannot determine target branch.", file=sys.stderr)
    sys.exit(1)

  default_remote_name = args.remote
  repo_default_branch = get_repo_default_branch()
  mainline_branches = {repo_default_branch, 'main', 'master', 'develop'}

  all_local_branches = get_all_branches()
  if start_branch not in all_local_branches:
    print(f"Error: Target branch '{start_branch}' not local.", file=sys.stderr)
    sys.exit(1)

  branches_to_process: List[str] = []
  try:
    branches_to_process = get_stack_branches_ordered(start_branch,
                                                     mainline_branches,
                                                     all_local_branches)
  except ValueError as e:
    print(f"Error determining stack order: {e}", file=sys.stderr)
    sys.exit(1)

  if not branches_to_process:
    print("Could not determine stack branches. Exiting.", file=sys.stderr)
    sys.exit(1)
  print(f"Processing stack (parent-first): {', '.join(branches_to_process)}")

  errors_occurred = False
  for branch in branches_to_process:
    print(f"\n--- Processing: {branch} ---")

    local_parent = get_branch_parent(branch)
    desired_base = repo_default_branch
    if local_parent and local_parent not in mainline_branches:
      upstream_base_name = get_upstream_branch_name(local_parent)
      if upstream_base_name:
        desired_base = upstream_base_name
        print(f"PR base determined from parent's upstream: '{desired_base}'")
      else:
        print(
            f"Warning: Parent '{local_parent}' lacks upstream. Using default '{repo_default_branch}' as PR base.",
            file=sys.stderr)

    push_args: List[str] = ['push', '-u']
    branch_remote_result = run_git_command(
        ['config', f'branch.{branch}.remote'], check=False)
    push_remote = default_remote_name
    if branch_remote_result.returncode == 0 and branch_remote_result.stdout.strip(
    ):
      push_remote = branch_remote_result.stdout.strip()

    remote_branch_name = get_upstream_branch_name(branch)
    if remote_branch_name:
      refspec = f"{branch}:{remote_branch_name}"  # Push local to its tracking remote branch
    else:
      print(
          f"Warning: No upstream for '{branch}'. Pushing to '{push_remote}/{branch}'.",
          file=sys.stderr)
      refspec = f"{branch}:{branch}"  # Fallback: push local name to same remote name

    push_args.extend([push_remote, refspec])
    if args.force:
      push_args.append('--force-with-lease')

    try:
      print(f"Pushing {branch} ({refspec})...")
      run_git_command(push_args)
    except SystemExit:
      errors_occurred = True
      print(f"Error pushing {branch}.", file=sys.stderr)
      continue

    try:
      pr_info = get_existing_pr_info(branch)
      if pr_info:
        pr_number = pr_info.get('number')
        current_base = pr_info.get('baseRefName')
        print(f"Found existing PR #{pr_number} base '{current_base}'.")
        if current_base != desired_base:
          print(f"Updating PR base to '{desired_base}'...")
          run_command(
              ['gh', 'pr', 'edit',
               str(pr_number), '--base', desired_base])
      else:
        print(f"Creating PR with base '{desired_base}'...")
        create_command = [
            'gh', 'pr', 'create', '--head', branch, '--base', desired_base,
            '--fill'
        ]
        if args.draft:
          create_command.append('--draft')
        run_command(create_command)
    except SystemExit:
      errors_occurred = True
      print(f"Error managing PR for {branch} via 'gh'.", file=sys.stderr)
      continue
    except Exception as e:
      errors_occurred = True
      print(f"Unexpected error managing PR for {branch}: {e}", file=sys.stderr)
      continue

  print("\n--- Stack sync process finished ---")
  if errors_occurred:
    print("One or more errors occurred.", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
  main()

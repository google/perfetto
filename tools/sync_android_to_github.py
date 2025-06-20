#!/usr/bin/env python3
# Copyright (C) 2025`` The Android Open Source Project
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

import subprocess
import argparse
import sys
import os
import tempfile
import shutil  # For shutil.which

# --- Configuration ---
GITHUB_REMOTE = "origin"
INTERNAL_REMOTE = "goog"
GITHUB_MAIN_BRANCH_NAME = "main"  # e.g., main, master
INTERNAL_MAIN_BRANCH_NAME = "main"
BRANCH_PREFIX = "dev/"  # UPDATED PREFIX
GIT_NOTES_REF = "refs/notes/commits"


class Colors:
  HEADER = '\033[95m'
  OKBLUE = '\033[94m'
  OKCYAN = '\033[96m'
  OKGREEN = '\033[92m'
  WARNING = '\033[93m'
  FAIL = '\033[91m'
  ENDC = '\033[0m'
  BOLD = '\033[1m'
  UNDERLINE = '\033[4m'


def print_color(color_code, message):
  print(f"{color_code}{message}{Colors.ENDC}")


def run_command(command_list,
                capture_output=False,
                text=True,
                dry_run=False,
                is_modifying_command=False,
                check_exit_code=True,
                cwd=None,
                extra_env=None,
                ignore_error=False):
  command_str = ' '.join(command_list)
  effective_env = os.environ.copy()
  if extra_env:
    effective_env.update(extra_env)

  if dry_run and is_modifying_command:
    print_color(Colors.OKCYAN, f"DRY-RUN: Would execute: {command_str}")
    return subprocess.CompletedProcess(command_list, 0, stdout="", stderr="")

  if dry_run:
    print_color(Colors.OKCYAN, f"DRY-RUN (info): Executing: {command_str}")
  else:
    print_color(Colors.OKBLUE, f"Running: {command_str}")

  try:
    process = subprocess.run(
        command_list,
        capture_output=capture_output,
        text=text,
        check=check_exit_code and
        not ignore_error,  # only check if not ignoring errors
        cwd=cwd,
        env=effective_env)
    if ignore_error and process.returncode != 0:
      print_color(
          Colors.WARNING,
          f"Command returned non-zero exit code {process.returncode}, but error is ignored."
      )
    return process
  except subprocess.CalledProcessError as e:
    print_color(Colors.FAIL, f"ERROR running command: {command_str}")
    print_color(Colors.FAIL, f"  Return code: {e.returncode}")
    if e.stdout:
      print_color(Colors.FAIL, f"  Stdout: {e.stdout.strip()}")
    if e.stderr:
      print_color(Colors.FAIL, f"  Stderr: {e.stderr.strip()}")
    raise
  except FileNotFoundError:
    print_color(
        Colors.FAIL,
        f"ERROR: Command '{command_list[0]}' not found. Is it installed and in PATH?"
    )
    raise


def check_command_exists(command_name):
  if not shutil.which(command_name):
    print_color(
        Colors.FAIL,
        f"Required command '{command_name}' not found. Please install it and ensure it's in your PATH."
    )
    sys.exit(1)


def get_original_branch():
  try:
    proc = run_command(["git", "symbolic-ref", "--short", "HEAD"],
                       capture_output=True)
    return proc.stdout.strip()
  except subprocess.CalledProcessError:  # Detached HEAD
    proc = run_command(["git", "rev-parse", "--short", "HEAD"],
                       capture_output=True)
    return proc.stdout.strip()


def pre_flight_checks(dry_run):
  print_color(Colors.HEADER, "--- Running Pre-flight Checks ---")
  check_command_exists("git")
  check_command_exists("gh")

  try:
    run_command(["git", "rev-parse", "--is-inside-work-tree"],
                capture_output=True)
  except subprocess.CalledProcessError:
    print_color(Colors.FAIL, "ERROR: Not inside a Git repository.")
    sys.exit(1)

  for remote in [GITHUB_REMOTE, INTERNAL_REMOTE]:
    try:
      run_command(["git", "remote", "get-url", remote], capture_output=True)
    except subprocess.CalledProcessError:
      print_color(Colors.FAIL, f"ERROR: Git remote '{remote}' not found.")
      sys.exit(1)
  print_color(Colors.OKGREEN,
              f"✓ Remotes '{GITHUB_REMOTE}' and '{INTERNAL_REMOTE}' found.")

  try:
    run_command(["gh", "auth", "status"])  # No capture, just check auth
  except subprocess.CalledProcessError:
    print_color(
        Colors.FAIL,
        "ERROR: GitHub CLI ('gh') not authenticated. Please run 'gh auth login'."
    )
    sys.exit(1)
  print_color(Colors.OKGREEN, "✓ GitHub CLI ('gh') is authenticated.")

  git_status_proc = run_command(["git", "status", "--porcelain"],
                                capture_output=True)
  if git_status_proc.stdout.strip():
    print_color(
        Colors.WARNING,
        "Warning: Your Git working directory or staging area is not clean.")
    if not dry_run:
      if input("Proceed anyway? (y/N): ").lower() != 'y':
        print_color(Colors.FAIL, "Aborting due to unclean Git state.")
        sys.exit(1)
  else:
    print_color(Colors.OKGREEN, "✓ Git working directory is clean.")
  print_color(Colors.HEADER, "-------------------------------")


def fetch_updates(dry_run):
  print_color(
      Colors.HEADER,
      f"--- Fetching updates for remotes: {GITHUB_REMOTE}, {INTERNAL_REMOTE} ---"
  )
  try:
    run_command(
        ["git", "remote", "update", GITHUB_REMOTE, INTERNAL_REMOTE, "--prune"],
        dry_run=dry_run,
        is_modifying_command=True)  # Modifying local remote-tracking branches
    print_color(Colors.OKGREEN, "✓ Remotes updated.")
  except subprocess.CalledProcessError:
    print_color(Colors.FAIL, "ERROR: Failed to update remotes.")
    sys.exit(1)
  print_color(Colors.HEADER, "------------------------------------")


def fetch_notes(dry_run):
  print_color(Colors.HEADER,
              f"--- Fetching notes for remote {GITHUB_REMOTE} ---")
  try:
    run_command(
        ["git", "fetch", GITHUB_REMOTE, f"{GIT_NOTES_REF}:{GIT_NOTES_REF}"],
        dry_run=dry_run,
        is_modifying_command=True)  # Modifying local remote-tracking branches
    print_color(Colors.OKGREEN, "✓ Remotes updated.")
  except subprocess.CalledProcessError:
    print_color(Colors.FAIL, "ERROR: Failed to fetch notes.")
    sys.exit(1)
  print_color(Colors.HEADER, "------------------------------------")


def get_commit_details(commit_hash, dry_run):
  details_format = "%H%n%an%n%ae%n%ad%n%cn%n%ce%n%cd%n%s%n%b"  # Hash, Author Name, Author Email, Author Date, Committer Name, Committer Email, Committer Date, Subject, Body
  try:
    proc = run_command([
        "git", "show", "--quiet", f"--format=format:{details_format}",
        commit_hash
    ],
                       capture_output=True,
                       dry_run=dry_run,
                       is_modifying_command=False)
    parts = proc.stdout.strip().split(
        '\n', 8)  # Adjusted split for 8 newlines -> 9 parts
    return {
        "hash": parts[0],
        "author_name": parts[1],
        "author_email": parts[2],
        "author_date": parts[3],
        "committer_name": parts[4],
        "committer_email": parts[5],
        "committer_date": parts[6],
        "subject": parts[7],  # Subject is now a distinct part
        "body": parts[8] if len(parts) > 8 else ""  # Body is the last part
    }
  except Exception as e:
    print_color(Colors.FAIL,
                f"Could not get details for commit {commit_hash}: {e}")
    return None


def get_commits_to_port(dry_run):
  print_color(
      Colors.OKBLUE,
      f"Identifying commits in '{INTERNAL_REMOTE}/{INTERNAL_MAIN_BRANCH_NAME}' not noted or part of '{GITHUB_REMOTE}/{GITHUB_MAIN_BRANCH_NAME}'..."
  )
  rev_list_cmd = [
      "git", "rev-list", "--reverse", "--no-merges", "--right-only",
      "--cherry-pick",
      f"{GITHUB_REMOTE}/{GITHUB_MAIN_BRANCH_NAME}..{INTERNAL_REMOTE}/{INTERNAL_MAIN_BRANCH_NAME}"
  ]
  try:
    proc_rev_list = run_command(
        rev_list_cmd,
        capture_output=True,
        dry_run=dry_run,
        is_modifying_command=False)
  except subprocess.CalledProcessError:
    print_color(Colors.FAIL, "ERROR: Failed to list candidate commits.")
    return []

  potential_commits = [
      line for line in proc_rev_list.stdout.strip().split('\n') if line
  ]
  if not potential_commits:
    print_color(Colors.OKGREEN, "No new candidate commits found by rev-list.")
    return []

  commits_to_port_list = []
  for commit_hash in potential_commits:
    try:
      run_command(["git", "notes", "--ref", GIT_NOTES_REF, "show", commit_hash],
                  capture_output=True,
                  dry_run=dry_run,
                  is_modifying_command=False,
                  check_exit_code=True)  # Check for notes
      print_color(
          Colors.WARNING,
          f"Commit {commit_hash} already has a note on '{GIT_NOTES_REF}', skipping."
      )
    except subprocess.CalledProcessError:  # No note found, which is what we want
      commits_to_port_list.append(commit_hash)

  return commits_to_port_list


def main_loop(dry_run, original_branch):
  commits_to_port = get_commits_to_port(dry_run)

  if not commits_to_port:
    print_color(Colors.OKGREEN, "No commits to process.")
    return

  print_color(Colors.HEADER,
              f"Found {len(commits_to_port)} commit(s) to process:")
  for i, commit_hash in enumerate(commits_to_port):
    details = get_commit_details(commit_hash,
                                 dry_run)  # Get details for display
    if details:
      print_color(Colors.OKCYAN,
                  f"  {i+1}. {commit_hash[:10]} - {details['subject']}")
    else:
      print_color(
          Colors.OKCYAN,
          f"  {i+1}. {commit_hash[:10]} - (Could not fetch details for preview)"
      )
  print("")

  last_successful_local_branch_base = f"{GITHUB_REMOTE}/{GITHUB_MAIN_BRANCH_NAME}"
  last_successful_github_branch_for_pr_base = GITHUB_MAIN_BRANCH_NAME
  processed_commit_count = 0

  for internal_commit_hash in commits_to_port:
    print_color(
        Colors.HEADER,
        f"\n--- Processing internal commit: {internal_commit_hash} ---")
    details = get_commit_details(internal_commit_hash, dry_run)
    if not details:
      print_color(
          Colors.FAIL,
          f"Skipping commit {internal_commit_hash} due to inability to fetch details."
      )
      continue

    print_color(Colors.OKGREEN, "Commit Details:")
    print(f"  Hash:    {details['hash']}")
    print(f"  Author:  {details['author_name']} <{details['author_email']}>")
    print(f"  Date:    {details['author_date']}")
    print(f"  Subject: {details['subject']}")
    print(f"  Body:\n{details['body']}\n")

    while True:
      action = input(
          f"Process this commit? [{Colors.OKGREEN}Y{Colors.ENDC}]es/[{Colors.WARNING}S{Colors.ENDC}]kip/[{Colors.FAIL}Q{Colors.ENDC}]uit: "
      ).lower()
      if action in ['y', 's', 'q']:
        break
      print_color(Colors.FAIL, "Invalid input. Please enter Y, S, or Q.")

    if action == 'q':
      print_color(Colors.FAIL, "Quitting script as per user request.")
      return
    if action == 's':
      print_color(Colors.WARNING, f"Skipping commit {internal_commit_hash}.")
      continue

    short_hash = details['hash'][:10]
    new_local_branch_name = f"{BRANCH_PREFIX}{short_hash}"

    try:
      run_command(["git", "rev-parse", "--verify", new_local_branch_name],
                  capture_output=True,
                  dry_run=False,
                  is_modifying_command=False)
      print_color(Colors.WARNING,
                  f"Local branch '{new_local_branch_name}' already exists.")
      if not dry_run:
        if input(
            f"Delete local branch '{new_local_branch_name}' and recreate? (y/N): "
        ).lower() != 'y':
          print_color(
              Colors.WARNING,
              f"Skipping commit {internal_commit_hash} due to existing local branch."
          )
          continue
        run_command(["git", "branch", "-D", new_local_branch_name],
                    dry_run=dry_run,
                    is_modifying_command=True)
    except subprocess.CalledProcessError:
      pass

    print_color(
        Colors.OKBLUE,
        f"Creating new local branch '{new_local_branch_name}' based on '{last_successful_local_branch_base}'..."
    )
    try:
      run_command([
          "git", "checkout", "-b", new_local_branch_name,
          last_successful_local_branch_base
      ],
                  dry_run=dry_run,
                  is_modifying_command=True)
    except subprocess.CalledProcessError:
      print_color(
          Colors.FAIL,
          f"ERROR: Failed to create or checkout branch '{new_local_branch_name}'. Stopping."
      )
      return

    print_color(
        Colors.OKBLUE,
        f"Cherry-picking internal commit '{internal_commit_hash}' (using -x)..."
    )
    try:
      run_command(["git", "cherry-pick", "-x", internal_commit_hash],
                  dry_run=dry_run,
                  is_modifying_command=True)
    except subprocess.CalledProcessError:
      if dry_run:
        print_color(
            Colors.WARNING,
            "DRY-RUN: Cherry-pick would have been attempted. If it failed, user would be prompted to resolve."
        )
      else:
        print_color(
            Colors.FAIL,
            f"CHERRY-PICK FAILED for {internal_commit_hash} onto {new_local_branch_name}."
        )
        print_color(Colors.WARNING,
                    "Please resolve the conflicts in another terminal.")
        print_color(
            Colors.WARNING,
            "Steps: 1. Fix files. 2. 'git add <resolved_files>'. 3. 'git cherry-pick --continue'."
        )
        print_color(
            Colors.WARNING,
            "Alternatively, to give up on this commit: 'git cherry-pick --abort'."
        )

        while True:
          conflict_choice = input(
              "Type 'c' when resolved and continued, or 'a' to abort this cherry-pick: "
          ).lower()
          if conflict_choice == 'c':
            status_proc = run_command(["git", "status", "--porcelain"],
                                      capture_output=True,
                                      dry_run=False,
                                      is_modifying_command=False)
            if not ("U " in status_proc.stdout or "AA " in status_proc.stdout or
                    "AM" in status_proc.stdout or
                    "AU " in status_proc.stdout):  # Check for unmerged paths
              am_path = os.path.join(
                  run_command(["git", "rev-parse", "--git-dir"],
                              capture_output=True).stdout.strip(),
                  "CHERRY_PICK_HEAD")
              if not os.path.exists(
                  am_path):  # Check if CHERRY_PICK_HEAD is gone
                print_color(
                    Colors.OKGREEN,
                    "✓ Cherry-pick conflicts assumed resolved and continued.")
                break
            print_color(
                Colors.FAIL,
                "It seems the cherry-pick is still in progress or not properly continued."
            )
            print_color(
                Colors.FAIL,
                "Please ensure 'git cherry-pick --continue' was successful (no conflicts remaining)."
            )
          elif conflict_choice == 'a':
            print_color(Colors.WARNING,
                        f"Aborting cherry-pick for {internal_commit_hash}...")
            run_command(["git", "cherry-pick", "--abort"],
                        dry_run=False,
                        is_modifying_command=True)
            print_color(
                Colors.WARNING,
                f"Cherry-pick aborted. Cleaning up branch '{new_local_branch_name}'."
            )
            run_command(["git", "checkout", last_successful_local_branch_base],
                        dry_run=False,
                        is_modifying_command=True)
            run_command(["git", "branch", "-D", new_local_branch_name],
                        dry_run=False,
                        is_modifying_command=True)
            print_color(
                Colors.FAIL,
                "SCRIPT STOPPED due to aborted cherry-pick. The chain is broken."
            )
            return
          else:
            print_color(Colors.FAIL, "Invalid choice. Type 'c' or 'a'.")
    print_color(Colors.OKGREEN,
                f"✓ Cherry-pick successful for {internal_commit_hash}.")

    print_color(
        Colors.OKBLUE,
        f"Pushing new branch '{new_local_branch_name}' to '{GITHUB_REMOTE}'...")
    try:
      run_command([
          "git", "push", "--set-upstream", GITHUB_REMOTE, new_local_branch_name
      ],
                  dry_run=dry_run,
                  is_modifying_command=True)
    except subprocess.CalledProcessError:
      print_color(
          Colors.FAIL,
          f"ERROR: Failed to push branch '{new_local_branch_name}'. Stopping.")
      return
    print_color(Colors.OKGREEN, f"✓ Branch '{new_local_branch_name}' pushed.")

    pr_title = f"{details['subject']}"
    pr_body_content = f"""Port of internal commit.

**Original commit message:**
Subject: {details['subject']}

{details['body']}

---
Original internal commit hash: {details['hash']}
Cherry-picked with `-x`.
This PR is part of a stack. Base branch: {last_successful_github_branch_for_pr_base}
"""
    print_color(Colors.OKBLUE, "Creating Pull Request on GitHub...")
    print(f"  Title: {pr_title}")
    print(f"  Base:  {last_successful_github_branch_for_pr_base}")
    print(f"  Head:  {new_local_branch_name}")

    pr_url = ""
    try:
      existing_pr_proc = run_command([
          "gh", "pr", "list", "--head", new_local_branch_name, "--json", "url",
          "--jq", ".[0].url"
      ],
                                     capture_output=True,
                                     dry_run=dry_run,
                                     is_modifying_command=False,
                                     check_exit_code=False)
      if existing_pr_proc.returncode == 0 and existing_pr_proc.stdout.strip():
        pr_url = existing_pr_proc.stdout.strip()
        print_color(
            Colors.WARNING,
            f"PR already exists for head branch '{new_local_branch_name}': {pr_url}"
        )
      else:
        if dry_run:
          print_color(
              Colors.OKCYAN,
              f"DRY-RUN: Would execute: gh pr create --base {last_successful_github_branch_for_pr_base} --head {new_local_branch_name} ..."
          )
          pr_url = f"https://github.com/example/repo/pull/DRY_RUN_PR_NUM_FOR_{short_hash}"
        else:
          with tempfile.NamedTemporaryFile(
              mode="w", delete=False, prefix="pr_body_",
              suffix=".md") as tmp_body_file:
            tmp_body_file.write(pr_body_content)
            tmp_body_file_path = tmp_body_file.name
          try:
            pr_create_proc = run_command([
                "gh", "pr", "create", "--base",
                last_successful_github_branch_for_pr_base, "--head",
                new_local_branch_name, "--title", pr_title, "--body-file",
                tmp_body_file_path
            ],
                                         capture_output=True,
                                         dry_run=dry_run,
                                         is_modifying_command=True)
            pr_url = pr_create_proc.stdout.strip()
          finally:
            os.remove(tmp_body_file_path)
    except subprocess.CalledProcessError as e:
      print_color(
          Colors.FAIL,
          f"ERROR: Failed to create Pull Request using 'gh'. Gh output:\n{e.stderr or e.stdout}"
      )
      if not dry_run:
        print_color(Colors.FAIL, "Stopping.")
        return
    print_color(Colors.OKGREEN, f"✓ Pull Request created/verified: {pr_url}")

    print_color(
        Colors.OKBLUE,
        f"Adding git note for internal commit '{internal_commit_hash}'...")
    note_message = f"GitHub PR: {pr_url} (Branch: {new_local_branch_name})"
    try:
      run_command([
          "git", "notes", "--ref", GIT_NOTES_REF, "add", "-f", "-m",
          note_message, internal_commit_hash
      ],
                  dry_run=dry_run,
                  is_modifying_command=True)
      print_color(Colors.OKGREEN, f"✓ Note added to {internal_commit_hash}.")
    except subprocess.CalledProcessError:
      print_color(Colors.FAIL,
                  f"Error: Failed to add git note for {internal_commit_hash}.")

    last_successful_local_branch_base = new_local_branch_name
    last_successful_github_branch_for_pr_base = new_local_branch_name
    processed_commit_count += 1
    print_color(Colors.HEADER, f"--- End processing {internal_commit_hash} ---")

  if processed_commit_count > 0:
    print_color(Colors.HEADER, "\n--- Pushing all local git notes ---")
    try:
      run_command(["git", "push", GITHUB_REMOTE, GIT_NOTES_REF],
                  dry_run=dry_run,
                  is_modifying_command=True)
      print_color(Colors.OKGREEN, "✓ Git notes pushed successfully.")
    except subprocess.CalledProcessError:
      print_color(
          Colors.FAIL,
          f"ERROR: Failed to push git notes. You might need to do this manually: git push {GITHUB_REMOTE} {GIT_NOTES_REF}"
      )
  else:
    print_color(
        Colors.OKBLUE,
        "No commits were processed to create PRs, skipping notes push.")


def main():
  parser = argparse.ArgumentParser(
      description="Automate creation of stacked GitHub PRs from an internal branch."
  )
  parser.add_argument(
      "--dry-run",
      action="store_true",
      help="Print actions without executing them (read-only commands may still run for info)."
  )
  args = parser.parse_args()

  original_branch = get_original_branch()
  print_color(Colors.OKBLUE, f"Original branch/commit: {original_branch}")

  try:
    pre_flight_checks(args.dry_run)
    if not args.dry_run:
      fetch_updates(args.dry_run)
    else:
      print_color(Colors.OKCYAN, "DRY-RUN: Would fetch updates from remotes.")

    fetch_notes(args.dry_run)

    main_loop(args.dry_run, original_branch)

  except (subprocess.CalledProcessError, FileNotFoundError) as e:
    print_color(Colors.FAIL, f"A critical error occurred. Exiting script.")
  except Exception as e:
    print_color(Colors.FAIL, f"An unexpected error occurred: {e}")
    import traceback
    traceback.print_exc()
  finally:
    print_color(
        Colors.OKBLUE,
        f"\nAttempting to return to original branch/commit: {original_branch}..."
    )
    try:
      current_branch_after_script = get_original_branch()
      if current_branch_after_script != original_branch:
        run_command(["git", "checkout", original_branch],
                    dry_run=False,
                    is_modifying_command=True,
                    ignore_error=True)
        print_color(Colors.OKGREEN, f"✓ Switched back to {original_branch}.")
      else:
        print_color(Colors.OKGREEN, f"✓ Already on {original_branch}.")
    except Exception as e:
      print_color(
          Colors.WARNING,
          f"Could not switch back to {original_branch}: {e}. Current branch: {get_original_branch()}"
      )
    print_color(Colors.HEADER, "Script finished.")


if __name__ == "__main__":
  main()

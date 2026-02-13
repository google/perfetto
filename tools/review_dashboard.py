#!/usr/bin/env python3
"""
Interactive GitHub PR Review Dashboard
A Gerrit-like TUI for managing your pending code reviews.

Requirements:
  pip install textual requests
  gh auth login  # GitHub CLI must be authenticated
"""

import asyncio
import json
import subprocess
import sys
from datetime import datetime, timezone
from typing import List, Optional, Dict, Any
from dataclasses import dataclass, field
from concurrent.futures import ThreadPoolExecutor

try:
  from textual.app import App, ComposeResult
  from textual.containers import Container, Vertical, Horizontal, VerticalScroll
  from textual.widgets import Header, Footer, Static, ListView, ListItem, Label, RichLog, Input, TextArea
  from textual.binding import Binding
  from textual.reactive import reactive
  from textual.worker import Worker, WorkerState
  from textual.screen import Screen, ModalScreen
  from rich.text import Text
  from rich.panel import Panel
  from rich.table import Table
  from rich.syntax import Syntax
  from rich.markup import escape
except ImportError:
  print("Error: Required packages not installed.")
  print("Please run: pip install textual requests")
  sys.exit(1)


@dataclass
class Review:
  """Represents a review on a PR."""
  author: str
  state: str  # APPROVED, COMMENTED, CHANGES_REQUESTED
  submitted_at: str


@dataclass
class Comment:
  """Represents a comment on a PR."""
  author: str
  body: str
  created_at: str


@dataclass
class Commit:
  """Represents a commit in a PR."""
  sha: str
  author: str
  message: str
  committed_at: str


@dataclass
class PullRequest:
  """Represents a GitHub Pull Request."""
  number: int
  title: str
  author: str
  created_at: str
  updated_at: str
  url: str
  reviews: List[Review] = field(default_factory=list)
  comments: List[Comment] = field(default_factory=list)
  commits: List[Commit] = field(default_factory=list)
  needs_action: Optional[
      bool] = None  # None = loading, True = needs action, False = reviewed
  is_loading: bool = True


@dataclass
class FileChange:
  """Represents a changed file in a PR."""
  filename: str
  status: str  # added, modified, removed, renamed
  additions: int
  deletions: int
  changes: int
  patch: str
  previous_filename: Optional[str] = None


@dataclass
class DraftComment:
  """Represents a draft review comment."""
  path: str
  line: int
  body: str
  side: str = "RIGHT"  # RIGHT for new file, LEFT for old file


def run_gh_command(args: List[str], check: bool = True) -> str:
  """Run a gh CLI command and return the output."""
  try:
    result = subprocess.run(
        ["gh"] + args, capture_output=True, text=True, check=check)
    return result.stdout
  except subprocess.CalledProcessError as e:
    if check:
      print(f"Error running gh command: {e}")
      print(f"stderr: {e.stderr}")
      sys.exit(1)
    return ""
  except FileNotFoundError:
    print("Error: 'gh' command not found. Please install GitHub CLI:")
    print("  https://cli.github.com/")
    sys.exit(1)


async def run_gh_command_async(args: List[str],
                               executor: ThreadPoolExecutor,
                               check: bool = True) -> str:
  """Run a gh CLI command asynchronously."""
  loop = asyncio.get_event_loop()
  return await loop.run_in_executor(executor,
                                    lambda: run_gh_command(args, check))


def get_current_user() -> str:
  """Get the currently authenticated GitHub user."""
  output = run_gh_command(["api", "user", "--jq", ".login"])
  return output.strip()


def parse_timestamp(ts: str) -> datetime:
  """Parse ISO timestamp from GitHub API."""
  return datetime.fromisoformat(ts.replace('Z', '+00:00'))


def time_ago(ts: str) -> str:
  """Convert timestamp to human-readable 'X mins/hours/days ago' format."""
  dt = parse_timestamp(ts)
  now = datetime.now(timezone.utc)
  diff = now - dt

  seconds = diff.total_seconds()
  if seconds < 60:
    return "just now"
  elif seconds < 3600:
    mins = int(seconds / 60)
    return f"{mins} min{'s' if mins != 1 else ''} ago"
  elif seconds < 86400:
    hours = int(seconds / 3600)
    return f"{hours} hour{'s' if hours != 1 else ''} ago"
  else:
    days = int(seconds / 86400)
    return f"{days} day{'s' if days != 1 else ''} ago"


def fetch_pr_list(repo: str, username: str) -> List[PullRequest]:
  """Quickly fetch basic PR list where user is a reviewer."""
  # Get all open PRs with basic info in one query
  prs_json = run_gh_command([
      "api",
      f"repos/{repo}/pulls",
      "--paginate",
      "-X",
      "GET",
      "-f",
      "state=open",
  ])

  prs_data = json.loads(prs_json)
  prs = []

  for pr_data in prs_data:
    # Check if user is a requested reviewer
    requested_reviewers = [
        r["login"] for r in pr_data.get("requested_reviewers", [])
    ]

    # Also check requested teams (user might be in a team)
    if username in requested_reviewers:
      prs.append(
          PullRequest(
              number=pr_data["number"],
              title=pr_data["title"],
              author=pr_data["user"]["login"],
              created_at=pr_data["created_at"],
              updated_at=pr_data["updated_at"],
              url=pr_data["html_url"],
              is_loading=True,
              needs_action=None))

  # Also get PRs where user has already reviewed (search)
  search_result = run_gh_command([
      "api",
      "search/issues",
      "-X",
      "GET",
      "-f",
      f"q=is:pr is:open repo:{repo} reviewed-by:{username}",
  ],
                                 check=False)

  if search_result:
    search_data = json.loads(search_result)
    existing_numbers = {pr.number for pr in prs}

    for item in search_data.get("items", []):
      pr_num = item["number"]
      if pr_num not in existing_numbers:
        prs.append(
            PullRequest(
                number=pr_num,
                title=item["title"],
                author=item["user"]["login"],
                created_at=item["created_at"],
                updated_at=item["updated_at"],
                url=item["html_url"],
                is_loading=True,
                needs_action=None))

  # Sort by updated date
  prs.sort(key=lambda p: parse_timestamp(p.updated_at), reverse=True)

  return prs


def fetch_pr_files(repo: str, pr_number: int) -> List[FileChange]:
  """Fetch the list of changed files in a PR."""
  files_json = run_gh_command(
      ["api", f"repos/{repo}/pulls/{pr_number}/files", "--paginate"])

  files_data = json.loads(files_json)
  files = []

  for f in files_data:
    files.append(
        FileChange(
            filename=f["filename"],
            status=f["status"],
            additions=f["additions"],
            deletions=f["deletions"],
            changes=f["changes"],
            patch=f.get("patch", ""),
            previous_filename=f.get("previous_filename")))

  return files


async def fetch_pr_details(repo: str, pr: PullRequest, username: str,
                           executor: ThreadPoolExecutor) -> PullRequest:
  """Fetch detailed information for a PR asynchronously."""
  try:
    # Fetch reviews, commits, and comments in parallel
    reviews_task = run_gh_command_async(
        ["api", f"repos/{repo}/pulls/{pr.number}/reviews"], executor)

    commits_task = run_gh_command_async(
        ["api", f"repos/{repo}/pulls/{pr.number}/commits"], executor)

    comments_task = run_gh_command_async(
        ["api", f"repos/{repo}/issues/{pr.number}/comments"], executor)

    # Wait for all three to complete
    reviews_json, commits_json, comments_json = await asyncio.gather(
        reviews_task, commits_task, comments_task)

    reviews_data = json.loads(reviews_json)
    commits_data = json.loads(commits_json)
    comments_data = json.loads(comments_json)

    # Process commits
    pr.commits = [
        Commit(
            sha=c["sha"][:7],
            author=c["commit"]["author"]["name"],
            message=c["commit"]["message"].split('\n')[0],
            committed_at=c["commit"]["author"]["date"]) for c in commits_data
    ]

    # Process comments
    pr.comments = [
        Comment(
            author=c["user"]["login"],
            body=c["body"][:100] + "..." if len(c["body"]) > 100 else c["body"],
            created_at=c["created_at"]) for c in comments_data
    ]

    # Process reviews
    pr.reviews = [
        Review(
            author=r["user"]["login"],
            state=r["state"],
            submitted_at=r["submitted_at"]) for r in reviews_data
    ]

    # Determine if PR needs action
    user_reviews = [r for r in reviews_data if r["user"]["login"] == username]
    pr.needs_action = True

    if user_reviews:
      last_review = max(user_reviews, key=lambda r: r["submitted_at"])
      last_review_time = parse_timestamp(last_review["submitted_at"])

      # Check if there are commits or comments after last review
      latest_commit_time = max(
          parse_timestamp(c.committed_at)
          for c in pr.commits) if pr.commits else datetime.min.replace(
              tzinfo=timezone.utc)

      latest_comment_time = max(
          parse_timestamp(c.created_at)
          for c in pr.comments) if pr.comments else datetime.min.replace(
              tzinfo=timezone.utc)

      if latest_commit_time <= last_review_time and latest_comment_time <= last_review_time:
        pr.needs_action = False

    pr.is_loading = False

  except Exception as e:
    print(f"Error fetching details for PR #{pr.number}: {e}")
    pr.is_loading = False
    pr.needs_action = None

  return pr


class TimelineView(Static):
  """Widget to display PR timeline (commits, reviews, comments)."""

  def __init__(self, *args, **kwargs):
    super().__init__(*args, **kwargs)
    self.pr: Optional[PullRequest] = None

  def set_pr(self, pr: Optional[PullRequest]):
    """Update the timeline for a given PR."""
    self.pr = pr
    self.update_timeline()

  def update_timeline(self):
    """Render the timeline."""
    if not self.pr:
      self.update("Select a PR to view timeline")
      return

    if self.pr.is_loading:
      self.update("⏳ Loading timeline...")
      return

    # Collect all events
    events = []

    for commit in self.pr.commits:
      events.append({
          'type': 'commit',
          'time': commit.committed_at,
          'data': commit
      })

    for review in self.pr.reviews:
      events.append({
          'type': 'review',
          'time': review.submitted_at,
          'data': review
      })

    for comment in self.pr.comments:
      events.append({
          'type': 'comment',
          'time': comment.created_at,
          'data': comment
      })

    # Sort by time
    events.sort(key=lambda e: parse_timestamp(e['time']))

    # Build timeline table
    table = Table(show_header=True, box=None, expand=True)
    table.add_column("Time", style="cyan", width=15)
    table.add_column("Event", style="yellow", width=12)
    table.add_column("Author", style="green", width=20)
    table.add_column("Details", style="white")

    for event in events:
      time_str = time_ago(event['time'])

      if event['type'] == 'commit':
        c = event['data']
        table.add_row(time_str, "📝 Commit", c.author,
                      f"[dim]{c.sha}[/dim] {c.message}")
      elif event['type'] == 'review':
        r = event['data']
        emoji = {
            "APPROVED": "✅",
            "CHANGES_REQUESTED": "❌",
            "COMMENTED": "💬"
        }.get(r.state, "👁️")
        table.add_row(time_str, f"{emoji} {r.state.replace('_', ' ').title()}",
                      r.author, "")
      elif event['type'] == 'comment':
        c = event['data']
        table.add_row(time_str, "💬 Comment", c.author,
                      c.body.replace('\n', ' '))

    self.update(table)


class PRListItem(ListItem):
  """A list item representing a PR."""

  def __init__(self, pr: PullRequest, label: Label, *args, **kwargs):
    super().__init__(*args, **kwargs)
    self.pr = pr
    self.label = label

  def compose(self) -> ComposeResult:
    """Compose the list item with its label."""
    yield self.label


class FileListItem(ListItem):
  """A list item representing a changed file."""

  def __init__(self, file: FileChange, *args, **kwargs):
    super().__init__(*args, **kwargs)
    self.file = file

  def compose(self) -> ComposeResult:
    """Compose the file list item."""
    status_emoji = {
        "added": "✨",
        "modified": "📝",
        "removed": "🗑️",
        "renamed": "📋"
    }.get(self.file.status, "❓")

    label = Label(
        f"{status_emoji} {self.file.filename} [green]+{self.file.additions}[/green] [red]-{self.file.deletions}[/red]"
    )
    yield label


@dataclass
class DiffLine:
  """Represents a line in a side-by-side diff."""
  old_num: Optional[int]  # Line number in old file (None if added)
  new_num: Optional[int]  # Line number in new file (None if deleted)
  old_text: str  # Text from old file
  new_text: str  # Text from new file
  line_type: str  # 'add', 'delete', 'context', 'header'


class DiffViewer(Static):
  """Widget to display side-by-side diff."""

  def __init__(self, *args, **kwargs):
    super().__init__(*args, **kwargs)
    self.file: Optional[FileChange] = None
    self.current_line = 0
    self.diff_lines: List[DiffLine] = []
    self.comments: Dict[int, str] = {}  # line -> comment
    self.can_focus = True

  def set_file(self, file: Optional[FileChange]):
    """Set the file to display."""
    self.file = file
    self.current_line = 0
    self.parse_and_render_diff()

  def parse_diff(self, patch: str) -> List[DiffLine]:
    """Parse unified diff into side-by-side line pairs."""
    lines = patch.split('\n')
    diff_lines = []
    old_line_num = 0
    new_line_num = 0

    for line in lines:
      if line.startswith('@@'):
        # Parse hunk header: @@ -old_start,old_count +new_start,new_count @@
        parts = line.split()
        if len(parts) >= 3:
          old_info = parts[1][1:]  # Remove '-'
          new_info = parts[2][1:]  # Remove '+'
          old_line_num = int(old_info.split(',')[0]) - 1
          new_line_num = int(new_info.split(',')[0]) - 1

        diff_lines.append(
            DiffLine(
                old_num=None,
                new_num=None,
                old_text=line,
                new_text="",
                line_type='header'))
      elif line.startswith('-'):
        # Deleted line (only in old file)
        old_line_num += 1
        diff_lines.append(
            DiffLine(
                old_num=old_line_num,
                new_num=None,
                old_text=line[1:],  # Remove '-' prefix
                new_text="",
                line_type='delete'))
      elif line.startswith('+'):
        # Added line (only in new file)
        new_line_num += 1
        diff_lines.append(
            DiffLine(
                old_num=None,
                new_num=new_line_num,
                old_text="",
                new_text=line[1:],  # Remove '+' prefix
                line_type='add'))
      elif line.startswith(' '):
        # Context line (in both files)
        old_line_num += 1
        new_line_num += 1
        content = line[1:]  # Remove ' ' prefix
        diff_lines.append(
            DiffLine(
                old_num=old_line_num,
                new_num=new_line_num,
                old_text=content,
                new_text=content,
                line_type='context'))

    return diff_lines

  def parse_and_render_diff(self):
    """Parse and render the diff for the current file."""
    if not self.file or not self.file.patch:
      self.update("No diff available")
      return

    self.diff_lines = self.parse_diff(self.file.patch)
    self.render_diff()

  def render_diff(self):
    """Render the side-by-side diff."""
    if not self.diff_lines:
      self.update("No diff available")
      return

    # Build the diff table for ALL lines
    table = Table(show_header=True, box=None, expand=True, padding=0)
    table.add_column("Old", style="white", width=None, ratio=1)
    table.add_column("│", style="dim", width=1)
    table.add_column("New", style="white", width=None, ratio=1)

    for i, diff_line in enumerate(self.diff_lines):
      is_highlighted = (i == self.current_line)
      comment_marker = "💬 " if i in self.comments else ""

      # Format line numbers and content
      if diff_line.line_type == 'header':
        # Header spans both columns
        old_cell = f"[cyan]{escape(diff_line.old_text)}[/cyan]"
        new_cell = ""
      elif diff_line.line_type == 'delete':
        # Deletion: show in left column only
        old_num_str = f"{diff_line.old_num:4d}" if diff_line.old_num else "    "
        old_cell = f"[red]{old_num_str} {escape(diff_line.old_text)}[/red]"
        new_cell = "[dim]    [/dim]"
      elif diff_line.line_type == 'add':
        # Addition: show in right column only
        new_num_str = f"{diff_line.new_num:4d}" if diff_line.new_num else "    "
        old_cell = "[dim]    [/dim]"
        new_cell = f"[green]{new_num_str} {escape(diff_line.new_text)}[/green]"
      else:  # context
        # Context: show in both columns
        old_num_str = f"{diff_line.old_num:4d}" if diff_line.old_num else "    "
        new_num_str = f"{diff_line.new_num:4d}" if diff_line.new_num else "    "
        old_cell = f"{old_num_str} {escape(diff_line.old_text)}"
        new_cell = f"{new_num_str} {escape(diff_line.new_text)}"

      # Add highlighting for current line
      if is_highlighted:
        old_cell = f"[reverse]{comment_marker}{old_cell}[/reverse]"
        new_cell = f"[reverse]{new_cell}[/reverse]"
      elif comment_marker:
        old_cell = f"{comment_marker}{old_cell}"

      table.add_row(old_cell, "│", new_cell)

    self.update(table)

  def move_line(self, delta: int):
    """Move the current line cursor and auto-scroll."""
    if not self.diff_lines:
      return

    # Update current line
    old_line = self.current_line
    self.current_line = max(
        0, min(len(self.diff_lines) - 1, self.current_line + delta))

    # Re-render to show new highlight
    self.render_diff()

    # Scroll to keep current line visible
    # Use Textual's scroll_to_region to scroll the highlighted line into view
    if self.current_line != old_line and hasattr(self, 'scroll_to_region'):
      # Each line in the table is approximately 1 line high
      # Add 1 for the header row
      line_y = self.current_line + 1
      # Try to keep the line centered in the viewport by scrolling to it
      try:
        # scroll_visible ensures the region is visible
        from textual.geometry import Region
        self.scroll_to_region(
            Region(0, line_y, self.size.width, 1), animate=False)
      except:
        # Fallback: just scroll to approximate y position
        try:
          self.scroll_to(y=line_y, animate=False)
        except:
          pass  # If scrolling fails, at least we updated the highlight

  def add_comment(self, comment: str):
    """Add a comment at the current line."""
    if self.current_line >= 0:
      self.comments[self.current_line] = comment
      self.render_diff()


class CommentDialog(ModalScreen[str]):
  """Modal dialog for adding a comment."""

  BINDINGS = [
      Binding("escape", "cancel", "Cancel"),
      Binding("ctrl+s", "save", "Save Comment"),
  ]

  def compose(self) -> ComposeResult:
    """Compose the dialog."""
    with Vertical():
      yield Label("Enter your comment (Ctrl+S to save, Esc to cancel):")
      yield TextArea(id="comment-input")

  def action_save(self):
    """Save the comment."""
    textarea = self.query_one("#comment-input", TextArea)
    self.dismiss(textarea.text)

  def action_cancel(self):
    """Cancel the dialog."""
    self.dismiss("")


class ReviewScreen(Screen):
  """Screen for reviewing a PR."""

  BINDINGS = [
      Binding("escape", "exit_review", "Exit Review"),
      Binding("j", "line_down", "Next Line"),
      Binding("k", "line_up", "Previous Line"),
      Binding("c", "add_comment", "Add Comment"),
      Binding("s", "submit_review", "Submit Review"),
  ]

  CSS = """
    ReviewScreen {
        layout: grid;
        grid-size: 1 3;
        grid-rows: 1fr 3fr 5;
    }

    #file-list {
        height: 100%;
        border: solid yellow;
    }

    #diff-viewer {
        height: 100%;
        border: solid blue;
        padding: 1;
        overflow-y: auto;
    }

    #review-status {
        height: 5;
        border: solid green;
        padding: 1;
    }
    """

  def __init__(self, repo: str, pr: PullRequest, *args, **kwargs):
    super().__init__(*args, **kwargs)
    self.repo = repo
    self.pr = pr
    self.files: List[FileChange] = []
    self.draft_comments: List[DraftComment] = []

  def compose(self) -> ComposeResult:
    """Compose the review screen."""
    yield Header()

    # File list
    file_list = ListView(id="file-list")
    file_list.border_title = f"Files in PR #{self.pr.number}"
    yield file_list

    # Diff viewer
    diff_viewer = DiffViewer(id="diff-viewer")
    diff_viewer.border_title = "Diff"
    yield diff_viewer

    # Review status
    status = Static(id="review-status")
    status.border_title = "Review Status"
    yield status

    yield Footer()

  def on_mount(self):
    """Load files when mounted."""
    self.load_files()

  def load_files(self):
    """Load the list of changed files."""
    self.files = fetch_pr_files(self.repo, self.pr.number)

    file_list = self.query_one("#file-list", ListView)
    for file in self.files:
      file_list.append(FileListItem(file))

    self.update_status()

  def on_list_view_highlighted(self, event: ListView.Highlighted):
    """Handle file selection."""
    if isinstance(event.item, FileListItem):
      diff_viewer = self.query_one("#diff-viewer", DiffViewer)
      diff_viewer.set_file(event.item.file)
      diff_viewer.border_title = f"Diff: {event.item.file.filename}"

  def action_line_down(self):
    """Move to next line in diff."""
    diff_viewer = self.query_one("#diff-viewer", DiffViewer)
    diff_viewer.move_line(1)

  def action_line_up(self):
    """Move to previous line in diff."""
    diff_viewer = self.query_one("#diff-viewer", DiffViewer)
    diff_viewer.move_line(-1)

  def action_add_comment(self):
    """Add a comment at the current line."""
    diff_viewer = self.query_one("#diff-viewer", DiffViewer)

    if not diff_viewer.file:
      return

    def handle_comment(comment: str):
      """Handle the comment result from the dialog."""
      if comment:
        diff_viewer.add_comment(comment)
        # Store draft comment
        self.draft_comments.append(
            DraftComment(
                path=diff_viewer.file.filename,
                line=diff_viewer.current_line,
                body=comment))
        self.update_status()

    # Show comment dialog with callback
    self.app.push_screen(CommentDialog(), handle_comment)

  def update_status(self):
    """Update the review status display."""
    status = self.query_one("#review-status", Static)
    comment_count = len(self.draft_comments)
    file_count = len(self.files)

    status.update(
        f"📁 {file_count} files | 💬 {comment_count} draft comments | "
        f"[yellow]'c'[/yellow]=comment [yellow]'s'[/yellow]=submit [yellow]'esc'[/yellow]=exit"
    )

  async def action_submit_review(self):
    """Submit the review."""
    # For now, just show what would be submitted
    if not self.draft_comments:
      self.notify("No comments to submit", severity="warning")
      return

    # TODO: Implement actual review submission via GitHub API
    self.notify(
        f"Would submit {len(self.draft_comments)} comments",
        severity="information")

  def action_exit_review(self):
    """Exit review mode."""
    self.app.pop_screen()


class ReviewDashboard(App):
  """Interactive GitHub PR Review Dashboard."""

  CSS = """
    Screen {
        layout: grid;
        grid-size: 1 3;
        grid-rows: 1fr 2fr 4;
    }

    #pr-list {
        height: 100%;
        border: solid green;
    }

    #timeline {
        height: 100%;
        border: solid blue;
        padding: 1;
    }

    #debug-log {
        height: 4;
        border: solid magenta;
        padding: 0 1;
    }

    ListView {
        height: 100%;
    }

    .needs-action {
        color: yellow;
    }

    .reviewed {
        color: green;
    }

    .loading {
        color: cyan;
    }
    """

  BINDINGS = [
      Binding("q", "quit", "Quit", priority=True),
      Binding("r", "refresh", "Refresh", priority=True),
      Binding("o", "open", "Open in Browser", priority=True),
      Binding("enter", "review", "Review PR", priority=True),
      Binding("v", "review", "Review PR", priority=True),
  ]

  def __init__(self, repo: str, username: str, prs: List[PullRequest]):
    super().__init__()
    self.repo = repo
    self.username = username
    self.prs = prs
    self.title = f"GitHub Review Dashboard - {repo}"
    self.sub_title = f"Logged in as: {username}"
    self.executor = ThreadPoolExecutor(max_workers=10)
    self.pr_items: Dict[int, PRListItem] = {}

  def compose(self) -> ComposeResult:
    """Build the UI."""
    yield Header()

    # PR List - create items but don't add yet
    for pr in self.prs:
      label = self._create_pr_label(pr)
      item = PRListItem(pr, label)
      self.pr_items[pr.number] = item

    # Yield ListView - items will be added in on_mount
    yield ListView(id="pr-list")

    # Timeline
    timeline = TimelineView(id="timeline")
    timeline.border_title = "Timeline"
    if self.prs:
      timeline.set_pr(self.prs[0])
    yield timeline

    # Debug log
    log = RichLog(id="debug-log", highlight=True, markup=True)
    log.border_title = "Debug Log"
    yield log

    yield Footer()

  def debug_log(self, message: str):
    """Write a message to the debug log."""
    try:
      log_widget = self.query_one("#debug-log", RichLog)
      log_widget.write(message)
    except:
      pass

  def _create_pr_label(self, pr: PullRequest) -> Label:
    """Create a label for a PR based on its state."""
    if pr.is_loading:
      status = "⏳ Loading..."
      style = "loading"
    elif pr.needs_action is None:
      status = "❓ Unknown"
      style = "loading"
    elif pr.needs_action:
      status = "🔴 NEEDS REVIEW"
      style = "needs-action"
    else:
      status = "✅ Reviewed"
      style = "reviewed"

    label = Label(
        f"#{pr.number} {status} | @{pr.author} | {pr.title[:60]} | {time_ago(pr.updated_at)}"
    )
    label.add_class(style)
    return label

  def on_mount(self) -> None:
    """Start loading PR details asynchronously when app starts."""
    self.debug_log("🚀 Starting up...")

    # Add items to ListView now that it's mounted
    pr_list = self.query_one("#pr-list", ListView)
    pr_list.border_title = f"Pull Requests ({len(self.prs)})"

    for pr in self.prs:
      if pr.number in self.pr_items:
        pr_list.append(self.pr_items[pr.number])

    self.debug_log(f"📋 Loaded {len(self.prs)} PRs to UI")

    # Start loading details using Textual's worker system
    self.debug_log(f"⏳ Starting to fetch details for {len(self.prs)} PRs...")
    self.load_pr_details()

  def load_pr_details(self) -> None:
    """Load details for all PRs asynchronously."""
    # Use run_worker to run async function in Textual's event loop
    self.run_worker(self._load_all_prs(), exclusive=False)

  async def _load_all_prs(self) -> None:
    """Worker function to load all PR details."""
    tasks = [
        fetch_pr_details(self.repo, pr, self.username, self.executor)
        for pr in self.prs
    ]

    self.debug_log(f"🔄 Created {len(tasks)} fetch tasks")

    # Process PRs as they complete (not in order)
    completed = 0
    for coro in asyncio.as_completed(tasks):
      try:
        pr = await coro
        completed += 1
        self.debug_log(f"✓ PR #{pr.number} loaded ({completed}/{len(tasks)})")
        self.update_pr_display(pr)
      except Exception as e:
        self.debug_log(f"❌ Error loading PR: {e}")
        completed += 1

  def update_pr_display(self, pr: PullRequest) -> None:
    """Update the display for a PR after its details have loaded."""
    if pr.number in self.pr_items:
      item = self.pr_items[pr.number]

      # Update the label content and style
      if pr.is_loading:
        status = "⏳ Loading..."
        style = "loading"
      elif pr.needs_action is None:
        status = "❓ Unknown"
        style = "loading"
      elif pr.needs_action:
        status = "🔴 NEEDS REVIEW"
        style = "needs-action"
      else:
        status = "✅ Reviewed"
        style = "reviewed"

      self.debug_log(f"🔄 Updating PR #{pr.number}: {status}")

      # Update label text and style
      item.label.update(
          f"#{pr.number} {status} | @{pr.author} | {pr.title[:60]} | {time_ago(pr.updated_at)}"
      )
      item.label.remove_class("loading", "needs-action", "reviewed")
      item.label.add_class(style)

      # Update timeline if this PR is currently selected
      pr_list = self.query_one("#pr-list", ListView)
      if pr_list.highlighted_child == item:
        timeline = self.query_one("#timeline", TimelineView)
        timeline.set_pr(pr)

      # Check if all loaded, then resort once
      if all(not pr.is_loading for pr in self.prs):
        self.resort_prs()

  def resort_prs(self) -> None:
    """Re-sort PRs after details are loaded."""
    self.debug_log("📊 All PRs loaded, re-sorting...")

    # Sort: PRs needing action first, then by updated date
    self.prs.sort(
        key=lambda p: (
            p.needs_action is True,  # True first, then False/None
            parse_timestamp(p.updated_at)),
        reverse=True)

    # Get the list view
    pr_list = self.query_one("#pr-list", ListView)

    # Remove all old items
    pr_list.clear()

    # Create fresh items in sorted order and add them
    self.pr_items = {}

    for pr in self.prs:
      # Create a fresh label for the sorted list
      label = self._create_pr_label(pr)
      new_item = PRListItem(pr, label)
      self.pr_items[pr.number] = new_item
      pr_list.append(new_item)

    # Update count
    needs_action = sum(1 for pr in self.prs if pr.needs_action is True)
    pr_list.border_title = f"Pull Requests ({len(self.prs)}) - {needs_action} need action"
    self.debug_log(f"✅ Done! {needs_action} PRs need action")

  def on_list_view_highlighted(self, event: ListView.Highlighted) -> None:
    """Handle PR selection."""
    if isinstance(event.item, PRListItem):
      timeline = self.query_one("#timeline", TimelineView)
      timeline.set_pr(event.item.pr)
      timeline.border_title = f"Timeline - PR #{event.item.pr.number}"

  def action_open(self) -> None:
    """Open the selected PR in browser."""
    pr_list = self.query_one("#pr-list", ListView)
    if pr_list.highlighted_child and isinstance(pr_list.highlighted_child,
                                                PRListItem):
      pr = pr_list.highlighted_child.pr
      subprocess.run(["open", pr.url] if sys.platform ==
                     "darwin" else ["xdg-open", pr.url])

  def action_review(self) -> None:
    """Enter review mode for the selected PR."""
    pr_list = self.query_one("#pr-list", ListView)
    if pr_list.highlighted_child and isinstance(pr_list.highlighted_child,
                                                PRListItem):
      pr = pr_list.highlighted_child.pr
      self.push_screen(ReviewScreen(self.repo, pr))

  def action_refresh(self) -> None:
    """Refresh the PR list."""
    self.exit(message="refresh")


def main():
  """Main entry point."""
  import argparse

  parser = argparse.ArgumentParser(
      description="Interactive GitHub PR Review Dashboard")
  parser.add_argument(
      "--repo",
      default="google/perfetto",
      help="GitHub repository (default: google/perfetto)")

  args = parser.parse_args()

  print("🔍 Fetching your pending reviews...")
  username = get_current_user()
  print(f"📝 Logged in as: {username}")

  prs = fetch_pr_list(args.repo, username)

  if not prs:
    print(f"✨ No pending reviews found for {username} in {args.repo}")
    return

  print(f"📊 Found {len(prs)} PRs (loading details...)")
  print()

  while True:
    app = ReviewDashboard(args.repo, username, prs)
    result = app.run()

    if result == "refresh":
      print("🔄 Refreshing...")
      prs = fetch_pr_list(args.repo, username)
    else:
      break


if __name__ == "__main__":
  main()

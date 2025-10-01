# Perfetto RFCs

This repository is set up to automatically manage RFCs (Request for Comments)
with GitHub Discussions integration.

**TL;DR**

- Directly push changes to the `rfcs` branch without any review.
- Any commit will trigger a creation/update of a matching entry in the
  GitHub Discussions page.
- Hold the discussion in the GitHub Discussions entry.
- Use GitHub permalink to quote inline snippets of the doc.

We are deliberately lax on the definition of RFC to keep the process lax and
have a low-barrier process to both document and discuss design decisions of any
size and controversy level.

## 📋 Overview

When you push or update an RFC markdown file, the system automatically:

- 📝 Creates/updates a corresponding GitHub Discussion
- 🔗 Links the discussion back to the source file
- 💬 Enables collaborative feedback through discussions

## 📝 Creating an RFC

### File Naming Convention

RFC files must follow this naming pattern: `<RFC_NUMBER>-<title-with-hyphens>.md`

Examples:

- `0001-buffer.md`
- `0002-ui-taskbar.md`
- `0123-authentication-strategy.md`

**Important:** The RFC number must be exactly 4 digits (zero-padded).

### Directory Structure

```txt
repository-root/
├── 0001-buffer.md
├── 0002-ui-taskbar.md
├── media/
│   ├── 0001/
│   │   ├── buffer1.png
│   │   └── buffer2.svg
│   └── 0002/
│       └── ui-screenshot1.png
├── .github/
│   ├── workflows/
│   │   └── rfc-automation.yml
│   └── scripts/
│       └── sync_rfc_to_discussion.py
└── .markdownlint.json
```

### Media Files

All images (SVG or PNG) should be placed in `/media/<RFC_NUMBER>/`:

- ✅ `/media/0001/diagram.png`
- ✅ `/media/0001/architecture.svg`
- ✅ `/media/0002/screenshot.png`
- ❌ `/images/diagram.png` (wrong location)
- ❌ `/media/diagram.png` (missing RFC number directory)

You are strongly encouraged to use draw.io / diagrams.net for your diagrams
as it is able to embed metadata in both svg and png files, allowing for
later re-editing.

If you use VSCode, you might find these two extensions particularly useful:

1. [Draw.io integration](https://marketplace.visualstudio.com/items?itemName=hediet.vscode-drawio)
particularly useful, as it allows to create diagrams directly within VSCode
simply by creating a new empy `xxx.drawio.{svg,png}` file.

2. [markdownlint](https://marketplace.visualstudio.com/items?itemName=DavidAnson.vscode-markdownlint)

## 🚀 Workflow

1. **Checkout/create the rfcs branch:**

    ```bash
    git checkout -b rfcs -t origin/rfcs
    ```

2. **Create your RFC:**: Copy the rfc-template.md

3. **Add content:**
   Edit your RFC file with your markdown content. Include:
   - Problem statement
   - Proposed solution
   - Alternatives considered
   - Implementation details
   - References to media files if needed

4. **Add media files (optional):**

   ```bash
   mkdir -p media/0001
   touch media/0001/diagram.drawio.png
   # Edit with vscode or app.diagrams.net
   ```

5. **Commit and push:**

   ```bash
   git add 0001-buffer.md media/0001/
   git commit -m "Add RFC-0001: Buffer Management"
   git push
   ```

6. **Automated actions:**
   - Markdown linter runs automatically
   - GitHub Discussion is created/updated
   - Team can comment and provide feedback

## 💬 GitHub Discussions

Each RFC automatically gets a GitHub Discussion created in the repository.
The discussion:

- Contains the full RFC content
- Links back to the source file
- Updates when the RFC file is modified
- Serves as the central place for team feedback

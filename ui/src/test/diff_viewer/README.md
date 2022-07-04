# CI screenshot diff viewer

This directory contains the source of screenshots diff viewer used on Perfetto
CI. The way it works as follows:

When a screenshot test is failing, the testing code will write a line of the
form

```
failed-screenshot.png;failed-screenshot-diff.png
```

To a file called `report.txt`. Diff viewer is just a static page that uses Fetch
API to download this file, parse it, and display images in a list of rows.

The page assumes `report.txt` to be present in the same directory, same goes for
screenshot files. To simplify deployment, the viewer is developed without a
framework and constructs DOM using `document.createElement` API.

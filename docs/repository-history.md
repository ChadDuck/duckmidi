# How repository history works

Every exporter with **Export to Repository** shares one history runtime. It records what each export changed so you can revert it, even after a restart. This page explains how it stores and protects your files. To use it, see [Repository history](user-guide.md#repository-history) in the user guide.

The runtime is the `db` library in the plugins folder (`db.dll`, `db.so` or `db.dylib`), which bundles SQLite 3.50.4, yyjson 0.12.0 and pugixml 1.15. duckmidi finds its `duckmidi_history_runtime` entry point and hands it each exporter's [`DmeHistoryAdapter`](../include/duckmidi_repository_plugin.h).

## Sections and starting points

Each history section belongs to one exporter, game, checkout and game version, so different checkouts and versions never mix. The section also stores the name and options shown in the history panel.

The starting point (the baseline) is what duckmidi reverts to:

- If the exporter can list its files, they're all captured when the section is first opened, including uncommitted edits.
- Otherwise, each file is captured the first time an export writes it.

Git and existing `.bkp` files are never used as history. **Initialize** makes the current files the new starting point and clears that section's recorded changes.

## What's stored

The database lives at `plugins/history/repositories.sqlite3` and holds:

- File contents, stored once and shared between sections, checked by SHA-256 when read.
- For each managed file: its original content, the content duckmidi expects there now, and its size, timestamp and mode when last seen.
- The current changes, each with its recipe and the files it touches.
- Folders duckmidi created.
- The transaction in progress, if any.

Each change has a key, such as a song slot. Exporting the same key again replaces the earlier change. Every time something changes, all current changes are rebuilt on top of the starting point, so reverting one never puts an old file back over a later export. Changes that leave everything as it started are dropped, and unused file contents are cleaned up.

## Replaced samples

A sample replacement can carry a `drum-pan` value. If it does, the SM64/HackerSM64 and OoT exporters apply it to every drum that shares the replaced sample, and record those bank edits with the sample. Without one, drum pan stays as it was. Older recipes keep their original behavior.

## With history off

When **Enable** is off in the history drawer (`DmeRepositoryRequest.untracked`), exports skip the database entirely: nothing is opened, created or recorded. Each export is applied to the checkout as it is now, so exporting the same song again finds and updates its earlier files.

Changed files are written in place. With **Keep original files** on (`keep_originals`), each file's previous content is first saved as `<path>.bkp`, unless one already exists. With it off, files are simply overwritten. **Initialize** and reverting aren't available in this mode.

## Outside edits

Before changing anything, the runtime checks each managed file's size, timestamp and mode against what it recorded. Unchanged files aren't read. If something changed, the file is hashed: when the content still matches, the record is just refreshed; otherwise the operation stops and names the file. A recognized file that appeared after initialization also stops it. **Initialize** resolves the conflict by accepting the current files.

## Safe writes and recovery

Locks on the history folder and on the checkout keep two duckmidi windows, or two programs, from changing things at once. History must be on a local disk; remote or read-only storage fails instead of quietly keeping history in memory. The runtime refuses paths that leave the checkout or pass through links, and keeps the folders it's using open so they can't be renamed mid-operation.

Each change follows the same steps:

1. Check the new files and anything they depend on.
2. Save every before and after version in the database.
3. Write each file to a temporary file, flush it and rename it into place.
4. Verify the result, then update the database.

Cancelling rolls back finished steps. Keep your build tools stopped while an export or revert runs, since the files don't all change at the same instant.

After a crash, the runtime compares each pending file with its saved before and after versions, then puts back removed folders and files and removes empty folders it made. If an outside edit or access error gets in the way, it keeps the pending record and blocks further changes to that section until the conflict is resolved.

## Limits

- 20,000 files per section.
- 256 MiB per file or recipe.
- Up to 64 MiB of recently read content cached during an operation.

History databases from development builds are refused with a message naming the schema.

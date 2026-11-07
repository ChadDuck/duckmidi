# Repository runtime

Every exporter with Export to Repository shares one history runtime, `db` (`db.dll`, `db.so` or `db.dylib` in the plugins folder), which contains SQLite 3.50.4, yyjson 0.12.0 and pugixml 1.15. The desktop and core do not link SQLite. The host finds the runtime's `duckmidi_history_runtime` entry point and passes each exporter's [`DmeHistoryAdapter`](../include/duckmidi_repository_plugin.h) to it.

## Scopes and baselines

A scope binds the exporter, the detected target, the physical checkout identity and the build version, and records the target's display name and capabilities for the history panel. Separate checkouts, targets and versions keep separate histories.

An adapter with an inventory gets its baseline when the scope is first opened: every listed file, including dirty and untracked inputs. An adapter without one gets a first-touch baseline: each path is captured the first time an install writes it. Git and existing `.bkp` files are never treated as history. Initialize adopts the current files as the new baseline and discards that scope's recorded changes.

## Ledger

The database at `plugins/history/repositories.sqlite3` stores:

- content-addressed file bytes, shared between scopes and verified by SHA-256 when read;
- one row per managed path with its baseline content, the content duckmidi expects there now, and the size, timestamp and mode last observed on disk;
- the current logical modifications, each with its recipe and affected paths;
- directories duckmidi created;
- the pending transaction, while one is applying.

Applying an intent with an existing key replaces it. Rendering recomposes every current intent over the baseline, so removing one never restores a stale file over a later export. Intents whose effects equal the baseline are dropped, and unreferenced content is removed after each operation.

## Sample replacement recipes

Native sample replacement recipes may include an optional `drum-pan` override. When present, the SM64/HackerSM64 and OoT adapters apply it to every drum entry sharing the replaced physical sample and record those bank edits with the sample. Without an override, existing drum pan is preserved; earlier stored recipes retain their original behavior.

## Outside edits

Before changing anything, the runtime compares each managed path's directory entry with its recorded size, timestamp and mode. A file whose entry is unchanged is not read. A changed entry is hashed; if the content still matches, only the recorded entry is refreshed, otherwise the operation stops and names the path. A recognized file that appeared after initialization also stops it. Initialize resolves the conflict by adopting the current files.

## Transactions and recovery

An OS lock on the history folder and one on the physical checkout serialize changes across instances and processes. History must be on a local disk; unwritable or remote storage fails instead of falling back to memory. Anchored directory handles refuse traversal, linked parents and aliased destinations; they are reused during an operation and block outside renames of those folders while it runs.

Each transaction validates the desired state and its declared dependencies, stores every before and after file in the ledger, then writes each file through a flushed temporary file and rename. Cancellation rolls back completed steps, and the ledger is updated only after the result is verified. Build tools should stay stopped while a transaction applies, since files do not change at one instant.

Recovery after a crash compares every pending path with its recorded before and after states, restores removed directories, restores files, then removes empty directories it created. If an outside edit or access failure prevents that, the pending record and its bytes are kept and further changes to that scope are blocked until the conflict is resolved.

## Limits

20,000 paths per scope and 256 MiB per file or recipe. The runtime keeps up to 64 MiB of recently read content in memory during an operation. Older history databases from development builds are refused with an explicit schema message.

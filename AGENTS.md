# Assimil8or contributor notes

This repository builds an unofficial firmware extension for the Rossum
Electro-Music Assimil8or. C and assembly under `src/` are the overlay that
replaces a reclaimed region of the stock image; `ext/` is the second image
loaded from the card. `src/stock_abi.ld` holds the stock addresses the code
links against and `src/patch_layout.json` the instructions the patch
replaces. `tools/make_patch.py` builds the patch, and `patcher.py` and
`web/` apply it to a user's own copy of Rossum's software. Never add Rossum
software, a patched `app`, or a disassembly to the repository.

Documentation contains current facts and contracts. History lives in Git.

## Workflow

Carry implementation requests through edits and relevant verification.
Resolve routine choices from the task and current code; ask only when a
missing decision materially affects the result. Read-only reviews and
diagnoses end with findings unless implementation is requested.

Validate external data at its entry point and trust internal callers. Keep
the stock-software hash, the overlay bounds, the hook and splice checks, and
the result hashes intact. A patch may only fill the overlay region, replace
the listed instructions with branches into it, and copy the listed stock
ranges. Delete dead code with its build entries. Do not add durable tests
for new work; use existing checks and focused temporary experiments.

`PROTOCOL.md` and `PRESET_FORMAT.md` are contracts other people's editors
depend on. Change a message or a preset line only by adding to it, and
update the document in the same commit. Presets: read every layout a release
has written, write only the current one, and raise `EXTRAS_FORMAT` when the
written layout changes.

A change users will notice adds a line under `## Unreleased` in
`CHANGELOG.md`, in plain words, in the same commit; note what an earlier
release makes of presets it saves. `VERSION` changes only with a release.

For code changes, run `make check`; with Rossum's v3.04 download at hand,
`A8_STOCK_APP=/path/to/app-v304-260610.zip make check` also applies the
patch with both patchers and compares them. Run `make release` when patch
generation or packaging changes. Documentation or agent configuration
changes require only diff and syntax review. Nothing here proves a build on
hardware: say what was and was not run on a module.

After completing requested changes and verification, commit them unless the
user asks to leave them uncommitted. Stage only the task's changes. Pushing
requires separate user direction. Report the result, verification, and
remaining limits in concise prose.

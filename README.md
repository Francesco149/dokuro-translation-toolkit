# Dokuro-chan Fan Translation Toolkit

**Start with `docs/ORIENTATION.md`** - the unified front: project state, file formats, what's
been verified, the RE findings, the workflow rule, and the open TODOs. If you're an AI picking
this up in a fresh chat, read that file before doing anything else, then `docs/NEXT_SESSION.md`
for the unfinished work to carry over (currently the chunk-budget bypass RE).

**Branch layout:** `public` is the GitHub branch (single squashed snapshot, **no game
binaries** - only the `.sym` debug info); the local `master` is the archive with the full
history and `game-files/` (SCRIPT.UNI + ELF), which are gitignored on `public`. Nightly CI
(GitHub Actions) builds `build/dokuro-editor.exe` from `public` - see
`docs/BUILD_AND_TEST.md`.

**Status:** `native/` (C++) is the authoritative implementation. `dotnet/` and `reveng/`
are **potentially out of date** - kept specifically so free reverse-engineering efforts
can run in the Claude.ai website sandbox (plain .NET/Python, no nix toolchain).

**License:** MIT (see `LICENSE`). Note the game files are NOT part of this repo's code -
the game content itself is the copyright of its owners.

- `docs/` - orientation + format spec + RE findings + build/test how-to + next-session brief
  (one unified front; no session logs).
- `tools/` - standalone Python scripts (container split/join/inspect). No dependencies beyond
  stdlib. Useful for quick CLI probing independent of the .NET tool.
- `dotnet/DokuroScript.Core/` - **POTENTIALLY OUT OF DATE** .NET engine (STCM2 binary
  parse/rebuild, UNI2 container split/join, Shift-JIS text handling, project model, text-dump
  import/export). Sandbox-oriented; the native `core/` is authoritative.
- `dotnet/TestHarness/` - **POTENTIALLY OUT OF DATE** `dotnet run -- path/to/SCRIPT.UNI`
  regression harness for Core. Same sandbox purpose; the native host test suite is
  authoritative.
- `native/` - **the current translator tool**: Dear ImGui + DX11 dialogue editor (Win7
  compatible, i686 PE) with project folders (original/working copies, optional ISO patching),
  drag-reorder with original-vs-custom tracking, revert/add/delete, per-slot size-budget
  enforcement, a direct script editor tab, hardened txt export/import, and persistent
  deduped undo. Build/test/sign: `nix develop --command make -C native [tests|sign|selftest]`.
- `tools/gen_cp932/` - regenerates `native/core/cp932_tables.h` from .NET's cp932 so the
  native engine is byte-exact with the C# reference.

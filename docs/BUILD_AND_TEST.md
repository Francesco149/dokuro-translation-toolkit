# Building & Testing

## The native editor (current tool) — builds, tests, signing

The main translator tool is `native/dokuro-editor.exe` (C++17, Dear ImGui + DX11, 32-bit
Win32 PE — runs on Windows 7 32/64-bit and everything newer). Build setup mirrors the
OpenSummoners `tools/res_explorer` (i686 mingw cross, static, DX11). Run everything inside
`nix develop` (the flake exports `IMGUI_DIR`; running make outside fails loudly):

```
nix develop --command make -C native          # -> build/dokuro-editor.exe
nix develop --command make -C native sign     # + Authenticode self-sign (see below)
nix develop --command make -C native tests    # Linux host test binary -> build/run_tests
nix develop --command make -C native selftest # exe --selftest (needs Windows host or wine)
```

- **Stale-object safety** (load-bearing, do not remove): the Makefile
  fingerprints the whole build config (resolved toolchain paths + versions,
  `IMGUI_DIR`, every compile/link flag, env vars the nix wrappers consume) into
  `obj/.build_config`; every object depends on it, so ANY config change triggers
  a full rebuild — objects compiled against different headers/configs can never
  be mixed into one link again. Auto header deps (`-MMD -MP`) rebuild exactly
  the objects that include a changed header. Built-in make rules are disabled
  (`-r`) so missing `.d` files can't be "remade" into bogus targets. The exe
  also writes `crash.log` (first-chance access violations + unhandled
  exceptions with register context and stack dump) next to itself.
- **Host tests** (`native/tests/test_main.cpp`, ~4300 checks): untouched byte
  round trip on all 35 slots, ASCII/Cyrillic edit round trips, full-project
  stress (every string edited), dump export/import robustness (deleted blocks,
  garbage, CRLF, reorder), sanitize cases (incl. newline stripping), cp932
  encode/decode parity vs .NET, asm disasm/assemble byte round trip per slot +
  syntax-error battery, per-slot budget, undo (cross-session, dedup, tamper
  detection), load_pair reopen (structural changes, custom lines), synthetic-ISO
  in-place patching, encoding validation. Run with
  `build/run_tests <path-to-SCRIPT.UNI>` (defaults to `game-files/SCRIPT.UNI`).
- **Windows selftest**: `dokuro-editor.exe --selftest <SCRIPT.UNI> <tmpdir>` —
  headless core smoke test through the real mingw binary; writes `selftest.log`
  into `<tmpdir>` and exits 0 on success.
- **Headless UI regression**: `dokuro-editor.exe --uitest <SCRIPT.UNI|project.txt>
  <outdir>` — scripted drive of the real render loop (doc ops, filters, tab
  switching, reopen, io-injected clicks/typing/Enter, perf) with framebuffer
  PNG dumps in outdir; `--screenshot <out.png> [project]` saves one frame.
  Per the workflow rule these are the DEFAULT verification path.
- **Signing** (`make sign`): self-signs with a generated cert via
  `osslsigncode`. A signature (even self-signed) suppresses the Windows 7
  "Open File – Security Warning" MOTW dialog. Win8+/SmartScreen still shows
  its "unknown publisher" prompt for a self-signed cert — the chosen workaround
  is the Attachment Manager policy *Inclusion list for moderate risk file
  types* (registry
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Associations\ModRiskFileTypes`,
  add `.exe`), or `powershell Unblock-File <exe>` per file.

The exe's `dokuro_editor.ini` (next to it) holds UI font size (default 19 px),
undo backlog caps, window size, and the last project. Per-project state lives
in the project folder (`project.txt`, `undo.log`). Architecture: `docs/ORIENTATION.md`.

## Nightly CI (GitHub Actions)

`.github/workflows/nightly.yml` runs on a schedule (03:17 UTC, plus manual
dispatch): apt i686 mingw + Dear ImGui v1.91.4 (the version the flake pins),
then the host + ASan suites in **synthetic-only mode** and the Win32 cross
build, uploading `build/dokuro-editor.exe` as a 14-day artifact. The public
branch ships no game binaries, so the real-script tests cannot run there —
run the full suite locally from the archive branch (which keeps
`game-files/`): `cd native && ../build/run_tests`.

## Reference only: the superseded dotnet toolchain

`dotnet/DokuroScript.Core` is the byte-level reference implementation of the
format (the native `core/` is a port of it; `tools/gen_cp932` regenerates
`cp932_tables.h` from .NET's cp932). `dotnet/TestHarness` exercises Core
against the real `SCRIPT.UNI` (`dotnet run` from `dotnet/TestHarness/` with
`DOTNET_ROLL_FORWARD=LatestMajor` if only a newer SDK is installed). The
superseded WinForms GUI was removed from the tree (recoverable from git
history). If NuGet access is blocked, an empty
`<packageSources><clear/></packageSources>` `NuGet.config` next to each
`.csproj` lets plain net8.0 projects restore from the local framework packs.



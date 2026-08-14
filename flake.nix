{
  description = "Dokuro-chan fan translation toolkit — dev environment (PS2 RE + .NET toolchain + native editor)";

  inputs = {
    # Same revision as /opt/src/nix-lab for shared binary cache.
    nixpkgs.url = "github:nixos/nixpkgs/241313f4e8e508cb9b13278c2b0fa25b9ca27163";
  };

  outputs =
    { self, nixpkgs }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
      # Host-native binutils targeting mipsel (like Debian's
      # binutils-mipsel-linux-gnu): pkgsCross.X.binutils builds for host=X,
      # but .buildPackages builds for the build machine while targeting X —
      # so objdump runs on x86_64 and understands mips:5900 (EE MMI ops).
      mipsel-binutils = pkgs.pkgsCross.mipsel-linux-gnu.buildPackages.binutils;
      # reveng/od.sh calls `mipsel-linux-gnu-objdump`; nixpkgs names it
      # mipsel-unknown-linux-gnu-objdump. Alias for compatibility.
      mipsel-objdump-alias = pkgs.writeShellScriptBin "mipsel-linux-gnu-objdump" ''
        exec ${mipsel-binutils}/bin/mipsel-unknown-linux-gnu-objdump "$@"
      '';
      # 32-bit mingw cross-compiler — native/dokuro-editor.exe is a 32-bit
      # Win32 PE (runs on Windows 7 32- and 64-bit), same as the OpenSummoners
      # res_explorer build setup.
      mingw32 = pkgs.pkgsCross.mingw32.buildPackages;
      # Dear ImGui sources for the native editor (DX11 backend compiled into
      # the Windows PE — ImGui is source-vendored, not a lib). IMGUI_DIR is
      # exported into the dev shell, mirroring tools/res_explorer.
      imguiSrc = pkgs.imgui.src;
    in
    {
      devShells.x86_64-linux.default = pkgs.mkShell {
        packages = [
          pkgs.python3 # tools/ps2dbg.py, reveng dwarf parsing, investigation scripts
          pkgs.nodejs # pcsx2-mcp server, if ever run from WSL
          pkgs.dotnet-sdk # dotnet/DokuroScript.Core + TestHarness + tools/gen_cp932
          pkgs.p7zip # 7z CLI for ISO inspection/extraction
          pkgs.tinyxxd # hex dumps
          pkgs.jq
          pkgs.gnumake # native/ builds
          pkgs.openssl # native/ make sign (self-signed Authenticode cert)
          pkgs.osslsigncode # native/ make sign (skip the Win7 MOTW warning dialog)
          mingw32.gcc # i686-w64-mingw32-g++ — produces Win32 PE
          mingw32.binutils
          mipsel-binutils
          mipsel-objdump-alias
        ];

        shellHook = ''
          # Dear ImGui source checkout for the native editor (native/Makefile).
          export IMGUI_DIR=${imguiSrc}

          # mingw cross-compiler convenience aliases (same names as OpenSummoners).
          export MINGW_CC=i686-w64-mingw32-gcc
          export MINGW_CXX=i686-w64-mingw32-g++
        '';
      };

      # Forked PCSX2 with the DebugServer bridge (input injection +
      # framebuffer screenshots). See emulator/README.md.
      packages.x86_64-linux.pcsx2-dbg = pkgs.pcsx2.overrideAttrs (old: {
        pname = "pcsx2-dbg";
        patches = (old.patches or [ ]) ++ [
          ./emulator/patches/0001-debugserver-input-screenshot.patch
        ];
      });
    };
}

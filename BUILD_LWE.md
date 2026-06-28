# Building a custom linux-wallpaperengine

The Nix-packaged `linux-wallpaperengine` (2026-05-12 snapshot) has a real bug
in its native QuickJS `VectorAdapter`: reading `x`/`y`/`z`/`w` off a
vector-typed JS object via `JS_GetPropertyStr` doesn't reliably trigger the
class's exotic property getter. Scripts that do
`new Vec3(otherVec.multiply(scalar))` — the common hover-to-scale workshop
utility script (workshopId `3489089062`, used by GTA 6's logo objects among
many others) — silently get back a degenerate `(0,0,0)` vector instead of a
copy of the source. Whatever the script drives (most visibly an image's
`scale`) then renders at zero size: invisible, with no error logged anywhere.

`lwescenepatch.cpp`'s injected shim works around part of this by always
overriding `Vec2`/`Vec3`/`Vec4`/`Color` with our own implementation rather
than only supplying them when missing — but the cleanest fix is just using a
newer `linux-wallpaperengine` build where this is fixed upstream. `lweview.cpp`
prefers `~/.local/share/lwepaper/lwe-custom/linux-wallpaperengine` over the
Nix-profile binary when present.

## Why this lives in `~/.local`, not `~/.nix-profile`

`~/.local` is on the home partition and survives SteamOS updates. The
slynobody catsout-fork approach (installed via pacman to the SteamOS rootfs)
got wiped by the very next OS update, which is what motivated switching to
linux-wallpaperengine + this custom plugin in the first place.

## Rebuilding

```sh
# 1. Make sure the nix daemon is running (it doesn't survive reboots/logins
#    on this SteamOS setup — no systemd unit for it was ever installed).
sudo nohup /nix/var/nix/profiles/default/bin/nix-daemon &

# 2. Clone + fetch submodules (this pulls ~9 third-party repos: glslang and
#    SPIRV-Cross forks, stb, nlohmann/json, MimeTypes, kissfft, argparse,
#    Catch2, quickjs-ng).
git clone https://github.com/Almamu/linux-wallpaperengine.git ~/code/lwe-source
cd ~/code/lwe-source
git submodule update --init --recursive

# 3. Find dbus-1.pc (nix's linux-wallpaperengine devShell doesn't put it on
#    PKG_CONFIG_PATH by default for some reason) and configure inside the
#    devShell, which has the exact buildInputs/CEF download logic the
#    project expects.
DBUS_PC=$(find /nix/store -maxdepth 2 -name dbus-1.pc 2>/dev/null | grep -v dev | head -1)
mkdir build && cd build
nix --extra-experimental-features "nix-command flakes" develop \
    "nixpkgs#linux-wallpaperengine" --command bash -c "
  export PKG_CONFIG_PATH=\$PKG_CONFIG_PATH:$(dirname "$DBUS_PC")
  cmake .. -DCMAKE_BUILD_TYPE=Release \
           -DCMAKE_EXE_LINKER_FLAGS=-Wl,--allow-shlib-undefined
  make -j\$(nproc)
"
```

`-Wl,--allow-shlib-undefined` is needed because CEF's prebuilt `libcef.so`
pulls in dozens of transitive symbols (NSPR, cairo, pango, cups, IPP, GTK
a11y bits, etc.) that aren't all reachable through
`nixpkgs#linux-wallpaperengine`'s declared `buildInputs` — they're satisfiable
at runtime (CEF dynamically loads them lazily as needed for web-wallpaper
rendering, which we don't use), just not at *link* time of the final
executable. Without this flag the link fails outright.

## Packaging the result

```sh
DEST=~/.local/share/lwepaper/lwe-custom
mkdir -p "$DEST"
cp build/output/linux-wallpaperengine "$DEST/linux-wallpaperengine.real"
cp build/output/liblinux-wallpaperengine-lib.so "$DEST/"
cp -r build/cef/cef_binary_*/Release "$DEST/cef"
strip --strip-debug "$DEST/cef/libcef.so"   # ~1GB -> ~340MB, unstripped by default
strip "$DEST/linux-wallpaperengine.real" "$DEST/liblinux-wallpaperengine-lib.so"
```

### The runtime-libpath.txt + wrapper script

Two gotchas forced this design instead of just baking a proper RPATH in with
`patchelf`:

1. **glibc seems to choke on a very long (~9KB) `$ORIGIN`-relative RPATH
   string.** `patchelf --force-rpath` happily *writes* it (confirmed via
   `readelf -d`), but `LD_DEBUG=libs` then shows the dynamic linker only
   searching the literal `$ORIGIN` expansion as if it were the *entire*
   rpath, silently dropping everything appended after it. Switching to
   `LD_LIBRARY_PATH` set in a wrapper script (scoped to just this one
   process) avoids the issue entirely.
2. **Don't export that `LD_LIBRARY_PATH` in your interactive shell while
   debugging.** It's built from `ldd`'s resolved paths for half of
   nixpkgs (CEF's closure is enormous), and exporting it process-wide breaks
   unrelated tools that happen to share library names with something in that
   closure — `ldd` and `gdb` both segfaulted immediately when run with it set
   globally. Always scope it to the *target* process only (a wrapper script,
   or `gdb`'s `set environment` which applies only to the inferior, not gdb
   itself).

`runtime-libpath.txt` next to the wrapper holds that colon-joined path list.
Regenerate it if libraries move (e.g. after a Nix channel update bumps
versions):

```sh
ldd /nix/store/*-linux-wallpaperengine-*/bin/linux-wallpaperengine 2>&1 \
  | grep '=>' | awk '{print $3}' | xargs -I{} dirname {} | sort -u \
  | tr '\n' ':' | sed 's/:$//' > "$DEST/runtime-libpath.txt"
# then also append glew's lib dir explicitly — the cached binary's resolved
# closure pins an older glew than what the devShell-built binary links
# against:
echo -n ":$(nix build --no-link --print-out-paths 'nixpkgs#glew.out' 2>/dev/null)/lib" \
  >> "$DEST/runtime-libpath.txt"
```

The wrapper itself (`$DEST/linux-wallpaperengine`, what the plugin actually
invokes) just sets `LD_LIBRARY_PATH` from that file plus the bundled `cef/`
dir, then `exec`s `linux-wallpaperengine.real`. See the file in
`~/.local/share/lwepaper/lwe-custom/` for the current version — it's not
checked into this repo since it's machine-specific generated output, not
source.

## Verifying

```sh
nixGL ~/.local/share/lwepaper/lwe-custom/linux-wallpaperengine \
  --window 0x0x1920x1080 --bg <some workshop id> --silent --fps 5 \
  --assets-dir ~/.local/share/Steam/steamapps/common/wallpaper_engine/assets
```

`lweview.cpp` picks this binary up automatically (checked before falling back
to `~/.nix-profile/bin/linux-wallpaperengine`) — no plugin config needed once
it's in place.

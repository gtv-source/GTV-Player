#!/bin/bash
# Package the GPL source of the G-Player QEMU build (GPL-2.0 section 3).
#
#   gplayer/make-source-release.sh [-o <outdir>] [--dlls <dir>] [--no-download]
#
# Run from an MSYS2 shell in any directory of this tree. Produces in <outdir>
# (default: ../gplayer-source-release next to the tree):
#
#   gplayer-qemu-<ver>-<date>.tar.xz   the working tree as built: tracked files
#                                      with local edits, new GTV files, the
#                                      populated subprojects, GPLAYER-BUILD.txt
#   third-party/                       (unless --no-download) source of what is
#                                      shipped in binary form but not built here:
#     qemu-<ver>.tar.xz                upstream release: SeaBIOS/iPXE/... sources
#                                      for the pc-bios firmware in share/
#     aehd-v2.2-src.zip                Android Emulator Hypervisor Driver 2.2
#     msys2/*.src.tar.zst              MSYS2 source packages of every DLL found
#                                      in --dlls <dir> (the release folder)
#
# Nothing in the git repository (index, stash, branches) is changed.
set -euo pipefail

out=""
dlls=""
download=1
while [ $# -gt 0 ]; do
    case "$1" in
        -o) shift; out="$1" ;;
        --dlls) shift; dlls="$1" ;;
        --no-download) download=0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

src=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$src"
ver=$(cat VERSION)
name="gplayer-qemu-${ver}-$(date +%Y%m%d)"
out=${out:-"$(dirname "$src")/gplayer-source-release"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---------- file list ----------
# Local scratch that is not part of the source: build trees, editor temp files,
# and checkouts left from older QEMU layouts (9.2 takes these from subprojects/).
exclude='^(build[^/]*/|RCa[0-9]+$|dtc/|meson/|roms/sgabios/|tests/fp/berkeley-(soft|test)float-3/|ui/keycodemapdb/|subprojects/\.wraplock$)'

{
    # tracked (as in the working tree) + untracked, not ignored
    git ls-files -c -o --exclude-standard
    # wrap subprojects downloaded for the build (ignored by subprojects/.gitignore)
    for sp in subprojects/*/; do
        sp=${sp%/}
        [ -f "$sp.wrap" ] || continue
        find "$sp" -type f -not -path "*/.git/*" -not -name .git
    done
} | grep -Ev "$exclude" | sort -u | while IFS= read -r f; do
    [ -f "$f" ] && printf '%s\n' "$f"    # skips files deleted in the working tree
done > "$tmp/files"

# ---------- build notes ----------
# Files that differ from upstream: compared with the v<ver> tag rather than
# git status, so the list stays right after the changes are committed.
changes() {
    if git rev-parse -q --verify "v${ver}^{commit}" >/dev/null; then
        {
            git diff --name-status --no-renames --ignore-submodules=all "v${ver}" -- 2>/dev/null
            git ls-files -o --exclude-standard | sed 's/^/A\t/'
        } | RE="$exclude" awk -F'\t' '$2 !~ ENVIRON["RE"] { print "  " $1 " " $2 }'
    else
        git status --porcelain --untracked-files=all --ignore-submodules=all |
            RE="$exclude" awk '{ p = substr($0, 4) } p !~ ENVIRON["RE"] { print "  " $0 }'
    fi
}

configure_cmd=$(sed -n "s/^exec '..\/configure' //p" build/config.status 2>/dev/null |
                 sed 's/ *"\$@"$//' || true)
cat > "$tmp/GPLAYER-BUILD.txt" <<EOF
G-Player QEMU ${ver} — source of GPlayer-Launcher.exe (qemu-system-x86_64.exe)
and GPlayer-Img.exe (qemu-img.exe).

Based on QEMU ${ver} (https://www.qemu.org), GNU GPL version 2; see LICENSE
and COPYING. Upstream commit: $(git rev-parse HEAD)
Packaged: $(date -u +%Y-%m-%dT%H:%M:%SZ)

Build (MSYS2 MINGW64 shell, packages listed in the QEMU Windows build docs).
configure creates symbolic links (build/qemu-bundle): turn on Windows
Developer Mode or run the shell as Administrator. Tested with gcc 15.2,
Python 3.14, CMake 4.2 (--disable-libcbor avoids a Meson 1.5 / CMake 4 crash).

  mkdir build && cd build
  ../configure ${configure_cmd:-'--target-list=x86_64-softmmu --enable-whpx --enable-sdl --enable-opengl'} --disable-sdl-image --disable-libcbor
  ninja qemu-system-x86_64.exe qemu-img.exe

G-Player names: qemu-system-x86_64.exe -> GPlayer-Launcher.exe,
qemu-img.exe -> GPlayer-Img.exe.

Changes against upstream QEMU ${ver} (tag v${ver}; A added, M modified,
D deleted; committed or not):
$(changes)
EOF

# ---------- tarball ----------
tar -cf "$tmp/$name.tar" --transform "s,^,$name/," -T "$tmp/files" \
    --owner=0 --group=0 --mode='u+rw,go+r-w'
tar -rf "$tmp/$name.tar" --transform "s,^,$name/," -C "$tmp" GPLAYER-BUILD.txt
xz -T0 -6 -c "$tmp/$name.tar" > "$out/$name.tar.xz"
echo "source: $out/$name.tar.xz ($(wc -l < "$tmp/files") files)"

[ "$download" = 1 ] || exit 0

# ---------- third-party sources ----------
fetch() {  # url file
    [ -s "$2" ] && return 0
    curl -fL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"
}
mkdir -p "$out/third-party/msys2"
fetch "https://download.qemu.org/qemu-${ver}.tar.xz" "$out/third-party/qemu-${ver}.tar.xz"
fetch "https://github.com/google/android-emulator-hypervisor-driver/archive/refs/tags/v2.2.zip" \
      "$out/third-party/aehd-v2.2-src.zip"

if [ -n "$dlls" ]; then
    missing=()
    : > "$tmp/pkgs"
    for dll in "$dlls"/*.dll; do
        pkg=$(pacman -Qoq "/mingw64/bin/$(basename "$dll")" 2>/dev/null) || { missing+=("$(basename "$dll")"); continue; }
        echo "$pkg" >> "$tmp/pkgs"
    done
    sort -u -o "$tmp/pkgs" "$tmp/pkgs"
    while read -r pkg; do
        v=$(pacman -Q "$pkg" | cut -d' ' -f2)
        base=${pkg/mingw-w64-x86_64-/mingw-w64-}
        f="$base-$v.src.tar.zst"
        fetch "https://repo.msys2.org/mingw/sources/$f" "$out/third-party/msys2/$f" ||
            echo "WARNING: no source package for $pkg $v" >&2
    done < "$tmp/pkgs"
    [ ${#missing[@]} -eq 0 ] || echo "WARNING: not from MSYS2 (add their source by hand): ${missing[*]}" >&2
fi
echo "third-party: $out/third-party"

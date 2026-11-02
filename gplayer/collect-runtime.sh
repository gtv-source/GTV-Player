#!/bin/bash
# Assemble the redistributable QEMU runtime of G-Player into <dest>:
#
#   gplayer/collect-runtime.sh <dest> [--build <qemu build dir>]
#
# Publisher and version details: gplayer/release-info.conf (the exes are
# rebuilt first, so a change there is picked up).
#
#   GPlayer-Launcher.exe, GPlayer-Img.exe     from the QEMU build
#   *.dll                                     only the DLLs the two exes load,
#                                             taken from /mingw64/bin
#   share/                                    firmware the G-Player VM uses
#   COPYING, COPYING.LIB, LICENSE             QEMU licence texts
#   licenses/<package>/                       licence files of every DLL package
#   THIRD-PARTY-NOTICES.txt                   what is shipped and under which licence
#
# <dest> must be empty or not exist: files are never deleted from it.
# Run from an MSYS2 shell; needs objdump (mingw64) and pacman.
set -euo pipefail

dest=${1:?usage: collect-runtime.sh <dest> [--build <dir>]}
shift
src=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
build="$src/build"
[ "${1:-}" = "--build" ] && build=$2

MB=/mingw64/bin
OD="$MB/objdump.exe"
SYS=/c/Windows/System32

# Firmware for the G-Player machine (q35, virtio-vga, virtio-net, direct
# kernel boot); see core/gPlayer.js in the launcher.
FIRMWARE="bios-256k.bin vgabios-virtio.bin efi-virtio.rom efi-e1000.rom
          kvmvapic.bin linuxboot.bin linuxboot_dma.bin"

# DLLs loaded at run time with LoadLibrary (not in any import table).
EXTRA_DLLS=""

if [ -e "$dest" ] && [ -n "$(ls -A "$dest")" ]; then
    echo "$dest is not empty" >&2; exit 1
fi

# Rebuild first: the exes carry the version info of gplayer/release-info.conf
# (through version.rc), and must match the current source.
echo "build: ninja in $build"
ninja -C "$build" qemu-system-x86_64.exe qemu-img.exe >/dev/null ||
    { echo "build failed: run ninja in $build to see the errors" >&2; exit 1; }

mkdir -p "$dest/share" "$dest/licenses"

cp "$build/qemu-system-x86_64.exe" "$dest/GPlayer-Launcher.exe"
cp "$build/qemu-img.exe" "$dest/GPlayer-Img.exe"

# ---------- DLL closure ----------
# Windows DLLs (System32) are never shipped, even when MSYS2 has a file of the
# same name (e.g. Mesa's opengl32.dll).
declare -A seen
queue=("$dest/GPlayer-Launcher.exe" "$dest/GPlayer-Img.exe")
for d in $EXTRA_DLLS; do queue+=("$MB/$d"); seen[${d,,}]=1; done
while [ ${#queue[@]} -gt 0 ]; do
    f=${queue[0]}; queue=("${queue[@]:1}")
    for d in $("$OD" -p "$f" | awk '/DLL Name/ {print $3}'); do
        [ -n "${seen[${d,,}]:-}" ] && continue
        seen[${d,,}]=1
        [ -f "$SYS/$d" ] && continue
        if [ -f "$MB/$d" ]; then
            queue+=("$MB/$d")
        elif [[ ${d,,} != api-ms-win-* ]]; then
            echo "WARNING: $d (needed by $(basename "$f")) not found" >&2
        fi
    done
done
dlls=()
for d in "${!seen[@]}"; do
    [ -f "$SYS/$d" ] && continue
    real=$(cd "$MB" && ls | grep -i -x -F "$d" || true)
    [ -n "$real" ] && dlls+=("$real")
done
IFS=$'\n' dlls=($(printf '%s\n' "${dlls[@]}" | sort -f)); unset IFS
for d in "${dlls[@]}"; do cp "$MB/$d" "$dest/"; done

for fw in $FIRMWARE; do cp "$src/pc-bios/$fw" "$dest/share/"; done
cp "$src/COPYING" "$src/COPYING.LIB" "$src/LICENSE" "$dest/"

# Some packages (gmp, gnutls, nettle, ...) install no licence files: take the
# top-level COPYING*/LICENSE* of the upstream sources in the MSYS2 source package.
licenses_from_source() {  # <package short name> <version> <out dir>
    local work
    work=$(mktemp -d)
    curl -fsSL --retry 3 -o "$work/src.tar.zst" \
        "https://repo.msys2.org/mingw/sources/mingw-w64-$1-$2.src.tar.zst" || { rm -rf "$work"; return 1; }
    tar --zstd -xf "$work/src.tar.zst" -C "$work"
    for a in "$work"/*/*.tar.* "$work"/*/*.tgz; do
        [ -f "$a" ] || continue
        tar -xf "$a" -C "$work" --wildcards --no-wildcards-match-slash \
            '*/COPYING*' '*/LICENSE*' '*/LICENCE*' 2>/dev/null || true
    done
    mkdir -p "$3"
    find "$work" -mindepth 2 -maxdepth 2 -type f \
        \( -name 'COPYING*' -o -name 'LICENSE*' -o -name 'LICENCE*' \) -exec cp {} "$3/" \;
    rm -rf "$work"
    [ -n "$(ls -A "$3")" ] || { rmdir "$3"; return 1; }
}

# ---------- notices ----------
notices="$dest/THIRD-PARTY-NOTICES.txt"
qver=$(cat "$src/VERSION")

# publisher details, filled in once in gplayer/release-info.conf
COMPANY=GTV SOURCE_URL="" CONTACT_EMAIL=""
# shellcheck source=release-info.conf
[ -f "$src/gplayer/release-info.conf" ] && . "$src/gplayer/release-info.conf"
for v in SOURCE_URL CONTACT_EMAIL; do
    [ -n "${!v}" ] || echo "WARNING: $v is empty in gplayer/release-info.conf: fill it in before release" >&2
done
SOURCE_URL=${SOURCE_URL:-"<SOURCE-URL — fill in gplayer/release-info.conf before release>"}
CONTACT_EMAIL=${CONTACT_EMAIL:-"<CONTACT EMAIL — fill in gplayer/release-info.conf before release>"}
{
cat <<EOF
G-Player emulator components — third-party notices
==================================================

GPlayer-Launcher.exe and GPlayer-Img.exe are builds of QEMU ${qver}
(https://www.qemu.org) modified by ${COMPANY}, distributed under the GNU General
Public License version 2 (see COPYING and LICENSE). QEMU is a trademark of
Fabrice Bellard; G-Player is not affiliated with the QEMU project.

The complete corresponding source code of these programs, of the firmware in
share/, of the Android Emulator Hypervisor Driver in tools/aehd/ and of the
libraries below is available from ${COMPANY}:
    ${SOURCE_URL}
${COMPANY} will also provide it on request for at least three years from the date
you received this software: ${CONTACT_EMAIL}

Libraries (MSYS2 mingw64 packages; licence texts in licenses/<package>/)
------------------------------------------------------------------------
EOF
for d in "${dlls[@]}"; do
    pkg=$(pacman -Qoq "$MB/$d" 2>/dev/null || echo "?")
    if [ "$pkg" = "?" ]; then
        printf '%-28s (package unknown)\n' "$d"; continue
    fi
    info=$(pacman -Qi "$pkg")
    ver=$(sed -n 's/^Version *: //p' <<<"$info")
    lic=$(sed -n 's/^Licenses *: //p' <<<"$info" | sed 's/spdx://g')
    url=$(sed -n 's/^URL *: //p' <<<"$info")
    short=${pkg#mingw-w64-x86_64-}
    printf '%-28s %s %s\n%-28s   %s\n%-28s   %s\n' "$d" "$short" "$ver" "" "$lic" "" "$url"
    if [ ! -d "$dest/licenses/$short" ]; then
        if [ -d "/mingw64/share/licenses/$short" ]; then
            cp -r "/mingw64/share/licenses/$short" "$dest/licenses/"
        else
            licenses_from_source "$short" "$ver" "$dest/licenses/$short" ||
                echo "WARNING: no licence text for $short; add it by hand" >&2
        fi
    fi
done
cat <<EOF

Firmware (share/), built from the QEMU ${qver} source tree (roms/)
--------------------------------------------------------------------
bios-256k.bin                SeaBIOS — LGPL-3.0-only
vgabios-virtio.bin           SeaVGABIOS (SeaBIOS) — LGPL-3.0-only
efi-virtio.rom, efi-e1000.rom  iPXE — GPL-2.0-or-later (with UEFI parts under BSD-2-Clause-Patent)
kvmvapic.bin, linuxboot.bin, linuxboot_dma.bin  QEMU option ROMs — GPL-2.0-or-later

Android Emulator Hypervisor Driver 2.2 (tools/aehd/, when shipped)
------------------------------------------------------------------
Copyright Google LLC and the Linux KVM authors — GPL-2.0-only
(arch/x86/kvm/i8259.c: MIT). Source: https://github.com/google/android-emulator-hypervisor-driver (tag v2.2)

Bundled source files inside GPlayer-Launcher.exe
------------------------------------------------
stb_image.h, stb_truetype.h  Sean Barrett — MIT or public domain (Unlicense)
Font                         Segoe UI/Arial are read from the Windows Fonts folder at run time, not shipped
EOF
} > "$notices"
unix2dos -q "$notices" 2>/dev/null || true

echo "runtime: $dest (${#dlls[@]} DLLs, $(du -sh "$dest" | cut -f1))"

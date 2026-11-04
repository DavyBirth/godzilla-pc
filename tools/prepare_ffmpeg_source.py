"""Fetch and verify the complete source corresponding to the pinned FFmpeg prebuilt.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import hashlib
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "build/source-compliance"
SOURCES = {
    "ffmpeg-n7.1.1.tar.gz": (
        "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.1.1.tar.gz",
        "sha512",
        "6b9a5ee501be41d6abc7579a106263b31f787321cbc45dedee97abf992bf8236cdb2394571dd256a74154f4a20018d429ae7e7f0409611ddc4d6f529d924d175",
    ),
    "vcpkg-df8bfe5.tar.gz": (
        "https://github.com/microsoft/vcpkg/archive/df8bfe519564ae001903e5cdd32af0999531ef71.tar.gz",
        "sha256",
        "2960bcde9fa2f319fc8927668375ab905fdd17fc98799a482ad61583f6679345",
    ),
}
INSTRUCTIONS = """FFmpeg corresponding source
===========================

The runtime links the LGPL-2.1-or-later FFmpeg 7.1.1 prebuilt from
shadps4-emu/ext-ffmpeg-core, revision 94dde08c8a9e4271a93a2a7e4159e9fb05d30c0a.
Its native AAC decoder is selected by name (aac); FDK AAC is not linked.

The two archives here contain the full, unmodified FFmpeg implementation and
the exact vcpkg revision used to build it. FFmpeg's SHA-512 is the value in
that revision's ports/ffmpeg/portfile.cmake. Retain all upstream notices.

To rebuild the static Windows libraries, extract vcpkg, initialize a Git
checkout from that tree and apply dependencies/gz_ffmpeg-src/ffmpeg.patch.
Run bootstrap-vcpkg.bat, then run the following command from that checkout:

  vcpkg --overlay-triplets=<wrapper>/triplets install ffmpeg[avcodec,avfilter,avdevice,avformat,swresample,swscale]:x64-win-llvm-static-release

Here <wrapper> is dependencies/gz_ffmpeg-src. Its .github/workflows/build.yml
and triplets/ contain the original build steps and clang-cl toolchain.
The build downloads the pinned FFmpeg archive; ffmpeg-n7.1.1.tar.gz is supplied
here as a complete source copy. The recipe applies the patches in ports/ffmpeg/.

Application builds use the static libraries through the wrapper's CMake target.
The application's own sources and build instructions are at the source ZIP root.
"""


def source_files():
    paths = []
    for name, (_, algorithm, expected) in SOURCES.items():
        path = DESTINATION / name
        if not path.is_file() or hashlib.new(algorithm, path.read_bytes()).hexdigest() != expected:
            raise ValueError("Missing or changed FFmpeg source; run tools/prepare_ffmpeg_source.py: " + name)
        paths.append(path)
    instructions = DESTINATION / "BUILDING.txt"
    instructions.write_text(INSTRUCTIONS, encoding="utf-8")
    return paths + [instructions]


def main():
    DESTINATION.mkdir(parents=True, exist_ok=True)
    for name, (url, algorithm, expected) in SOURCES.items():
        path = DESTINATION / name
        if not path.is_file():
            temporary = path.with_suffix(path.suffix + ".download")
            try:
                with urllib.request.urlopen(url, timeout=60) as response:
                    temporary.write_bytes(response.read())
                if hashlib.new(algorithm, temporary.read_bytes()).hexdigest() != expected:
                    raise ValueError("Source checksum mismatch: " + name)
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
    for path in source_files():
        print(path.name)


if __name__ == "__main__":
    main()

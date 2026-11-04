# Standalone production AAC checks, without game data, a window or real saves.
# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot)
try {
    & clang++ -std=c++23 -fuse-ld=lld -DFMT_HEADER_ONLY -DNOMINMAX -DWIN32_LEAN_AND_MEAN `
        -Igpu -Igpu/shim -Ithird_party/shadPS4/src -Ibuild/_deps/gz_boost-src `
        -Ibuild/_deps/gz_fmt-src/include -Ibuild/_deps/gz_ffmpeg-src/include `
        tools/test_ajm_aac.cpp gpu/ajm_aac.cpp -o build/test_ajm_aac.exe `
        build/externals/ffmpeg-94dde08/lib/avcodec.lib `
        build/externals/ffmpeg-94dde08/lib/avutil.lib `
        build/externals/ffmpeg-94dde08/lib/swresample.lib `
        -lbcrypt -lole32 -loleaut32 -lstrmiids -lshlwapi -lws2_32 -luser32 -luuid -lpsapi
    if ($LASTEXITCODE) { throw 'AAC test compilation failed' }
    & build/test_ajm_aac.exe
    if ($LASTEXITCODE) { throw 'AAC decoder checks failed' }
} finally { Pop-Location }

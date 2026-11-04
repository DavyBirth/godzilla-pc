# Process-only defaults validated with Godzilla; explicit environment choices win.
$gzReferenceFonts = Join-Path $PSScriptRoot '..\third_party\shadPS4\src\imgui\renderer\fonts'
if (-not $env:GZ_PRESENT_MODE) { $env:GZ_PRESENT_MODE = 'Fifo' }
if (-not $env:GZ_SYSTEM_FONT) {
    $gzLatinFont = Join-Path $gzReferenceFonts 'NotoSans-Regular.ttf'
    if (Test-Path -LiteralPath $gzLatinFont) {
        $env:GZ_SYSTEM_FONT = [IO.Path]::GetFullPath($gzLatinFont)
        Write-Host 'Fonte de compatibilidade: Noto Sans (substitui SST-Roman ausente).'
    }
}
if (-not $env:GZ_SYSTEM_FONT_JP) {
    $gzJapaneseFont = Join-Path $gzReferenceFonts 'NotoSansCJK-Regular.ttc'
    if (Test-Path -LiteralPath $gzJapaneseFont) {
        $env:GZ_SYSTEM_FONT_JP = [IO.Path]::GetFullPath($gzJapaneseFont)
        Write-Host 'Fonte de compatibilidade: Noto Sans CJK (substitui SSTJpPro ausente).'
    }
}

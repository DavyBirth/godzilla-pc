param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
$launcherRoot=Split-Path $PSScriptRoot
$framework=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$compiler=Join-Path $framework 'csc.exe'
$wpf=Join-Path $framework 'WPF'
& $compiler /nologo /codepage:65001 /target:winexe /platform:x64 /optimize+ /langversion:5 `
    /r:System.dll /r:System.Core.dll /r:System.Web.Extensions.dll /r:System.Xaml.dll `
    "/r:$wpf\WindowsBase.dll" "/r:$wpf\PresentationCore.dll" "/r:$wpf\PresentationFramework.dll" `
    /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll `
    "/win32icon:$launcherRoot\src\godzilla.ico" "/out:$Output" "$launcherRoot\src\GodzillaLauncher.cs" "$launcherRoot\src\LauncherLanguages.cs" "$launcherRoot\src\ReleaseUpdater.cs" "$launcherRoot\src\PromptArtworkUpdate.cs" "$launcherRoot\src\NativeRuntime.cs"
if($LASTEXITCODE){throw 'Godzilla launcher compilation failed'}
& $compiler /nologo /codepage:65001 /target:winexe /platform:x64 /optimize+ /debug- /langversion:5 `
    /r:System.dll /r:System.Core.dll /r:System.Xaml.dll /r:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Drawing.dll `
    "/r:$wpf\WindowsBase.dll" "/r:$wpf\PresentationCore.dll" "/r:$wpf\PresentationFramework.dll" `
    "/win32icon:$launcherRoot\src\godzilla.ico" "/out:$(Join-Path (Split-Path $Output) 'Godzilla.Update.exe')" "$launcherRoot\src\GodzillaUpdater.cs" "$launcherRoot\src\ReleaseUpdater.cs" "$launcherRoot\src\OwnerIcon.cs"
if($LASTEXITCODE){throw 'Update helper compilation failed'}

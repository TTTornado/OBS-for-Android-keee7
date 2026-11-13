param(
    [string]$ToolsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools'),
    [string]$Clang = 'C:/msys64/clang64/bin/clang.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $ToolsRoot 'checks/screen-regression'
$java = Join-Path $ToolsRoot 'jdk-17.0.12/bin'
$javaSources = Join-Path $repo 'obs-studio/frontend/cmake/android/java'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& "$java/javac.exe" -encoding UTF-8 --release 8 -Xlint:all -d $out `
    "$javaSources/com/obsproject/studio/ObsProjectionSession.java" `
    "$repo/scripts/android-screen/ObsProjectionSessionTest.java"
if ($LASTEXITCODE -ne 0) { throw 'Projection session test compilation failed' }
& "$java/java.exe" -ea -cp $out com.obsproject.studio.ObsProjectionSessionTest
if ($LASTEXITCODE -ne 0) { throw 'Projection session regression failed' }
& $Clang -std=c11 -Wall -Wextra -Werror "-I$repo/obs-studio/plugins/android-screen" `
    "$repo/scripts/android-screen/capture-size-test.c" -o "$out/capture-size-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Capture-size test compilation failed' }
& "$out/capture-size-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Capture-size regression failed' }
& $Clang -std=c11 -Wall -Wextra -Werror `
    "$repo/obs-studio/plugins/android-screen/tests/capture-alpha-test.c" -o "$out/capture-alpha-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Capture-alpha test compilation failed' }
& "$out/capture-alpha-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Capture-alpha regression failed' }
& $Clang -std=c++17 -Wall -Wextra -Werror `
    "-I$repo/obs-studio/frontend/widgets" `
    "$repo/scripts/android-screen/screen-audio-start-rollback-test.cpp" -o "$out/screen-audio-start-rollback-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Screen audio start rollback test compilation failed' }
& "$out/screen-audio-start-rollback-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Screen audio start rollback regression failed' }
Write-Host 'CPU premultiplied-alpha and screen audio rollback tests passed.'
Write-Host 'Host lifecycle and sizing tests passed. Android service/driver behaviour still requires device tests.'

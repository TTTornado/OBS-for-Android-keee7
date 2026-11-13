param(
    [ValidateSet('configure','build','package','all')][string]$Action = 'all',
    [string]$ToolsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools'),
    [int]$Jobs = 8,
    [string]$NativeDeps,
    [ValidateSet('6.11.1')][string]$QtVersion = '6.11.1',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$GradleVersion = '9.3.1',
    [ValidateRange(36, 99)][int]$CompileSdkVersion = 36,
    [string]$SigningConfigFile = $env:OBS_ANDROID_SIGNING_CONFIG,
    [switch]$DebugSigning,
    [switch]$AntutuPackage
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$tools = [IO.Path]::GetFullPath($ToolsRoot)
$build = Join-Path $tools ('build-obs33-arm64-qt' + $QtVersion.Replace('.', ''))
$logs = Join-Path $tools 'build-logs'
$cmake = Join-Path $tools 'cmake/cmake-3.31.6-windows-x86_64/bin/cmake.exe'
$ninja = Join-Path $tools 'ninja/ninja.exe'
$ndk = Join-Path $tools 'ndk/android-ndk-r30'
$qt = Join-Path $tools "Qt/$QtVersion/android_arm64_v8a"
$hostQt = Join-Path $tools "Qt/$QtVersion/msvc2022_64"
$prefix = if ($NativeDeps) { [IO.Path]::GetFullPath($NativeDeps) } else { Join-Path $tools 'android-deps-ffmpeg8/arm64-v8a' }
$sdk = Join-Path $tools 'android-sdk'
$env:JAVA_HOME = Join-Path $tools 'jdk-17.0.12'
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_HOME = $sdk
$env:ANDROID_NDK_ROOT = $ndk
$env:ANDROID_NDK_HOME = $ndk
$env:ANDROID_USER_HOME = Join-Path $tools 'android-user-home'
$env:GRADLE_USER_HOME = Join-Path $tools 'gradle-cache'
# Qt invokes Gradle internally. A persistent daemon can retain this script's
# redirected output handle after a successful build and block the pipeline.
$env:GRADLE_OPTS = ($env:GRADLE_OPTS + ' -Dorg.gradle.daemon=false').Trim()
$env:PATH = "$tools/gradle/gradle-$GradleVersion/bin;$env:JAVA_HOME/bin;$hostQt/bin;$tools/ninja;$env:PATH"
New-Item -ItemType Directory -Force -Path $build,$logs,$env:ANDROID_USER_HOME | Out-Null
$packageId = 'com.obsproject.studio'
if ($AntutuPackage) { $packageId = 'com.antutu.ABenchMark' }
$packageSource = Join-Path $build 'android-package-source'
$sourcePackage = Join-Path $repo 'obs-studio/frontend/cmake/android'
$buildFullPath = [IO.Path]::GetFullPath($build).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$expectedPackageSource = [IO.Path]::GetFullPath((Join-Path $buildFullPath 'android-package-source'))
$packageSource = [IO.Path]::GetFullPath($packageSource)
if ($packageSource -ne $expectedPackageSource -or
    !$packageSource.StartsWith($buildFullPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace package staging outside the expected build directory: $packageSource"
}
if (Test-Path -LiteralPath $packageSource) { Remove-Item -LiteralPath $packageSource -Recurse -Force }
New-Item -ItemType Directory -Force -Path $packageSource | Out-Null
Get-ChildItem -LiteralPath $sourcePackage -Force | Where-Object Name -ne 'libs' |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $packageSource -Recurse -Force }
$manifestPath = Join-Path $packageSource 'AndroidManifest.xml'
$manifestText = [IO.File]::ReadAllText($manifestPath)
$manifestText = ([regex]'(?<=\bpackage=")[^"]+(?=")').Replace($manifestText, $packageId, 1)
[IO.File]::WriteAllText($manifestPath, $manifestText, [Text.UTF8Encoding]::new($false))
$servicePath = Join-Path $packageSource 'java/com/obsproject/studio/ObsForegroundService.java'
$serviceText = [IO.File]::ReadAllText($servicePath)
$serviceText = ([regex]'import com\.(?:obsproject\.studio|antutu\.ABenchMark)\.R;').Replace($serviceText, "import $packageId.R;", 1)
[IO.File]::WriteAllText($servicePath, $serviceText, [Text.UTF8Encoding]::new($false))
$revision = ((Get-Content -LiteralPath "$ndk/source.properties" | Where-Object { $_ -match '^Pkg.Revision\s*=' }) -split '=',2)[1].Trim()
$sdkNdk = Join-Path $sdk "ndk/$revision"
if (!(Test-Path -LiteralPath $sdkNdk)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $sdkNdk -Parent) | Out-Null
    New-Item -ItemType Junction -Path $sdkNdk -Target $ndk | Out-Null
}
function Invoke-BuildTool([string]$Program, [string[]]$Arguments, [string]$LogName) {
    # Gradle daemons can retain inherited log handles after a successful build.
    $log = Join-Path $logs ([IO.Path]::GetFileNameWithoutExtension($LogName) + '-' +
        (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $PID + '.log')
    Write-Host "Running $LogName; log: $log"
    # Windows PowerShell treats native stderr warnings as PowerShell errors.
    # The tool's exit status, rather than the presence of stderr, decides failure.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Program @Arguments 2>&1 | Out-File -Encoding utf8 -LiteralPath $log
        $toolExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($toolExit -ne 0) {
        Get-Content -LiteralPath $log -Tail 65 | Write-Host
        throw "$LogName failed ($toolExit)"
    }
    Get-Content -LiteralPath $log -Tail 8 | Write-Host
}
if ($Action -in @('configure','build','package','all')) {
    $argsConfigure = @('-S', "$repo/obs-studio", '-B', $build, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake",
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29', '-DANDROID_STL=c++_shared',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DCMAKE_PREFIX_PATH=$prefix;$qt", "-DQT_HOST_PATH=$hostQt",
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DENABLE_PLUGINS=ON', '-DENABLE_FRONTEND=ON',
        '-DENABLE_SCRIPTING=OFF', '-DENABLE_HEVC=ON', "-DANDROID_SDK_ROOT=$sdk", "-DCMAKE_INSTALL_PREFIX=$build/install")
    $argsConfigure += "-DOBS_ANDROID_COMPILE_SDK_VERSION=$CompileSdkVersion"
    $argsConfigure += "-DOBS_ANDROID_PACKAGE_NAME=$packageId", "-DOBS_ANDROID_PACKAGE_SOURCE_DIR=$($packageSource.Replace('\','/'))"
    Invoke-BuildTool $cmake $argsConfigure 'configure.log'
}
if ($Action -in @('build','package','all')) {
    Invoke-BuildTool $cmake @('--build', $build, '--parallel', "$Jobs") 'native-build.log'
}
if ($Action -in @('package','all')) {
    $modules = @(Get-ChildItem -Path "$build/rundir" -Recurse -File -Filter '*.so' |
        Where-Object { $_.DirectoryName -match 'obs-plugins$' })
    foreach ($required in @('android-mediacodec.so','android-browser.so')) {
        if ($required -notin $modules.Name) { throw "Built plugin missing: $required" }
    }
    # 插件走 assets：运行时由 AndroidRuntimeBootstrap 解到 $OBS_ROOT_PATH/lib。
    # （APK 的 lib/<abi>/ 只解 lib*.so，我们的插件名不带 lib 前缀，解不出来。）
    $packageAssets = Join-Path $packageSource 'assets/obs-plugins'
    if (Test-Path -LiteralPath $packageAssets) { Remove-Item -LiteralPath $packageAssets -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $packageAssets | Out-Null
    foreach ($module in $modules) { Copy-Item -LiteralPath $module.FullName -Destination $packageAssets -Force }
    $stripExe = Join-Path $ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe'
    if (Test-Path -LiteralPath $stripExe) {
        foreach ($module in (Get-ChildItem -LiteralPath $packageAssets -File -Filter '*.so')) {
            & $stripExe --strip-unneeded $module.FullName 2>$null | Out-Null
        }
    }
    Invoke-BuildTool 'python' @("$PSScriptRoot/android-plugin-bundle.py", 'generate', $packageAssets) 'plugin-bundle.log'
    $androidBuild = Join-Path $build 'frontend/android-build'
    $deploy = Join-Path $hostQt 'bin/androiddeployqt.exe'
    $unsigned = Join-Path $androidBuild 'obs-studio-test.apk'
    Invoke-BuildTool $deploy @('--input', "$build/frontend/android-obs-studio-deployment-settings.json",
        '--output', $androidBuild, '--apk', $unsigned, '--android-platform', "android-$CompileSdkVersion", '--release', '--aux-mode') 'android-stage.log'
    Invoke-BuildTool 'python' @("$PSScriptRoot/stage-qt-gradle-properties.py",
        '--deployment-settings', "$build/frontend/android-obs-studio-deployment-settings.json",
        '--output', $androidBuild, '--android-platform', "android-$CompileSdkVersion",
        '--sdk', $sdk, '--build-tools', '36.0.0') 'gradle-props.log'
    # Patch the staged runtime only. A second androiddeployqt invocation would
    # overwrite it with the pristine SDK jar, so build directly with Gradle.
    $patchedQtJar = Join-Path $build 'qt-ime-patched/Qt6Android.jar'
    Invoke-BuildTool 'python' @("$PSScriptRoot/qt-android-ime/patch-qt-android-jar.py",
        '--qt-version', $QtVersion, '--jar', "$qt/jar/Qt6Android.jar",
        '--android-jar', "$sdk/platforms/android-$CompileSdkVersion/android.jar",
        '--javac', "$env:JAVA_HOME/bin/javac.exe", '--output', $patchedQtJar) 'qt-ime-runtime.log'
    Copy-Item -LiteralPath $patchedQtJar -Destination "$androidBuild/libs/Qt6Android.jar" -Force
    Invoke-BuildTool "$tools/gradle/gradle-$GradleVersion/bin/gradle.bat" @('--no-daemon', '-p', $androidBuild, 'assembleRelease') 'android-package.log'
    $apk = Get-ChildItem -Path $androidBuild -Recurse -File -Filter '*unsigned.apk' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (!$apk) { $apk = Get-Item -LiteralPath $unsigned -ErrorAction Stop }
    $releases = Join-Path $repo 'releases'
    New-Item -ItemType Directory -Force -Path $releases | Out-Null
    $aligned = Join-Path $build 'obs-test-aligned.apk'
    $modeName = if ($AntutuPackage) { 'antutu' } else { 'obs' }
    $signed = Join-Path $releases ('OBS-Android-arm64-' + $modeName + '-hwcodec-browser-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.apk')
    Invoke-BuildTool "$sdk/build-tools/36.0.0/zipalign.exe" @('-f', '-P', '16', '4', $apk.FullName, $aligned) 'zipalign.log'
    if ($DebugSigning) {
        Invoke-BuildTool "$sdk/build-tools/36.0.0/apksigner.bat" @('sign','--ks',"$repo/android-shell/debug.keystore",
            '--ks-key-alias','androiddebugkey','--ks-pass','pass:android','--key-pass','pass:android','--out',$signed,$aligned) 'sign.log'
    } else {
        if (!$SigningConfigFile) { $SigningConfigFile = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'release key.txt' }
        if (!(Test-Path -LiteralPath $SigningConfigFile)) { throw 'Release signing configuration missing; pass -SigningConfigFile or explicitly choose -DebugSigning.' }
        Invoke-BuildTool 'python' @("$PSScriptRoot/sign-android-apk.py", '--config', $SigningConfigFile,
            '--input', $aligned, '--output', $signed, '--tools-root', $tools) 'sign.log'
    }
    Invoke-BuildTool "$sdk/build-tools/36.0.0/apksigner.bat" @('verify','--verbose',$signed) 'verify-signature.log'
    Invoke-BuildTool "$sdk/build-tools/36.0.0/zipalign.exe" @('-c','-P','16','-v','4',$signed) 'verify-alignment.log'
    Invoke-BuildTool 'python' @("$PSScriptRoot/verify-android-apk.py", $signed, '--tools-root', $tools,
        '--native-dir', $build, '--expected-package', $packageId) 'strict-apk-check.log'
    Invoke-BuildTool 'python' @("$PSScriptRoot/qt-android-ime/verify-runtime-apk.py", $signed) 'qt-runtime-apk-check.log'
    Invoke-BuildTool 'python' @("$PSScriptRoot/android-plugin-bundle.py", 'verify', $signed) 'plugin-bundle-apk.log'
    Get-FileHash -Algorithm SHA256 -LiteralPath $signed
    Write-Host "Test APK: $signed"
}

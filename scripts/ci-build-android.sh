#!/usr/bin/env bash
# CI 用的一站式 Android APK 构建（在 Ubuntu runner 上验证）。
#
#   bash scripts/ci-build-android.sh               完整：工具链 → 依赖 → OBS → 打包 → 签名
#   bash scripts/ci-build-android.sh configure     只跑 CMake configure（快速验证 CMake 改动）
#
# 与 scripts/build-frontend-apk-wsl.sh 的分工：那个假设工具链**已装好**（读 ~/.obs-android-env.sh），
# 是给开发机用的；这个自己把工具链拉下来，所以能在裸 runner 上跑。
#
# 依赖（FFmpeg 等 14 个）不由这里下载二进制 —— 由 deps-android 从源码构建到
# deps-android/prebuilt/，workflow 里用 actions/cache 缓存。首次构建最慢，命中后很快。
#
# 环境变量（都有默认值）：
#   NDK_VERSION     默认 r30（若系统 SDK 已预装对应版本则自动复用）
#   QT_VERSION      默认 6.11.1
#   CMAKE_VERSION   默认 3.31.6（必须 >= 3.28：obs-studio 的 cmake_minimum_required 是 3.28...3.30；系统已满足则复用）
#   CMDLINE_TOOLS   默认 13114758
#   GRADLE_VERSION  默认 9.3.1（设为 system 或系统已装 >= 9.0 时直接复用系统 gradle）
#   BUILD_TOOLS     默认 36.0.0（若系统 SDK 已预装则自动复用）
#   PLATFORM        默认 android-36（若系统 SDK 已预装则自动复用）
#   JOBS            默认 nproc
#   TOOLS_ROOT      自行拉取的工具链落盘位置（workflow 缓存它，通常仅含 Qt），默认 $HOME/android-ci-tools
#   DL_ROOT         下载物落盘位置（**不**缓存），默认 $TOOLS_ROOT-dl
#   SIGN_KEYSTORE   给了且文件存在就签名；否则只出未签名包
#   SIGN_KEY_ALIAS / SIGN_STORE_PASS / SIGN_KEY_PASS  签名三件套
set -euo pipefail

STEP="${1:-all}"
ABI=arm64-v8a
API="${API:-29}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${OBS_SOURCE_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"

NDK_VERSION="${NDK_VERSION:-r30}"
QT_VERSION="${QT_VERSION:-6.11.1}"
CMAKE_VERSION="${CMAKE_VERSION:-3.31.6}"
CMDLINE_TOOLS="${CMDLINE_TOOLS:-13114758}"
GRADLE_VERSION="${GRADLE_VERSION:-9.3.1}"
BUILD_TOOLS="${BUILD_TOOLS:-36.0.0}"
PLATFORM="${PLATFORM:-android-36}"
JOBS="${JOBS:-$(nproc)}"
TOOLS_ROOT="${TOOLS_ROOT:-$HOME/android-ci-tools}"
DL_ROOT="${DL_ROOT:-$TOOLS_ROOT-dl}"

# 语义化版本比较：$1 >= $2 返回 0。
version_ge() {
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]
}

resolve_sdk() {
    local cand
    for cand in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "/usr/local/lib/android/sdk"; do
        [ -n "$cand" ] || continue
        if [ -f "$cand/platforms/$PLATFORM/android.jar" ] &&
           [ -x "$cand/build-tools/$BUILD_TOOLS/apksigner" ]; then
            echo "$cand"
            return 0
        fi
    done
    echo "$TOOLS_ROOT/android-sdk"
}

resolve_ndk() {
    local cand props
    for cand in "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK_HOME:-}" "$TOOLS_ROOT/ndk/android-ndk-$NDK_VERSION"; do
        [ -n "$cand" ] && [ -f "$cand/source.properties" ] && { echo "$cand"; return 0; }
    done
    for props in "$SDK"/ndk/*/source.properties /usr/local/lib/android/sdk/ndk/*/source.properties; do
        [ -f "$props" ] || continue
        if grep -Eq "^Pkg\.ReleaseName *= *${NDK_VERSION}[[:space:]]*$" "$props"; then
            dirname "$props"
            return 0
        fi
    done
    echo "$TOOLS_ROOT/ndk/android-ndk-$NDK_VERSION"
}

QT="$TOOLS_ROOT/Qt/$QT_VERSION"
HOST="$QT/gcc_64"
SDK="$(resolve_sdk)"
NDK="$(resolve_ndk)"
GRADLE_HOME="$TOOLS_ROOT/gradle-$GRADLE_VERSION"
CMAKE_HOME="$TOOLS_ROOT/cmake-$CMAKE_VERSION-linux-x86_64"
BUILD_DIR="$ROOT/build-fe-$ABI-ci"
PREFIX="$ROOT/deps-android/prebuilt/$ABI"

export ANDROID_SDK_ROOT="$SDK"
export ANDROID_HOME="$SDK"
export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK_HOME="$NDK"
if [ -z "${JAVA_HOME:-}" ]; then
    for _jvm in /usr/lib/jvm/temurin-17-jdk-amd64 /usr/lib/jvm/java-17-openjdk-amd64; do
        [ -d "$_jvm" ] && { JAVA_HOME="$_jvm"; break; }
    done
fi
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"
# 尊重调用方或 setup-gradle 管理的标准 ~/.gradle，不再强塞进 TOOLS_ROOT（避免缓存耦合膨胀）。
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$HOME/.gradle}"
export PATH="$CMAKE_HOME/bin:$GRADLE_HOME/bin:$SDK/platform-tools:$SDK/cmdline-tools/latest/bin:$JAVA_HOME/bin:$PATH"

log() { echo ""; echo "########## $* ##########"; }

# ccache：按「预处理后的源码 + 编译器 + 完整编译选项」的内容哈希复用 .o。
# 三层共用一份缓存：deps 的 autotools/x264/ffmpeg 走 build.sh 里的同名 wrapper，
# deps 的 CMake 系与 OBS 主构建走 CMAKE_{C,CXX}_COMPILER_LAUNCHER。
# COMPILERCHECK=content 是因为 NDK 是解压出来的，不同 runner 上 mtime 不同。
setup_ccache() {
    if ! command -v ccache >/dev/null 2>&1; then
        log "ccache 未安装，跳过编译缓存"
        return 0
    fi
    export USE_CCACHE=1
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$ROOT}"
    export CCACHE_NOHASHDIR=true
    export CCACHE_COMPILERCHECK=content
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    export CCACHE_COMPRESS=true
    log "ccache 已启用：$(ccache --version | head -1) dir=$CCACHE_DIR"
}

# 7-Zip 的二进制名随发行版而变：Ubuntu 24.04 的 7zip 包给 7zz 与 7z/7za/7zr，
# 旧版 p7zip 给 7z。Qt 的 .7z 必须解包，所以这里把常见名字都认一遍，不写死。
SEVENZIP=""
find_sevenzip() {
    local c
    for c in 7z 7zz 7za 7zr; do
        if command -v "$c" >/dev/null 2>&1; then
            SEVENZIP="$c"
            return 0
        fi
    done
    echo "FATAL: 找不到 7-Zip 解包器（试过 7z/7zz/7za/7zr）。" >&2
    echo "       Ubuntu 24.04：sudo apt-get install -y 7zip" >&2
    echo "       （注意 p7zip-full 在 24.04 已是 transitional 包，不提供 7z）" >&2
    return 1
}

# ---------------------------------------------------------------- 工具链

fetch_cmake() {
    # obs-studio 要求 CMake >= 3.28：若宿主机/runner 已预装满足要求的 CMake（如 ubuntu-24.04 自带 3.31.6），
    # 直接复用；仅在缺失或版本过低时才自行下载到 TOOLS_ROOT。
    if [ -x "$CMAKE_HOME/bin/cmake" ]; then
        echo "CMake 已就位：$("$CMAKE_HOME/bin/cmake" --version | head -1)"
        return
    fi
    if command -v cmake >/dev/null 2>&1; then
        local sys_ver
        sys_ver="$(cmake --version 2>/dev/null | sed -n 's/^cmake version //p' | head -1)"
        if [ -n "$sys_ver" ] && version_ge "$sys_ver" "3.28"; then
            echo "复用系统 CMake：$sys_ver ($(command -v cmake))"
            return
        fi
    fi
    log "下载 CMake $CMAKE_VERSION"
    mkdir -p "$TOOLS_ROOT" "$DL_ROOT"
    local tgz="$DL_ROOT/cmake-$CMAKE_VERSION.tar.gz"
    [ -s "$tgz" ] || curl -fL --retry 3 --retry-delay 5 -o "$tgz" \
        "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
    tar -xzf "$tgz" -C "$TOOLS_ROOT"
    [ -x "$CMAKE_HOME/bin/cmake" ] || { echo "FATAL: CMake 解包后没找到 $CMAKE_HOME/bin/cmake" >&2; exit 1; }
}

fetch_ndk() {
    [ -f "$NDK/source.properties" ] && { echo "NDK 已就位：$NDK"; return; }
    log "下载 NDK $NDK_VERSION"
    mkdir -p "$TOOLS_ROOT/ndk" "$DL_ROOT"
    local zip="$DL_ROOT/android-ndk-$NDK_VERSION-linux.zip"
    [ -s "$zip" ] || curl -fL --retry 3 --retry-delay 5 -o "$zip" \
        "https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip"
    unzip -q "$zip" -d "$TOOLS_ROOT/ndk"
    rm -f "$zip"
    [ -f "$NDK/source.properties" ] || { echo "FATAL: 解压后没找到 $NDK" >&2; exit 1; }
}

fetch_qt() {
    if [ -x "$HOST/bin/androiddeployqt" ]; then
        echo "Qt 已就位：$QT"
        probe_host_tools
        return
    fi
    log "下载 Qt $QT_VERSION（Android arm64 目标 + Linux gcc_64 宿主，aqt）"
    # 用 aqt 按版本/目标解析官方仓库，不再手工拼 download.qt.io 的 7z URL：
    # 旧实现把 6.9.3 的构建时间戳与 RHEL 后缀写死，升版本要重新找包，
    # 还漏过单独的 ICU 73 运行时包（Ubuntu 24.04 自带 74）。
    # aqt 输出到 $TOOLS_ROOT/Qt，会建出 $QT_VERSION/{android_arm64_v8a,gcc_64}，
    # 与 configure_obs 的 -DCMAKE_PREFIX_PATH / -DQT_HOST_PATH 对齐；
    # 完整桌面宿主包自带所需运行时，不需要额外补 ICU。
    pip install --quiet aqtinstall
    mkdir -p "$TOOLS_ROOT/Qt"
    aqt install-qt all_os android "$QT_VERSION" android_arm64_v8a -O "$TOOLS_ROOT/Qt"
    aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 -O "$TOOLS_ROOT/Qt"
    [ -x "$HOST/bin/androiddeployqt" ] || {
        echo "FATAL: Qt 宿主缺 androiddeployqt（aqt 安装不完整？）" >&2
        exit 1
    }
    probe_host_tools
}

# 宿主工具自检。**这是为了 fail fast**：宿主 uic/moc 缺共享库时，CMake 要到 AutoUic
# 阶段才报错（那时已跑完依赖构建，几十分钟），而这里在工具链装配阶段就能撞出来（秒级）。
# 前面两次 CI 失败都是"跑到一半才发现工具链缺件"，这个探针就是针对那类问题。
#
# 判据是**动态链接器能不能加载它**，不是退出码：这些工具对 --version/--help 的返回码
# 并不统一（androiddeployqt --help 就返回 1），用 `|| exit 1` 会把能跑的工具判成坏的
# ——我自己就踩过：uic/moc/rcc 全 OK，探针却因为 androiddeployqt 的退出码 1 让 CI 挂了。
# 所以：**能加载 = 输出里有该工具自己的字样**；加载失败 = 输出里出现
# "error while loading shared libraries" / "cannot open shared object"。
probe_host_tools() {
    log "自检宿主 Qt 工具（uic/moc/rcc/androiddeployqt）"
    local tool p out
    for tool in uic moc rcc androiddeployqt; do
        p=""
        for cand in "$HOST/libexec/$tool" "$HOST/bin/$tool"; do
            [ -x "$cand" ] && { p="$cand"; break; }
        done
        if [ -z "$p" ]; then
            echo "  WARN: 找不到 $tool（libexec/bin 都没有）"
            continue
        fi
        # --version/--help 都可能非零退出，所以不判退出码，只收输出。
        out="$("$p" --version 2>&1 || true)"
        [ -n "$out" ] || out="$("$p" --help 2>&1 || true)"
        case "$out" in
        *"error while loading shared libraries"*|*"cannot open shared object file"*)
            echo "FATAL: $tool 起不来（共享库缺失）—— 下面是它的原始报错：" >&2
            printf '  %s\n' "$out" | head -3 >&2
            exit 1
            ;;
        esac
        echo "  OK   $tool"
    done
}


fetch_sdk() {
    if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
        log "下载 Android SDK commandline-tools"
        mkdir -p "$SDK/cmdline-tools" "$DL_ROOT"
        local zip="$DL_ROOT/cmdline-tools.zip"
        [ -s "$zip" ] || curl -fL --retry 3 --retry-delay 5 -o "$zip" \
            "https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS}_latest.zip"
        unzip -q "$zip" -d "$SDK/cmdline-tools"
        rm -f "$zip"
        # 官方要求目录名恰好是 latest，否则 sdkmanager 认为布局不对。
        [ -d "$SDK/cmdline-tools/latest" ] || mv "$SDK/cmdline-tools/cmdline-tools" "$SDK/cmdline-tools/latest"
    fi
    if [ ! -f "$SDK/platforms/$PLATFORM/android.jar" ] ||
       [ ! -x "$SDK/build-tools/$BUILD_TOOLS/apksigner" ] ||
       [ ! -x "$SDK/platform-tools/adb" ]; then
        log "安装 SDK 组件（$PLATFORM / build-tools $BUILD_TOOLS / platform-tools）"
        yes 2>/dev/null | "$SDK/cmdline-tools/latest/bin/sdkmanager" --licenses >/dev/null 2>&1 || true
        "$SDK/cmdline-tools/latest/bin/sdkmanager" --install \
            "platform-tools" "platforms;$PLATFORM" "build-tools;$BUILD_TOOLS" >/dev/null
    else
        echo "Android SDK 已就位：$SDK ($PLATFORM / build-tools $BUILD_TOOLS)"
    fi
    # 让 SDK 布局里也能按 revision 找到 NDK（Gradle/AGP 会去 $SDK/ndk/<rev> 找）；
    # 若 NDK 本身就在 $SDK/ndk/<rev> 则跳过，避免自覆盖。
    local rev target
    rev="$(sed -n 's/^Pkg\.Revision *= *//p' "$NDK/source.properties" | tr -d ' \r')"
    if [ -n "$rev" ]; then
        target="$SDK/ndk/$rev"
        if [ "$(readlink -f "$target" 2>/dev/null || true)" != "$(readlink -f "$NDK")" ]; then
            mkdir -p "$SDK/ndk"
            ln -sfn "$NDK" "$target"
        fi
    fi
}

GRADLE_BIN="${GRADLE_BIN:-}"
fetch_gradle() {
    if [ -n "$GRADLE_BIN" ] && [ -x "$GRADLE_BIN" ]; then
        echo "Gradle 已指定：$GRADLE_BIN"
        return
    fi
    if [ -x "$GRADLE_HOME/bin/gradle" ]; then
        GRADLE_BIN="$GRADLE_HOME/bin/gradle"
        echo "Gradle 已就位：$GRADLE_HOME"
        return
    fi
    local sys_bin sys_ver=""
    sys_bin="$(command -v gradle 2>/dev/null || true)"
    if [ -n "$sys_bin" ]; then
        sys_ver="$("$sys_bin" --version 2>/dev/null | awk '/^Gradle /{print $2; exit}')"
        if [ "$GRADLE_VERSION" = "system" ] || { [ -n "$sys_ver" ] && version_ge "$sys_ver" "9.0"; }; then
            GRADLE_BIN="$sys_bin"
            echo "复用系统 Gradle：${sys_ver:-unknown} ($GRADLE_BIN)"
            return
        fi
    fi
    if [ "$GRADLE_VERSION" = "system" ]; then
        echo "FATAL: GRADLE_VERSION=system，但 PATH 上找不到 gradle" >&2
        exit 1
    fi
    log "下载 Gradle $GRADLE_VERSION"
    mkdir -p "$TOOLS_ROOT" "$DL_ROOT"
    local zip="$DL_ROOT/gradle-$GRADLE_VERSION-bin.zip"
    [ -s "$zip" ] || curl -fL --retry 3 --retry-delay 5 -o "$zip" \
        "https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip"
    unzip -q "$zip" -d "$TOOLS_ROOT"
    rm -f "$zip"
    GRADLE_BIN="$GRADLE_HOME/bin/gradle"
}

# ---------------------------------------------------------------- 依赖

build_deps() {
    # 两个哨兵：FFmpeg（core 批次尾部）与 libusb（usb 批次）。
    # 缺任何一个都说明缓存不完整，重新构建（deps 脚本自己会按依赖名跳过已完成的）。
    if [ -f "$PREFIX/lib/libavcodec.a" ] && [ -f "$PREFIX/lib/libusb-1.0.a" ]; then
        echo "依赖产物已在位：$PREFIX（缓存命中，跳过源码构建）"
        install_nlohmann_json
        return
    fi
    log "拉取依赖源码"
    ( cd "$ROOT/deps-android" && bash fetch.sh )
    log "交叉编译依赖（含 FFmpeg/x264，最慢的一环）"
    ( cd "$ROOT/deps-android" && NDK="$NDK" API="$API" JOBS="$JOBS" USE_CCACHE="${USE_CCACHE:-0}" bash build.sh "$ABI" )
    for lib in libavcodec.a libusb-1.0.a libturbojpeg.a; do
        [ -f "$PREFIX/lib/$lib" ] || { echo "FATAL: 依赖缺失：$PREFIX/lib/$lib" >&2; exit 1; }
    done
    install_nlohmann_json
}

# frontend/CMakeLists.txt:73 会**无条件** include feature-plugin-manager.cmake，
# 而那里面第一行就是 find_package(nlohmann_json 3.11 REQUIRED)。
# 它不是 deps-android 清单里的包（那份清单是给 libobs/插件的），所以早期 CI 会在这里失败：
#   CMake Error ... Could not find a package configuration file provided by "nlohmann_json"
# 本仓的 Windows 构建之所以没暴露这个问题，是因为开发机上**恰好**装了 conda 的 nlohmann_json
#（CMakeCache 里 nlohmann_json_DIR 指向 miniconda3）。这种"靠机器上的偶发条件"不能在 CI 复现，
# 所以这里显式装一份：官方 release 的 header-only 包 + CMake config，装进依赖前缀
#（$PREFIX 已经在一个 CMAKE_PREFIX_PATH 里，find_package 自然找得到）。
install_nlohmann_json() {
    local ver=3.11.3
    if [ -f "$PREFIX/share/cmake/nlohmann_json/nlohmann_jsonConfig.cmake" ]; then
        echo "nlohmann_json 已就位（$PREFIX/share/cmake/nlohmann_json）"
        return
    fi
    log "安装 nlohmann_json $ver（前端 find_package 需要，header-only）"
    mkdir -p "$DL_ROOT"
    local tb="$DL_ROOT/json.tar.xz"
    [ -s "$tb" ] || curl -fL --retry 3 --retry-delay 5 -o "$tb" \
        "https://github.com/nlohmann/json/releases/download/v$ver/json.tar.xz"
    local tmp="$DL_ROOT/json-extract"
    rm -rf "$tmp"; mkdir -p "$tmp"
    tar -xJf "$tb" -C "$tmp"
    [ -d "$tmp/json" ] || { echo "FATAL: json.tar.xz 解包后没有 json/ 目录" >&2; exit 1; }
    # 头文件 → $PREFIX/include；CMake 包 → $PREFIX/share/cmake（官方布局）
    mkdir -p "$PREFIX/include/nlohmann" "$PREFIX/share/cmake"
    # json_fwd.hpp must ship alongside json.hpp: GoLiveAPI_CensoredJson.hpp includes
    # only the former, and its absence is exactly what broke frontend target 345/457.
    for _h in json.hpp json_fwd.hpp; do
        cp -f "$tmp/json/single_include/nlohmann/$_h" "$PREFIX/include/nlohmann/"
    done
    rm -rf "$PREFIX/share/cmake/nlohmann_json"
    cp -r "$tmp/json/cmake" "$PREFIX/share/cmake/nlohmann_json" 2>/dev/null || true
    # 官方 release 里带的是 *.in 模板（给 install 用的），find_package 需要的是生成好的
    # Config + ConfigVersion + Targets 三件。这里手写一份最小可用的（header-only 目标）——
    # 比拉起 CMake 的 install 快得多，且行为等价：一个 INTERFACE 目标 + include 目录。
    local d="$PREFIX/share/cmake/nlohmann_json"
    mkdir -p "$d"
    cat > "$d/nlohmann_jsonConfig.cmake" <<EOF
# 最小可用的 nlohmann_json 包配置（header-only）。由 scripts/ci-build-android.sh 生成。
if(TARGET nlohmann_json::nlohmann_json)
  return()
endif()
add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "$PREFIX/include")
set(nlohmann_json_VERSION "$ver")
set(nlohmann_json_FOUND TRUE)
EOF
    cat > "$d/nlohmann_jsonConfigVersion.cmake" <<EOF
set(PACKAGE_VERSION "$ver")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
  if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
  endif()
endif()
EOF
    rm -rf "$tmp"
    [ -f "$d/nlohmann_jsonConfig.cmake" ] || { echo "FATAL: nlohmann_json 配置没写成" >&2; exit 1; }
    echo "  已安装：$PREFIX/include/nlohmann/json.hpp + $d"
}

# ---------------------------------------------------------------- OBS

configure_obs() {
    log "CMake configure"
    rm -rf "$BUILD_DIR"
    local -a cc_launcher=()
    if [ "${USE_CCACHE:-0}" = "1" ] && command -v ccache >/dev/null 2>&1; then
        cc_launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    fi
    cmake -S "$ROOT/obs-studio" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="$PREFIX;$QT/android_arm64_v8a" \
        -DQT_HOST_PATH="$HOST" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DENABLE_PLUGINS=ON \
        -DENABLE_FRONTEND=ON \
        -DENABLE_SCRIPTING=OFF \
        -DENABLE_HEVC=ON \
        -DANDROID_SDK_ROOT="$SDK" \
        -DOBS_ANDROID_COMPILE_SDK_VERSION="${PLATFORM#android-}" \
        -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/install" \
        "${cc_launcher[@]}"
}

build_obs() {
    log "构建整树（-j$JOBS）"
    cmake --build "$BUILD_DIR" -j"$JOBS"
}

package_apk() {
    log "暂存插件 .so 进包源目录（assets/obs-plugins）"
    local pkg_assets="$ROOT/obs-studio/frontend/cmake/android/assets/obs-plugins"
    local plug_src
    plug_src="$(find "$BUILD_DIR/rundir" -type d -path '*/lib/obs-plugins' 2>/dev/null | head -1)"
    if [ -z "$plug_src" ]; then
        echo "FATAL: rundir 里没有 obs-plugins 目录 —— 插件没编出来，APK 会是空壳" >&2
        exit 1
    fi
    # 先清空再拷：只 cp -f 会让删掉的插件一直留在包里、旧 .so 覆盖新构建。
    rm -rf "$pkg_assets"
    mkdir -p "$pkg_assets"
    # 不变式：rundir/lib/obs-plugins/ 里的东西**就是**插件。再断言一次，防止前端应用库
    # （libobs_<abi>.so，曾让 APK 白胖一倍）被当插件混进来。
    local f
    for f in "$plug_src"/*.so; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in
            libobs_*|libQt*) echo "FATAL: lib/obs-plugins 里混进了非插件：$(basename "$f")" >&2; exit 1 ;;
        esac
    done
    cp -f "$plug_src"/*.so "$pkg_assets"/
    # strip：assets 不经过 Gradle 的 stripReleaseDebugSymbols，rundir 里那份是带 -g 的
    local strip="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
    for f in "$pkg_assets"/*.so; do
        [ -f "$f" ] || continue
        if [ -x "$strip" ]; then "$strip" --strip-unneeded "$f" || true; fi
    done
    # 这两个是本 fork 的核心插件。缺了就早点报错，别出一个"看着正常"的包。
    local m
    for m in android-mediacodec.so android-browser.so; do
        [ -f "$pkg_assets/$m" ] || { echo "FATAL: 插件缺失：$m" >&2; exit 1; }
    done
    # 运行时按 MANIFEST 逐个从 assets 解出来（不依赖 Qt 的 assets 目录列举能力）
    local count
    count=$(ls -1 "$pkg_assets"/*.so 2>/dev/null | wc -l)
    [ "$count" -gt 0 ] || { echo "FATAL: 插件目录为空：$pkg_assets" >&2; exit 1; }
    python3 "$SCRIPT_DIR/android-plugin-bundle.py" generate "$pkg_assets"
    echo "  已暂存：$(cd "$pkg_assets" && ls -1 *.so | tr '\n' ' ')"

    log "androiddeployqt 出包"
    local ab_dir="$BUILD_DIR/frontend/android-build"
    local fe="$BUILD_DIR/frontend"
    # 强制重打包：清掉 androiddeployqt/gradle 的 native 中间件。
    # 注意**不能**删整个 android-build/libs：CMake 把应用库 libobs_<abi>.so 放在那里，
    # 删了 androiddeployqt 会报 "Cannot find application binary"。
    # 只清掉上一轮残留的**插件** .so（插件名不带 lib 前缀），保留 lib* 的应用/Qt 库。
    if [ -d "$ab_dir/libs/$ABI" ]; then
        find "$ab_dir/libs/$ABI" -maxdepth 1 -type f -name '*.so' ! -name 'lib*' -delete
    fi
    mkdir -p "$ab_dir/libs/$ABI"
    APP_LIB="$(ls "$BUILD_DIR/frontend"/libobs_*.so 2>/dev/null | head -1)"
    [ -n "$APP_LIB" ] && cp -f "$APP_LIB" "$ab_dir/libs/$ABI/"
    rm -rf "$ab_dir"/build/outputs/apk \
           "$ab_dir"/build/intermediates/merged_jni_libs \
           "$ab_dir"/build/intermediates/merged_native_libs \
           "$ab_dir"/build/intermediates/stripped_native_libs
    ( cd "$fe" && "$HOST/bin/androiddeployqt" \
        --input "$fe/android-obs-studio-deployment-settings.json" \
        --output "$ab_dir" \
        --apk "$ab_dir/obs-studio.apk" \
        --depfile "$ab_dir/obs-studio.d" \
        --builddir "$fe" \
        --android-platform "$PLATFORM" \
        --release --aux-mode )
    python3 "$SCRIPT_DIR/qt-android-ime/patch-qt-android-jar.py" \
        --qt-version "$QT_VERSION" --jar "$QT/android_arm64_v8a/jar/Qt6Android.jar" \
        --android-jar "$SDK/platforms/$PLATFORM/android.jar" --javac "$JAVA_HOME/bin/javac" \
        --output "$BUILD_DIR/qt-ime-patched/Qt6Android.jar"
    cp -f "$BUILD_DIR/qt-ime-patched/Qt6Android.jar" "$ab_dir/libs/Qt6Android.jar"
    "$GRADLE_BIN" --no-daemon -p "$ab_dir" assembleRelease


    local apk
    apk="$(find "$ab_dir/build/outputs/apk/release" -name '*-unsigned.apk' 2>/dev/null | head -1)"
    [ -n "$apk" ] || apk="$(find "$BUILD_DIR" -name '*-unsigned.apk' 2>/dev/null | head -1)"
    [ -n "$apk" ] || apk="$(find "$ab_dir" -name '*.apk' ! -name '*-signed*' 2>/dev/null | head -1)"
    [ -n "$apk" ] || { echo "FATAL: 没找到生成的 APK" >&2; exit 1; }
    python3 "$SCRIPT_DIR/qt-android-ime/verify-runtime-apk.py" "$apk"
    python3 "$SCRIPT_DIR/android-plugin-bundle.py" verify "$apk"
    python3 "$SCRIPT_DIR/verify-android-apk.py" "$apk" \
        --aapt "$SDK/build-tools/$BUILD_TOOLS/aapt" \
        --readelf "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    echo "$apk" > "$BUILD_DIR/.unsigned-apk-path"
    echo "  未签名 APK：$apk"
}

sign_apk() {
    local apk out
    apk="$(cat "$BUILD_DIR/.unsigned-apk-path")"
    out="$BUILD_DIR/OBS-Android-$ABI-release.apk"

    if [ -z "${SIGN_KEYSTORE:-}" ] || [ ! -f "$SIGN_KEYSTORE" ]; then
        log "未提供签名密钥 —— 只产出未签名 APK"
        echo "  提示：release 包必须签名才能安装。本地先生成密钥（scripts/generate-release-key.sh），"
        echo "        或把 base64 密钥配进 GitHub Secrets，然后重跑。"
        echo "$apk" > "$BUILD_DIR/.final-apk-path"
        return
    fi

    # 少了任何一件签名参数都不要"先出个包"——那会产出一个装不上的 release APK，
    # 而且失败会晚到用户手上才暴露。
    : "${SIGN_KEY_ALIAS:?给了 SIGN_KEYSTORE 就必须给 SIGN_KEY_ALIAS}"
    : "${SIGN_STORE_PASS:?给了 SIGN_KEYSTORE 就必须给 SIGN_STORE_PASS}"
    : "${SIGN_KEY_PASS:?给了 SIGN_KEYSTORE 就必须给 SIGN_KEY_PASS}"

    log "zipalign（-P 16：16 KB 页对齐，Android 15+ 对 .so 的硬要求）"
    local aligned="$BUILD_DIR/aligned.apk"
    "$SDK/build-tools/$BUILD_TOOLS/zipalign" -f -P 16 4 "$apk" "$aligned"

    log "apksigner 签名"
    "$SDK/build-tools/$BUILD_TOOLS/apksigner" sign \
        --ks "$SIGN_KEYSTORE" \
        --ks-key-alias "$SIGN_KEY_ALIAS" \
        --ks-pass "pass:$SIGN_STORE_PASS" \
        --key-pass "pass:$SIGN_KEY_PASS" \
        --out "$out" "$aligned"

    log "校验（签名 + 对齐）"
    "$SDK/build-tools/$BUILD_TOOLS/apksigner" verify --verbose "$out"
    "$SDK/build-tools/$BUILD_TOOLS/zipalign" -c -P 16 -v 4 "$out" | tail -3

    echo "$out" > "$BUILD_DIR/.final-apk-path"
    echo "  已签名 APK：$out"
    sha256sum "$out"
    du -h "$out" | cut -f1 | xargs echo "  大小："
}

# ---------------------------------------------------------------- main

find_sevenzip
setup_ccache

log "环境"
echo "repo      : $ROOT"
echo "tools     : $TOOLS_ROOT"
echo "downloads : $DL_ROOT"
echo "ndk       : $NDK_VERSION ($NDK)"
echo "qt        : $QT_VERSION"
echo "cmake     : $CMAKE_VERSION"
echo "gradle    : $GRADLE_VERSION (user_home=$GRADLE_USER_HOME)"
echo "sdk       : $SDK  platform=$PLATFORM build-tools=$BUILD_TOOLS"
echo "jobs      : $JOBS"
echo "7-zip     : $SEVENZIP"
java -version 2>&1 | head -1

fetch_cmake
fetch_ndk
fetch_qt
fetch_sdk
fetch_gradle

cmake --version | head -1
"$GRADLE_BIN" --version | awk '/^Gradle /{print $0; exit}'

build_deps

configure_obs
[ "$STEP" = "configure" ] && { log "只配置，完成"; exit 0; }

build_obs
package_apk
sign_apk

log "完成"

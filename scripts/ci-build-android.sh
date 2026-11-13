#!/usr/bin/env bash
# CI 用的一站式 Android APK 构建（在已就位的 Ubuntu / workflow runner 上运行）。
#
#   bash scripts/ci-build-android.sh               完整：环境校验 → 依赖 → OBS → 打包 → 签名
#   bash scripts/ci-build-android.sh configure     只跑 CMake configure（快速验证 CMake 改动）
#
# 与 scripts/build-frontend-apk-wsl.sh 的分工：那个读 ~/.obs-android-env.sh，给本地开发机用；
# 本脚本假设环境已由 workflow（或宿主）准备好，不再自带兜底下载逻辑，缺工具时直接失败退出。
#
# 环境变量（都有默认值）：
#   NDK_VERSION     默认 r30（校验已有 NDK 的 Pkg.ReleaseName）
#   QT_VERSION      默认 6.11.1
#   GRADLE_VERSION  默认 system（或指定版本号；也可直接传 GRADLE_BIN）
#   BUILD_TOOLS     默认 36.0.0
#   PLATFORM        默认 android-36
#   JOBS            默认 nproc
#   QT_ROOT         Qt 安装位置，默认 $HOME/Qt
#   DL_ROOT         依赖源码/nlohmann_json 临时下载位置（不缓存），默认 $ROOT/../obs-dl
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
GRADLE_VERSION="${GRADLE_VERSION:-system}"
BUILD_TOOLS="${BUILD_TOOLS:-36.0.0}"
PLATFORM="${PLATFORM:-android-36}"
JOBS="${JOBS:-$(nproc)}"
QT_ROOT="${QT_ROOT:-$HOME/Qt}"
DL_ROOT="${DL_ROOT:-$ROOT/../obs-dl}"

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
    return 1
}

ndk_matches_version() {
    local dir="$1"
    [ -n "$dir" ] && [ -f "$dir/source.properties" ] || return 1
    grep -Eq "^Pkg\.ReleaseName *= *${NDK_VERSION}[[:space:]]*$" "$dir/source.properties"
}

resolve_ndk() {
    if [ -n "${NDK:-}" ] && [ -f "$NDK/source.properties" ]; then
        echo "$NDK"
        return 0
    fi
    local cand
    # Runner 默认会把 ANDROID_NDK_ROOT 指到旧版本（如 r27d），必须校验 Pkg.ReleaseName == NDK_VERSION。
    for cand in \
        "${ANDROID_NDK_ROOT:-}" \
        "${ANDROID_NDK_HOME:-}" \
        "${ANDROID_NDK_LATEST_HOME:-}" \
        "$SDK"/ndk/* \
        /usr/local/lib/android/sdk/ndk/*; do
        if ndk_matches_version "$cand"; then
            echo "$cand"
            return 0
        fi
    done
    return 1
}

QT="$QT_ROOT/$QT_VERSION"
HOST="$QT/gcc_64"
SDK="$(resolve_sdk || true)"
NDK="$(resolve_ndk || true)"
BUILD_DIR="$ROOT/build-fe-$ABI-ci"
PREFIX="$ROOT/deps-android/prebuilt/$ABI"

export ANDROID_SDK_ROOT="$SDK"
export ANDROID_HOME="$SDK"
export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK_HOME="$NDK"
export ANDROID_NDK="$NDK"
if [ -z "${JAVA_HOME:-}" ]; then
    for _jvm in /usr/lib/jvm/temurin-17-jdk-amd64 /usr/lib/jvm/java-17-openjdk-amd64; do
        [ -d "$_jvm" ] && { JAVA_HOME="$_jvm"; break; }
    done
fi
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"
# 尊重调用方或 setup-gradle 管理的标准 ~/.gradle。
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$HOME/.gradle}"
export PATH="$SDK/platform-tools:$SDK/cmdline-tools/latest/bin:$JAVA_HOME/bin:$PATH"

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

# ---------------------------------------------------------------- 工具链校验

check_cmake() {
    command -v cmake >/dev/null 2>&1 || { echo "FATAL: 找不到 cmake" >&2; exit 1; }
    local sys_ver
    sys_ver="$(cmake --version 2>/dev/null | sed -n 's/^cmake version //p' | head -1)"
    if [ -z "$sys_ver" ] || ! version_ge "$sys_ver" "3.28"; then
        echo "FATAL: CMake 版本不足（需要 >= 3.28，当前：${sys_ver:-unknown}）" >&2
        exit 1
    fi
    echo "CMake     : $sys_ver ($(command -v cmake))"
}

check_ndk() {
    if [ -z "$NDK" ] || [ ! -f "$NDK/source.properties" ]; then
        echo "FATAL: 找不到匹配 NDK_VERSION=$NDK_VERSION 的 NDK（请确认 runner 预装或设置 NDK）" >&2
        exit 1
    fi
    echo "NDK       : $NDK"
}

check_qt() {
    if [ ! -d "$QT/android_arm64_v8a" ] || [ ! -x "$HOST/bin/androiddeployqt" ]; then
        echo "FATAL: Qt $QT_VERSION 未就位：$QT（请先安装 android_arm64_v8a 与 gcc_64 到 QT_ROOT）" >&2
        exit 1
    fi
    echo "Qt        : $QT"
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
            echo "FATAL: 找不到宿主 Qt 工具 $tool（$HOST）" >&2
            exit 1
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

check_sdk() {
    if [ -z "$SDK" ] ||
       [ ! -f "$SDK/platforms/$PLATFORM/android.jar" ] ||
       [ ! -x "$SDK/build-tools/$BUILD_TOOLS/apksigner" ] ||
       [ ! -x "$SDK/platform-tools/adb" ]; then
        echo "FATAL: Android SDK 缺组件（需要 $PLATFORM / build-tools $BUILD_TOOLS / platform-tools；SDK=${SDK:-unset}）" >&2
        exit 1
    fi
    echo "SDK       : $SDK ($PLATFORM / build-tools $BUILD_TOOLS)"
    # Gradle/AGP 需要按 revision 在 $SDK/ndk/<rev> 找到 NDK。
    local rev
    rev="$(sed -n 's/^Pkg\.Revision *= *//p' "$NDK/source.properties" | tr -d ' \r')"
    if [ -n "$rev" ] && [ ! -d "$SDK/ndk/$rev" ]; then
        echo "FATAL: SDK 布局缺少 NDK 目录 $SDK/ndk/$rev（实际 NDK 在 $NDK）" >&2
        exit 1
    fi
}

GRADLE_BIN="${GRADLE_BIN:-}"
check_gradle() {
    if [ -n "$GRADLE_BIN" ]; then
        [ -x "$GRADLE_BIN" ] || { echo "FATAL: GRADLE_BIN 不可执行：$GRADLE_BIN" >&2; exit 1; }
        echo "Gradle    : $GRADLE_BIN"
        return
    fi
    local sys_bin sys_ver=""
    sys_bin="$(command -v gradle 2>/dev/null || true)"
    [ -n "$sys_bin" ] || { echo "FATAL: PATH 上找不到 gradle" >&2; exit 1; }
    sys_ver="$("$sys_bin" --version 2>/dev/null | awk '/^Gradle /{print $2; exit}')"
    if [ "$GRADLE_VERSION" != "system" ] && [ "$sys_ver" != "$GRADLE_VERSION" ]; then
        echo "FATAL: Gradle 版本不匹配（要求：$GRADLE_VERSION，实际：${sys_ver:-unknown} @ $sys_bin）" >&2
        exit 1
    fi
    if [ -n "$sys_ver" ] && ! version_ge "$sys_ver" "9.0"; then
        echo "FATAL: Gradle 版本过低（需要 >= 9.0，当前：$sys_ver @ $sys_bin）" >&2
        exit 1
    fi
    GRADLE_BIN="$sys_bin"
    echo "Gradle    : ${sys_ver:-unknown} ($GRADLE_BIN)"
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
    python3 "$SCRIPT_DIR/stage-qt-gradle-properties.py" \
        --deployment-settings "$fe/android-obs-studio-deployment-settings.json" \
        --output "$ab_dir" \
        --android-platform "$PLATFORM" \
        --sdk "$SDK" \
        --build-tools "$BUILD_TOOLS"
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

setup_ccache

log "环境与工具链校验"
echo "repo      : $ROOT"
echo "downloads : $DL_ROOT"
echo "jobs      : $JOBS"
echo "gradle    : $GRADLE_VERSION (user_home=$GRADLE_USER_HOME)"

command -v java >/dev/null 2>&1 || { echo "FATAL: 找不到 java（JAVA_HOME=${JAVA_HOME:-unset}）" >&2; exit 1; }
java -version 2>&1 | head -1

check_cmake
check_ndk
check_qt
check_sdk
check_gradle

"$GRADLE_BIN" --version | awk '/^Gradle /{print $0; exit}'

build_deps

configure_obs
[ "$STEP" = "configure" ] && { log "只配置，完成"; exit 0; }

build_obs
package_apk
sign_apk

log "完成"

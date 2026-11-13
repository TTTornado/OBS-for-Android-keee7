#!/usr/bin/env python3
"""Stage local.properties and gradle.properties for Qt Android --aux-mode builds.

`androiddeployqt --aux-mode` prepares libraries, assets, resources, and
build.gradle, but returns before `buildAndroidProject()`, which normally writes
`local.properties` and Qt's required properties (such as `qtGradlePluginType`)
into `gradle.properties`. When CMake's default APK target is excluded from ALL,
this helper fills those two property files from the CMake-generated
deployment-settings JSON so Gradle can assemble directly after Qt6Android.jar
patching.
"""

from __future__ import annotations

import argparse
import configparser
import json
import os
from pathlib import Path
import re


def _parse_kv_file(path: Path) -> dict[str, str]:
    props: dict[str, str] = {}
    if not path.is_file():
        return props
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("!") or "=" not in raw:
            continue
        k, v = raw.split("=", 1)
        props[k.strip()] = v.strip()
    return props


def _merge_kv_file(path: Path, updates: dict[str, str], template_lines: list[str] | None = None) -> None:
    remaining = dict(updates)
    out_lines: list[str] = []

    if path.is_file():
        source_lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    elif template_lines is not None:
        source_lines = list(template_lines)
    else:
        source_lines = []

    for raw in source_lines:
        stripped = raw.strip()
        if stripped == "@EXTRA_PROPERTIES@":
            continue
        if stripped and not stripped.startswith("#") and not stripped.startswith("!") and "=" in raw:
            key = raw.split("=", 1)[0].strip()
            if key in remaining:
                out_lines.append(f"{key}={remaining.pop(key)}")
                continue
        out_lines.append(raw.rstrip())

    for key, val in remaining.items():
        out_lines.append(f"{key}={val}")

    path.write_text("\n".join(out_lines) + "\n", encoding="utf-8", newline="\n")


def _clean_package_name(name: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z._]", "_", name)
    cleaned = re.sub(r"\.+", ".", cleaned).strip(".")
    if not cleaned:
        return "org.qtproject.example"
    return cleaned


def _read_ndk_revision(ndk_dir: Path) -> str:
    source_props = ndk_dir / "source.properties"
    if not source_props.is_file():
        raise FileNotFoundError(f"NDK source.properties not found: {source_props}")
    # configparser requires a section header; prepend a dummy section.
    cp = configparser.ConfigParser()
    cp.read_string("[ndk]\n" + source_props.read_text(encoding="utf-8", errors="replace"))
    rev = cp.get("ndk", "Pkg.Revision", fallback="").strip()
    if not rev:
        raise ValueError(f"Pkg.Revision missing in {source_props}")
    return rev


def _uses_integer_compile_sdk(build_gradle: Path) -> bool:
    if not build_gradle.is_file():
        return False
    for raw in build_gradle.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("//") or line.startswith("*") or line.startswith("/*"):
            continue
        if "compileSdkVersion androidCompileSdkVersion.toInteger()" in line:
            return True
    return False


def _resolve_abi_value(raw_val: object, abi: str, default: str = "") -> str:
    if isinstance(raw_val, dict):
        v = raw_val.get(abi)
        return str(v) if v else default
    if isinstance(raw_val, str) and raw_val:
        return raw_val
    return default


def _resolve_qt_paths(settings: dict) -> tuple[str, str, list[str]]:
    archs = settings.get("architectures") or {}
    enabled_abis = [abi for abi, triple in archs.items() if triple is not None]
    if not enabled_abis:
        raise ValueError("No enabled architectures in deployment settings")

    for abi in enabled_abis:
        qt_dir = _resolve_abi_value(settings.get("qt"), abi)
        if qt_dir:
            qt_data_dir = _resolve_abi_value(settings.get("qtDataDirectory"), abi, default=".")
            return qt_dir, qt_data_dir, enabled_abis

    raise ValueError("Missing matching 'qt' entry for enabled ABIs in deployment settings")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--deployment-settings", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--android-platform", required=True)
    ap.add_argument("--sdk", default="")
    ap.add_argument("--build-tools", default="")
    ap.add_argument("--build-aar", action="store_true")
    args = ap.parse_args()

    settings = json.loads(args.deployment_settings.read_text(encoding="utf-8"))
    output_dir = args.output
    output_dir.mkdir(parents=True, exist_ok=True)

    sdk_path = (
        args.sdk
        or settings.get("sdk")
        or os.environ.get("ANDROID_SDK_ROOT")
        or os.environ.get("ANDROID_HOME")
        or ""
    )
    if not sdk_path:
        raise ValueError("Android SDK path could not be resolved")

    qt_dir, qt_data_dir, enabled_abis = _resolve_qt_paths(settings)
    qt_java_dir = (Path(qt_dir) / qt_data_dir / "src/android/java").as_posix()
    template_props_path = Path(qt_dir) / qt_data_dir / "src/3rdparty/gradle/gradle.properties"

    template_lines: list[str]
    if template_props_path.is_file():
        template_lines = template_props_path.read_text(encoding="utf-8", errors="replace").splitlines()
    else:
        template_lines = [
            "org.gradle.jvmargs=-Xmx3200m -XX:MaxMetaspaceSize=768m -XX:+HeapDumpOnOutOfMemoryError -Dfile.encoding=UTF-8",
            "org.gradle.parallel=true",
            "android.useAndroidX=true",
        ]

    ndk_dir = Path(settings["ndk"])
    ndk_version = _read_ndk_revision(ndk_dir)

    build_gradle = output_dir / "build.gradle"
    platform = args.android_platform
    if _uses_integer_compile_sdk(build_gradle) and "-" in platform and platform.rsplit("-", 1)[-1].isdigit():
        compile_sdk = platform.rsplit("-", 1)[-1]
    else:
        compile_sdk = platform

    app_binary = settings.get("application-binary", "app")
    pkg_name = _clean_package_name(settings.get("android-package-name") or f"org.qtproject.example.{app_binary}")

    gradle_props_path = output_dir / "gradle.properties"
    existing_props = _parse_kv_file(gradle_props_path)

    build_tools = (
        existing_props.get("androidBuildToolsVersion")
        or str(settings.get("sdkBuildToolsRevision", ""))
        or args.build_tools
    )
    legacy_pkg = "true" if settings.get("android-legacy-packaging") else "false"

    updates = {
        "buildDir": "build",
        "qtAndroidDir": qt_java_dir,
        "qt5AndroidDir": qt_java_dir,
        "androidPackageName": pkg_name,
        "androidCompileSdkVersion": compile_sdk,
        "qtMinSdkVersion": str(settings.get("android-min-sdk-version", "28")),
        "qtTargetSdkVersion": str(settings.get("android-target-sdk-version", "36")),
        "androidNdkVersion": ndk_version,
        "legacyPackaging": legacy_pkg,
        "qtTargetAbiList": ",".join(enabled_abis),
        "qtGradlePluginType": "com.android.library" if args.build_aar else "com.android.application",
    }
    if build_tools:
        updates["androidBuildToolsVersion"] = build_tools

    _merge_kv_file(
        output_dir / "local.properties",
        {"sdk.dir": sdk_path.replace("\\", "/")},
    )
    _merge_kv_file(gradle_props_path, updates, template_lines=template_lines)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Сравнивает версии, закреплённые в CI, с новейшими стабильными выпусками.

    python3 tools/check_versions.py [--root каталог]

Читает .github/workflows/*.yml и CMakeLists.txt, спрашивает GitHub, LunarG и
Google и завершается с кодом 1, если что-то устарело или источник не ответил.
Предрелизы не считаются: GitHub не отдаёт их как latest, а Google помечает их
элементом preview. Токен для GitHub берётся из GITHUB_TOKEN.

Скрипт зовёт отдельный еженедельный workflow, поэтому новый выпуск чужого
инструмента не красит обычный CI. Только стандартная библиотека Python.
"""

import argparse
import json
import os
import pathlib
import re
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree

GITHUB_API = "https://api.github.com/repos/"
LUNARG_LATEST = "https://vulkan.lunarg.com/sdk/latest.json"
GOOGLE_REPOSITORY = "https://dl.google.com/android/repository/repository2-{}.xml"
GOOGLE_IMAGES = "https://dl.google.com/android/repository/sys-img/google_apis/sys-img2-{}.xml"
# Индексы Google нумеруются схемой, и новый номер появляется без объявления:
# старый при этом перестаёт пополняться. С этих номеров начинается поиск.
FIRST_REPOSITORY_SCHEMA = 4
FIRST_IMAGES_SCHEMA = 5

# Переменная в env workflow и то, с чем её сравнивать.
ENV_PINS = {
    "LLVM_VERSION": "llvm-major",
    "LLVM_WINDOWS_RELEASE": "llvm",
    "DXC_RELEASE": "dxc",
    "VULKAN_SDK_RELEASE": "vulkan-sdk",
    "ANDROID_NDK_VERSION": "android-ndk",
    "LAIUE_ANDROID_IMAGE": "android-image",
    "LAIUE_BUILD_TOOLS": "android-build-tools",
    "LAIUE_CMDLINE_TOOLS": "android-cmdline-tools",
    "LAIUE_EMULATOR": "android-emulator",
    "FOX_ANDROID_PLATFORM": "android-image",
    "FOX_BUILD_TOOLS": "android-build-tools",
    "FOX_CMDLINE_TOOLS": "android-cmdline-tools",
    "FOX_EMULATOR": "android-emulator",
}

# Пакеты индекса Google, у которых версия стоит в пути.
GOOGLE_PATH_PREFIXES = {
    "android-ndk": "ndk;",
    "android-build-tools": "build-tools;",
    "android-cmdline-tools": "cmdline-tools;",
}

ENV_LINE = re.compile(r"^\s*([A-Z][A-Z0-9_]*):\s*['\"]?([^'\"\s#]+)", re.MULTILINE)
ACTION_LINE = re.compile(r"uses:\s*([\w.-]+/[\w.-]+)@(\S+)(?:\s*#\s*(\S+))?")
CMAKE_POLICY_LINE = re.compile(r"cmake_minimum_required\(VERSION\s+[\d.]+\.\.\.([\d.]+)\)")
FULL_COMMIT = re.compile(r"[0-9a-f]{40}")
PLAIN_VERSION = re.compile(r"\d+(\.\d+)*")


class SourceError(Exception):
    """Источник версий не ответил или ответил не тем."""


class Missing(SourceError):
    """Такого адреса нет: 404."""


def parse_version(text):
    numbers = re.findall(r"\d+", text)
    if not numbers:
        raise SourceError(f"no version in {text!r}")
    return tuple(int(number) for number in numbers)


def add_pin(pins, name, source, version):
    """Одно имя может стоять в нескольких местах: считается самое старое."""
    if name not in pins or parse_version(version) < parse_version(pins[name][1]):
        pins[name] = (source, version)


def read_pins(root):
    """Возвращает {имя закрепления: (источник, версия)} и список нарушений."""
    pins, problems = {}, []
    for workflow in sorted((root / ".github" / "workflows").glob("*.yml")):
        text = workflow.read_text(encoding="utf-8")
        for name, value in ENV_LINE.findall(text):
            if name in ENV_PINS:
                add_pin(pins, name, ENV_PINS[name], value.removeprefix("android-"))
        for action, reference, comment in ACTION_LINE.findall(text):
            if not FULL_COMMIT.fullmatch(reference) or not comment:
                problems.append(f"{action}@{reference}: pin a full commit and name its version")
                continue
            add_pin(pins, action, f"github:{action}", comment)
    lists = root / "CMakeLists.txt"
    if lists.exists():
        policy = CMAKE_POLICY_LINE.search(lists.read_text(encoding="utf-8"))
        if policy:
            add_pin(pins, "cmake_minimum_required", "cmake-minor", policy.group(1))
    return pins, problems


def github_latest(fetch, repository):
    answer = json.loads(fetch(f"{GITHUB_API}{repository}/releases/latest"))
    return answer["tag_name"]


def newest_schema(fetch, template, first):
    """Самый свежий индекс Google: номер растёт, пока адрес существует."""
    body = fetch(template.format(first))
    schema = first + 1
    while True:
        try:
            body = fetch(template.format(schema))
        except Missing:
            return body
        schema += 1


def google_packages(body):
    """Стабильные пакеты индекса Google: {путь: версия}."""
    packages = {}
    for package in xml.etree.ElementTree.fromstring(body).findall("remotePackage"):
        revision = package.find("revision")
        channel = package.find("channelRef")
        if revision is None or revision.find("preview") is not None:
            continue
        if channel is None or channel.get("ref") != "channel-0":
            continue
        parts = (revision.findtext(part) for part in ("major", "minor", "micro"))
        packages[package.get("path")] = ".".join(part for part in parts if part is not None)
    return packages


def newest_in_path(paths, prefix, suffix=""):
    """Новейшая версия среди путей prefix<версия>suffix; чужие форматы пропускаются."""
    found = []
    for path in paths:
        if path.startswith(prefix) and path.endswith(suffix):
            version = path[len(prefix) : len(path) - len(suffix)]
            if PLAIN_VERSION.fullmatch(version):
                found.append(version)
    if not found:
        raise SourceError(f"no stable package {prefix}*{suffix}")
    return max(found, key=parse_version)


def read_latest(fetch, sources):
    """Новейшие версии для набора источников: {источник: версия}."""
    latest = {}
    repository = None
    images = None
    for source in sorted(sources):
        if source.startswith("github:"):
            latest[source] = github_latest(fetch, source.removeprefix("github:"))
        elif source in ("llvm", "llvm-major"):
            tag = github_latest(fetch, "llvm/llvm-project").removeprefix("llvmorg-")
            latest[source] = tag if source == "llvm" else tag.split(".")[0]
        elif source == "dxc":
            latest[source] = github_latest(fetch, "microsoft/DirectXShaderCompiler")
        elif source == "cmake-minor":
            tag = github_latest(fetch, "Kitware/CMake").removeprefix("v")
            latest[source] = ".".join(tag.split(".")[:2])
        elif source == "vulkan-sdk":
            latest[source] = json.loads(fetch(LUNARG_LATEST))["linux"]
        elif source == "android-image":
            if images is None:
                images = google_packages(newest_schema(fetch, GOOGLE_IMAGES, FIRST_IMAGES_SCHEMA))
            latest[source] = newest_in_path(images, "system-images;android-", ";google_apis;x86_64")
        else:
            if repository is None:
                body = newest_schema(fetch, GOOGLE_REPOSITORY, FIRST_REPOSITORY_SCHEMA)
                repository = google_packages(body)
            if source == "android-emulator":
                if "emulator" not in repository:
                    raise SourceError("no stable emulator package")
                latest[source] = repository["emulator"]
            elif source in GOOGLE_PATH_PREFIXES:
                latest[source] = newest_in_path(repository, GOOGLE_PATH_PREFIXES[source])
            else:
                raise SourceError(f"unknown source {source}")
    return latest


def compare(pins, latest):
    """Строки отчёта (имя, закреплено, новейшая, устарело ли) в порядке имён."""
    rows = []
    for name, (source, pinned) in sorted(pins.items()):
        newest = latest[source]
        rows.append((name, pinned, newest, parse_version(pinned) < parse_version(newest)))
    return rows


def http_fetch(url):
    headers = {"User-Agent": "laiue-version-check"}
    token = os.environ.get("GITHUB_TOKEN")
    if token and url.startswith(GITHUB_API):
        headers["Authorization"] = f"Bearer {token}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as error:
        kind = Missing if error.code == 404 else SourceError
        raise kind(f"{url}: HTTP {error.code}") from error
    except (urllib.error.URLError, TimeoutError) as error:
        raise SourceError(f"{url}: {error}") from error


def report(rows):
    lines = ["| Pin | Pinned | Newest stable | |", "| --- | --- | --- | --- |"]
    for name, pinned, newest, stale in rows:
        lines.append(f"| {name} | {pinned} | {newest} | {'outdated' if stale else 'ok'} |")
    return "\n".join(lines) + "\n"


def check(root, fetch):
    """Возвращает код выхода и текст отчёта."""
    pins, problems = read_pins(root)
    if not pins:
        return 1, f"no pinned versions were found under {root}\n"
    try:
        latest = read_latest(fetch, {source for source, _ in pins.values()})
    except (SourceError, KeyError, ValueError, xml.etree.ElementTree.ParseError) as error:
        return 1, f"a version source failed: {error}\n"
    rows = compare(pins, latest)
    text = report(rows)
    for problem in problems:
        text += f"- {problem}\n"
    return (1 if problems or any(row[3] for row in rows) else 0), text


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    arguments = parser.parse_args()
    code, text = check(arguments.root, http_fetch)
    print(text, end="")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as file:
            file.write(text)
    return code


if __name__ == "__main__":
    sys.exit(main())

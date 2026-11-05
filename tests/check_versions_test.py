"""Проверка версий: все ответы источников подделаны, сеть не нужна."""

import json
import pathlib
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True  # проверка дерева считает __pycache__ мусором
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import check_versions  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMMIT = "0123456789abcdef0123456789abcdef01234567"

WORKFLOW = f"""
env:
  LLVM_VERSION: '23'
  LLVM_WINDOWS_RELEASE: 23.1.5
  DXC_RELEASE: v1.9.2609
  VULKAN_SDK_RELEASE: 1.4.363.1
  ANDROID_NDK_VERSION: 30.0.16248370
  LAIUE_ANDROID_IMAGE: android-37.0
  LAIUE_BUILD_TOOLS: 37.0.0
  LAIUE_CMDLINE_TOOLS: '23.0'
  LAIUE_EMULATOR: 37.2.12
jobs:
  build:
    steps:
      - uses: actions/checkout@{COMMIT} # v7.0.1
"""

CMAKE_LISTS = "cmake_minimum_required(VERSION 4.2...4.4)\n"


def google_package(path, major, minor, micro=None, preview=False, channel="channel-0"):
    micro_element = "" if micro is None else f"<micro>{micro}</micro>"
    preview_element = "<preview>1</preview>" if preview else ""
    return (
        f'<remotePackage path="{path}"><revision><major>{major}</major><minor>{minor}</minor>'
        f'{micro_element}{preview_element}</revision><channelRef ref="{channel}"/></remotePackage>'
    )


REPOSITORY = (
    "<repository>"
    + google_package("ndk;30.0.16248370", 30, 0, 16248370)
    + google_package("ndk;31.0.1", 31, 0, 1, preview=True)
    + google_package("build-tools;37.0.0", 37, 0, 0)
    + google_package("build-tools;38.0.0-rc1", 38, 0, 0, preview=True)
    + google_package("cmdline-tools;23.0", 23, 0)
    + google_package("cmdline-tools;latest", 23, 0)
    + google_package("emulator", 37, 2, 12)
    + google_package("emulator", 38, 1, 1, channel="channel-3")
    + "</repository>"
)

IMAGES = (
    "<images>"
    + google_package("system-images;android-36.1;google_apis;x86_64", 1, 0)
    + google_package("system-images;android-37.0;google_apis;x86_64", 1, 0)
    + google_package("system-images;android-38-ext20;google_apis;x86_64", 1, 0)
    + google_package("system-images;android-CANARY;google_apis;x86_64", 1, 0)
    + "</images>"
)


def github(tag):
    return json.dumps({"tag_name": tag}).encode()


def sources():
    """Ответы, в которых всё закреплённое новейшее."""
    return {
        check_versions.GITHUB_API + "llvm/llvm-project/releases/latest": github("llvmorg-23.1.5"),
        check_versions.GITHUB_API
        + "microsoft/DirectXShaderCompiler/releases/latest": github("v1.9.2609"),
        check_versions.GITHUB_API + "Kitware/CMake/releases/latest": github("v4.4.5"),
        check_versions.GITHUB_API + "actions/checkout/releases/latest": github("v7.0.1"),
        check_versions.LUNARG_LATEST: json.dumps({"linux": "1.4.363.1"}).encode(),
        check_versions.GOOGLE_REPOSITORY.format(4): REPOSITORY.encode(),
        check_versions.GOOGLE_IMAGES.format(5): IMAGES.encode(),
    }


def fake_fetch(answers):
    def fetch(url):
        if url not in answers:
            if url.startswith("https://dl.google.com/"):
                raise check_versions.Missing(url)
            raise check_versions.SourceError(url)
        return answers[url]

    return fetch


class CheckVersionsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        workflows = self.root / ".github" / "workflows"
        workflows.mkdir(parents=True)
        self.workflow = workflows / "build.yml"
        self.workflow.write_text(WORKFLOW, encoding="utf-8")
        (self.root / "CMakeLists.txt").write_text(CMAKE_LISTS, encoding="utf-8")

    def check(self, answers):
        return check_versions.check(self.root, fake_fetch(answers))

    def test_everything_newest_passes(self):
        code, text = self.check(sources())
        self.assertEqual(code, 0, text)
        self.assertNotIn("outdated", text)

    def test_every_pin_is_reported(self):
        _, text = self.check(sources())
        for name in check_versions.ENV_PINS:
            if name.startswith("FOX_"):
                continue
            self.assertIn(name, text)
        self.assertIn("actions/checkout", text)
        self.assertIn("cmake_minimum_required", text)

    def test_a_new_release_fails_and_names_the_pin(self):
        answers = sources()
        url = check_versions.GITHUB_API + "llvm/llvm-project/releases/latest"
        answers[url] = github("llvmorg-23.1.6")
        code, text = self.check(answers)
        self.assertEqual(code, 1)
        self.assertRegex(text, r"LLVM_WINDOWS_RELEASE \| 23\.1\.5 \| 23\.1\.6 \| outdated")
        self.assertRegex(text, r"LLVM_VERSION \| 23 \| 23 \| ok")

    def test_a_new_major_outdates_the_major_pin_too(self):
        answers = sources()
        url = check_versions.GITHUB_API + "llvm/llvm-project/releases/latest"
        answers[url] = github("llvmorg-24.1.0")
        code, text = self.check(answers)
        self.assertEqual(code, 1)
        self.assertRegex(text, r"LLVM_VERSION \| 23 \| 24 \| outdated")

    def test_a_new_action_fails(self):
        answers = sources()
        answers[check_versions.GITHUB_API + "actions/checkout/releases/latest"] = github("v8.0.0")
        code, text = self.check(answers)
        self.assertEqual(code, 1)
        self.assertRegex(text, r"actions/checkout \| v7\.0\.1 \| v8\.0\.0 \| outdated")

    def test_a_new_cmake_minor_fails(self):
        answers = sources()
        answers[check_versions.GITHUB_API + "Kitware/CMake/releases/latest"] = github("v4.5.0")
        code, text = self.check(answers)
        self.assertEqual(code, 1)
        self.assertRegex(text, r"cmake_minimum_required \| 4\.4 \| 4\.5 \| outdated")

    def test_a_new_android_image_fails(self):
        answers = sources()
        answers[check_versions.GOOGLE_IMAGES.format(5)] = IMAGES.replace(
            "android-36.1", "android-37.1"
        ).encode()
        code, text = self.check(answers)
        self.assertEqual(code, 1)
        self.assertRegex(text, r"LAIUE_ANDROID_IMAGE \| 37\.0 \| 37\.1 \| outdated")

    def test_previews_and_other_channels_do_not_count(self):
        code, text = self.check(sources())
        self.assertEqual(code, 0, text)
        self.assertRegex(text, r"LAIUE_BUILD_TOOLS \| 37\.0\.0 \| 37\.0\.0 \| ok")
        self.assertRegex(text, r"ANDROID_NDK_VERSION \| 30\.0\.16248370 \| 30\.0\.16248370 \| ok")
        self.assertRegex(text, r"LAIUE_EMULATOR \| 37\.2\.12 \| 37\.2\.12 \| ok")

    def test_a_newer_index_of_google_is_followed(self):
        answers = sources()
        answers[check_versions.GOOGLE_REPOSITORY.format(5)] = REPOSITORY.replace(
            "build-tools;37.0.0", "build-tools;37.1.0"
        ).encode()
        code, text = self.check(answers)
        self.assertEqual(code, 1)
        self.assertRegex(text, r"LAIUE_BUILD_TOOLS \| 37\.0\.0 \| 37\.1\.0 \| outdated")

    def test_a_pin_ahead_of_the_newest_passes(self):
        self.workflow.write_text(WORKFLOW.replace("23.1.5", "23.2.0"), encoding="utf-8")
        code, text = self.check(sources())
        self.assertEqual(code, 0, text)

    def test_the_oldest_of_two_action_pins_counts(self):
        self.workflow.write_text(
            WORKFLOW + f"      - uses: actions/checkout@{COMMIT} # v6.0.0\n", encoding="utf-8"
        )
        code, text = self.check(sources())
        self.assertEqual(code, 1)
        self.assertRegex(text, r"actions/checkout \| v6\.0\.0 \| v7\.0\.1 \| outdated")

    def test_an_action_on_a_tag_fails(self):
        self.workflow.write_text(WORKFLOW + "      - uses: actions/cache@v6\n", encoding="utf-8")
        code, text = self.check(sources())
        self.assertEqual(code, 1)
        self.assertIn("actions/cache@v6", text)

    def test_a_silent_source_fails(self):
        answers = sources()
        del answers[check_versions.LUNARG_LATEST]
        code, text = self.check(answers)
        self.assertEqual(code, 1)
        self.assertIn("a version source failed", text)

    def test_a_broken_answer_fails(self):
        answers = sources()
        answers[check_versions.LUNARG_LATEST] = b"{}"
        self.assertEqual(self.check(answers)[0], 1)
        answers = sources()
        answers[check_versions.GOOGLE_REPOSITORY.format(4)] = b"<repository"
        self.assertEqual(self.check(answers)[0], 1)

    def test_no_pins_at_all_fails(self):
        self.workflow.unlink()
        (self.root / "CMakeLists.txt").unlink()
        self.assertEqual(self.check(sources())[0], 1)


class RealTreeTest(unittest.TestCase):
    """Переименованная переменная молча выпала бы из проверки."""

    def test_every_pin_of_the_engine_is_found(self):
        pins, problems = check_versions.read_pins(ROOT)
        self.assertEqual(problems, [])
        for name in (
            "LLVM_VERSION",
            "LLVM_WINDOWS_RELEASE",
            "DXC_RELEASE",
            "VULKAN_SDK_RELEASE",
            "ANDROID_NDK_VERSION",
            "LAIUE_ANDROID_IMAGE",
            "LAIUE_BUILD_TOOLS",
            "LAIUE_CMDLINE_TOOLS",
            "LAIUE_EMULATOR",
            "actions/checkout",
            "cmake_minimum_required",
        ):
            self.assertIn(name, pins)


if __name__ == "__main__":
    unittest.main()

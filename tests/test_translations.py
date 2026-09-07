#!/usr/bin/env python3
"""Validate the shipped Traditional Chinese Qt translations."""

from __future__ import annotations

import collections
import pathlib
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[1]
TRANSLATION_FILES = (
    ROOT / "Editor" / "translations" / "Editor_zh_TW.ts",
    ROOT / "DeviceSelector" / "translations" / "DeviceSelector_zh_TW.ts",
    ROOT / "UpdateChecker" / "translations" / "UpdateChecker_zh_TW.ts",
)
EDITOR_TRANSLATION_FILES_BY_LANGUAGE = {
    "de_DE": ROOT / "Editor" / "translations" / "Editor_de.ts",
    "fr_FR": ROOT / "Editor" / "translations" / "Editor_fr.ts",
    "zh_CN": ROOT / "Editor" / "translations" / "Editor_zh_CN.ts",
    "zh_TW": ROOT / "Editor" / "translations" / "Editor_zh_TW.ts",
}
ANALYSIS_RESET_SOURCES = {
    "Reset view",
    "Reset analysis zoom and position",
}
LOUDNESS_REQUIRED_FINISHED_SOURCES = {
    "Linear amplitude",
    "Squared amplitude",
    "Follow dB",
    "APO follow target: %1 dB",
    "APO follow target: unknown (source unavailable)",
    "APO follow target: muted (-inf dB)",
    "Preview uses the volume snapshot from when this window opened.",
    "APO volume-follow attenuation:",
    "Experimental fast engine:",
    "Experimental two-filter approximation. Uses less CPU but may differ noticeably from Full, especially at very low listening levels.",
    "The equal-loudness contour is temporarily disabled while this window is open; APO volume follow remains active when enabled.",
    "The selected playback device must remain the audible Windows default playback device. Muted, zero-volume, unreadable, or mismatched endpoints block the calibration signal.",
}
REMOVED_LOUDNESS_SOURCES = {
    "The bound playback volume could not be read. Reconnect the device, choose another binding, or use manual volume and try again.",
    "The bound playback volume could not be read. The measured level was not applied; reconnect the device, choose another binding, or use manual volume and try again.",
    "Loudness correction is temporarily disabled while this window is open.",
    "The selected playback device is not the Windows default playback device. Test-noise playback is blocked to prevent calibration on the wrong speaker.",
    "Make this device the Windows default to play the signal",
}
VST_STOP_FAILURE_SOURCE = (
    "The out-of-process VST host could not be stopped. This row was left "
    "unchanged. Close the host and try again."
)
VST_STOP_FAILURE_TRANSLATION = (
    "無法停止程序外 VST 主控程式。此列未變更。請關閉主控程式後再試一次。"
)
PLACEHOLDER_PATTERN = re.compile(r"%(?:\d+|n)")
NON_TAIWAN_UI_TERMS = (
    "配置",
    "訪問",
    "出錯",
    "許可權",
    "外掛模組",
    "錄音 / 擷取",
    "全域性",
    "首選項",
    "打開面板",
)


def active_messages(path: pathlib.Path):
    root = ET.parse(path).getroot()
    if root.get("language") != "zh_TW":
        raise AssertionError(f"{path} language is not zh_TW")

    for context in root.findall("context"):
        context_name = context.findtext("name", default="<unknown>")
        for message in context.findall("message"):
            translation = message.find("translation")
            if translation is not None and translation.get("type") in {
                "obsolete",
                "vanished",
            }:
                continue
            yield context_name, message, translation


class TraditionalChineseTranslationTests(unittest.TestCase):
    def test_loudness_catalogs_drop_replaced_copy_and_finish_new_controls(self) -> None:
        for locale, path in EDITOR_TRANSLATION_FILES_BY_LANGUAGE.items():
            root = ET.parse(path).getroot()
            messages = {
                message.findtext("source", default=""): message.find("translation")
                for message in root.findall("./context/message")
                if message.find("translation") is not None
                and message.find("translation").get("type")
                not in {"obsolete", "vanished"}
            }
            with self.subTest(locale=locale):
                self.assertTrue(
                    LOUDNESS_REQUIRED_FINISHED_SOURCES.issubset(messages)
                )
                self.assertTrue(REMOVED_LOUDNESS_SOURCES.isdisjoint(messages))
                for source in LOUDNESS_REQUIRED_FINISHED_SOURCES:
                    translation = messages[source]
                    self.assertNotEqual(translation.get("type"), "unfinished")
                    self.assertTrue("".join(translation.itertext()).strip())

    def test_compact_analysis_and_vst_status_copy_is_shipped(self) -> None:
        removed_vst_wall = (
            "NOTE: The VST module is not universally compatible with all VSTs on the market."
        )
        for locale, path in EDITOR_TRANSLATION_FILES_BY_LANGUAGE.items():
            root = ET.parse(path).getroot()
            messages = {
                message.findtext("source", default=""): message.find("translation")
                for message in root.findall("./context/message")
            }
            with self.subTest(locale=locale):
                self.assertFalse(
                    any(source.startswith(removed_vst_wall) for source in messages)
                )
                self.assertIn("Analysis status", messages)
                translation = messages["Analysis status"]
                self.assertIsNotNone(translation)
                self.assertNotEqual(translation.get("type"), "unfinished")
                self.assertTrue("".join(translation.itertext()).strip())

    def test_analysis_reset_view_is_finished_in_every_editor_locale(self) -> None:
        for locale, path in EDITOR_TRANSLATION_FILES_BY_LANGUAGE.items():
            root = ET.parse(path).getroot()
            self.assertEqual(root.get("language"), locale)
            messages = {
                message.findtext("source", default=""): message.find("translation")
                for message in root.findall("./context/message")
                if message.findtext("source", default="") in ANALYSIS_RESET_SOURCES
            }
            with self.subTest(locale=locale):
                self.assertEqual(set(messages), ANALYSIS_RESET_SOURCES)
                for source, translation in messages.items():
                    self.assertIsNotNone(translation, source)
                    self.assertNotEqual(translation.get("type"), "unfinished", source)
                    self.assertTrue("".join(translation.itertext()).strip(), source)

    def test_loudness_migration_choices_are_present(self) -> None:
        editor_root = ET.parse(TRANSLATION_FILES[0]).getroot()
        translated_sources = {
            message.findtext("source", default="")
            for message in editor_root.findall("./context/message")
        }
        expected = {
            "This unmarked entry could be an original shelf profile or a previously released formula profile. It remains unchanged and bypassed until you choose an interpretation.",
            "Keep existing formula values",
            "Convert original shelf profile",
        }
        self.assertTrue(expected.issubset(translated_sources))

    def test_outproc_vst_stop_failure_is_shipped_in_traditional_chinese(self) -> None:
        vst_gui = (
            ROOT / "Editor" / "guis" / "VSTPluginFilterGUI.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn(f'tr("{VST_STOP_FAILURE_SOURCE}")', vst_gui)

        editor_root = ET.parse(TRANSLATION_FILES[0]).getroot()
        messages = {
            message.findtext("source", default=""): message.find("translation")
            for message in editor_root.findall(
                "./context[name='VSTPluginFilterGUI']/message"
            )
        }
        self.assertIn(VST_STOP_FAILURE_SOURCE, messages)
        translation = messages[VST_STOP_FAILURE_SOURCE]
        self.assertIsNotNone(translation)
        self.assertNotEqual(translation.get("type"), "unfinished")
        self.assertEqual(
            "".join(translation.itertext()), VST_STOP_FAILURE_TRANSLATION
        )

    def test_qt_base_traditional_chinese_is_not_simplified_alias(self) -> None:
        for app in ("Editor", "DeviceSelector", "UpdateChecker"):
            translations = ROOT / app / "translations"
            traditional = (translations / "qtbase_zh_TW.qm").read_bytes()
            simplified = (translations / "qtbase_zh_CN.qm").read_bytes()
            self.assertNotEqual(
                traditional,
                simplified,
                f"{app} embeds the Simplified Chinese Qt catalog as zh_TW",
            )

    def test_every_active_message_is_translated(self) -> None:
        for path in TRANSLATION_FILES:
            for context, message, translation in active_messages(path):
                source = message.findtext("source", default="")
                with self.subTest(file=path.name, context=context, source=source):
                    self.assertIsNotNone(translation)
                    self.assertNotEqual(translation.get("type"), "unfinished")
                    self.assertTrue("".join(translation.itertext()).strip())

    def test_placeholders_are_preserved(self) -> None:
        for path in TRANSLATION_FILES:
            for context, message, translation in active_messages(path):
                source = message.findtext("source", default="")
                translated = "".join(translation.itertext())
                with self.subTest(file=path.name, context=context, source=source):
                    self.assertEqual(
                        collections.Counter(PLACEHOLDER_PATTERN.findall(source)),
                        collections.Counter(PLACEHOLDER_PATTERN.findall(translated)),
                    )

    def test_rich_text_translations_are_well_formed(self) -> None:
        for path in TRANSLATION_FILES:
            for context, message, translation in active_messages(path):
                source = message.findtext("source", default="")
                if not source.lstrip().startswith("<html"):
                    continue
                translated = "".join(translation.itertext())
                with self.subTest(file=path.name, context=context, source=source):
                    ET.fromstring(translated)

    def test_active_ui_uses_taiwan_terminology(self) -> None:
        for path in TRANSLATION_FILES:
            for context, message, translation in active_messages(path):
                source = message.findtext("source", default="")
                translated = "".join(translation.itertext())
                with self.subTest(file=path.name, context=context, source=source):
                    for term in NON_TAIWAN_UI_TERMS:
                        self.assertNotIn(term, translated)


if __name__ == "__main__":
    unittest.main()

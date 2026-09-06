"""Contracts for out-of-process VST parameter metadata transport."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class OutProcVSTConfigV4Tests(unittest.TestCase):
    def test_v4_is_bounded_and_reads_every_previous_version(self) -> None:
        header = read("outproc/OutProcVSTConfig.h")

        self.assertIn("OUTPROC_VST_CONFIG_VERSION = 4", header)
        self.assertIn("OutProcVSTParameterDescriptor", header)
        self.assertIn("parameterDescriptors", header)
        for field in (
            "api",
            "stableId",
            "name",
            "stepCount",
            "normalizedValue",
            "readOnly",
            "hidden",
        ):
            with self.subTest(field=field):
                self.assertRegex(header, rf"\b{field}\b")

        for version in (1, 2, 3):
            with self.subTest(version=version):
                self.assertRegex(header, rf"version\s*==\s*{version}")

        self.assertRegex(
            header,
            r"descriptorCount\s*>\s*OUTPROC_VST_CONFIG_MAX_DESCRIPTOR_COUNT",
        )
        self.assertRegex(
            header,
            r"descriptor\.name\.size\(\)\s*>\s*OUTPROC_VST_CONFIG_MAX_PARAMETER_NAME_LENGTH",
        )
        self.assertIn("OUTPROC_VST_CONFIG_MAX_DESCRIPTOR_COUNT = 16384", header)
        self.assertIn("OUTPROC_VST_CONFIG_MAX_DESCRIPTOR_NAME_BYTES", header)
        self.assertIn("descriptorNameBytes", header)
        self.assertIn("stream.peek()", header)

    def test_v4_malformed_descriptor_metadata_fails_closed(self) -> None:
        header = read("outproc/OutProcVSTConfig.h")
        v4_reader = header[header.index("if (version >= 4)") :]

        self.assertIn("descriptorCount > OUTPROC_VST_CONFIG_MAX_DESCRIPTOR_COUNT", v4_reader)
        self.assertRegex(v4_reader, r"api\s*>.*OutProcVSTParameterApi::VST3")
        self.assertIn("readOnly > 1", v4_reader)
        self.assertIn("hidden > 1", v4_reader)
        self.assertIn("!std::isfinite(descriptor.normalizedValue)", v4_reader)
        self.assertIn("OUTPROC_VST_CONFIG_MAX_DESCRIPTOR_NAME_BYTES", v4_reader)

    def test_gui_host_publishes_real_plugin_parameter_descriptors(self) -> None:
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")

        self.assertIn("getParameterDescriptors()", host)
        self.assertIn("config.parameterDescriptors", host)
        self.assertRegex(
            host,
            re.compile(
                r"writeGuiEffectState\(\).*?captureParameterDescriptors\(.*?"
                r"config\.parameterDescriptors.*?OutProcWriteVSTConfig",
                re.S,
            ),
        )

    def test_editor_caches_and_synchronizes_outproc_descriptors(self) -> None:
        header = read("Editor/guis/VSTPluginFilterGUI.h")
        source = read("Editor/guis/VSTPluginFilterGUI.cpp")

        self.assertIn("outProcParameterDescriptors", header)
        self.assertIn("config.parameterDescriptors", source)
        terminate = source[
            source.index("bool VSTPluginFilterGUI::terminateOutProcPanel") :
            source.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]
        self.assertIn("outProcParameterDescriptors =", terminate)
        self.assertRegex(
            source,
            re.compile(
                r"on_idle\(\).*?outProcParameterDescriptors\s*=",
                re.S,
            ),
        )
        available = source[
            source.index("VSTPluginFilterGUI::availableMidiParameters") :
            source.index("void VSTPluginFilterGUI::updateMidiButton")
        ]
        self.assertIn("outProcParameterDescriptors", available)
        self.assertNotIn('L"Parameter #"', available)

    def test_editor_row_remains_the_single_midi_configuration_owner(self) -> None:
        source = read("Editor/guis/VSTPluginFilterGUI.cpp")
        open_panel = source[
            source.index("void VSTPluginFilterGUI::openOutProcPanel") :
            source.index("bool VSTPluginFilterGUI::signalOutProcPanel")
        ]
        terminate = source[
            source.index("bool VSTPluginFilterGUI::terminateOutProcPanel") :
            source.index("void VSTPluginFilterGUI::applyDialog")
        ]
        idle = source[
            source.index("void VSTPluginFilterGUI::on_idle") :
            source.index("void VSTPluginFilterGUI::onAutomate")
        ]

        self.assertIn("config.midiConfig = midiConfig", open_panel)
        self.assertNotIn("midiConfig = updatedConfig.midiConfig", terminate)
        self.assertNotIn("midiConfig = updatedConfig.midiConfig", idle)
        self.assertIn("idleTimer.stop()", terminate)
        self.assertIn("recoveredStateChanged = chunkData != updatedConfig.chunkData", terminate)
        self.assertIn("*stateChanged = recoveredStateChanged", terminate)

        class_switch = source[
            source.index("void VSTPluginFilterGUI::on_vst3ClassComboBox_currentIndexChanged") :
            source.index("void VSTPluginFilterGUI::openOutProcPanel")
        ]
        self.assertLess(
            class_switch.index("ensureOutProcPanelStopped(false)"),
            class_switch.index("outProcParameterDescriptors.clear()"),
        )


if __name__ == "__main__":
    unittest.main()

"""Low-level contracts for realtime-safe VST MIDI control."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class VSTMidiRuntimeLowLevelTests(unittest.TestCase):
    def test_vst3_midi_descriptors_require_automatable_parameters(self) -> None:
        source = read("helpers/VSTPluginInstance.cpp")
        rebuild = source[
            source.index("void VSTPluginInstance::rebuildParameterDescriptors") :
            source.index("parameterDescriptors.reserve(max", source.index("void VSTPluginInstance::rebuildParameterDescriptors"))
        ]

        self.assertIn("ParameterInfo::kCanAutomate", rebuild)
        self.assertRegex(
            rebuild,
            re.compile(
                r"descriptor\.readOnly\s*=.*kIsReadOnly.*\|\|.*kCanAutomate",
                re.S,
            ),
        )

    def test_codec_is_versioned_and_bounded(self) -> None:
        codec = read("helpers/VSTMidiBindingCodec.h")
        self.assertIn("VST_MIDI_CONFIG_VERSION", codec)
        self.assertIn("VST_MIDI_MAX_BINDINGS", codec)
        self.assertIn("VST3ParamID", codec)
        self.assertIn("VST2ParameterIndex", codec)

    def test_runtime_rejects_hidden_midi_targets(self) -> None:
        source = read("helpers/WinMidiInput.cpp")
        configure = source[
            source.index("bool VSTMidiRuntime::configure") :
            source.index("void VSTMidiRuntime::stop")
        ]

        self.assertRegex(configure, r"parameter\.readOnly\s*\|\|\s*parameter\.hidden")
        self.assertIn("if (bindings.empty())\n\t\treturn false;", configure)

    def test_codec_rejects_ambiguous_overlapping_sources(self) -> None:
        codec = read("helpers/VSTMidiBindingCodec.h")
        self.assertIn("bindingsConflict", codec)
        self.assertIn("sourcesConflict", codec)
        self.assertIn("hasConflictingSource", codec)
        self.assertRegex(codec, r"channel\s*==\s*0xff")

    def test_winmm_callback_only_publishes_to_fixed_queue(self) -> None:
        source = read("helpers/WinMidiInput.cpp")
        callback = re.search(
            r"static\s+void\s+CALLBACK\s+midiCallback\s*\([^)]*\)\s*\{(.*?)\n\t\}",
            source,
            re.S,
        )
        self.assertIsNotNone(callback)
        body = callback.group(1)
        self.assertIn("fanOutMessage", body)
        for forbidden in (
            "midiInOpen",
            "midiInStart",
            "midiInStop",
            "midiInReset",
            "midiInClose",
            "new ",
            "delete ",
            "std::vector",
            "WaitForSingleObject",
            "SetEvent",
            "EnterCriticalSection",
        ):
            with self.subTest(token=forbidden):
                self.assertNotIn(forbidden, body)

    def test_process_broker_fans_out_one_physical_input(self) -> None:
        header = read("helpers/WinMidiInput.h")
        source = read("helpers/WinMidiInput.cpp")
        self.assertIn("class WinMidiInputBroker", source)
        self.assertIn("std::array<std::atomic<WinMidiInput*>, subscriberCapacity>", source)
        self.assertIn("stableIdentityMatches(identity, sessions[i].identity)", source)
        stable_match = re.search(
            r"bool\s+stableIdentityMatches\s*\([^)]*\)\s*\{(.*?)\n\t\}",
            source,
            re.S,
        )
        self.assertIsNotNone(stable_match)
        self.assertNotIn("driverVersion", stable_match.group(1))
        self.assertIn("session.subscribers[subscriberIndex].store(input", source)
        self.assertEqual(source.count("midiInOpen("), 1)
        self.assertIn("VSTMidiConnectionState::Busy", source)
        self.assertIn("WinMidiInput::reconnectIntervalMs", source)
        self.assertIn("connectionState()", header)
        self.assertIn("BrokerCapacityExceeded", header)

        fanout = re.search(
            r"void\s+fanOutMessage\s*\([^)]*\)\s*\{(.*?)\n\t\}", source, re.S
        )
        self.assertIsNotNone(fanout)
        for forbidden in ("new ", "delete ", "std::vector", "midiIn", "WaitForSingleObject", "mutex"):
            with self.subTest(token=forbidden):
                self.assertNotIn(forbidden, fanout.group(1))

    def test_reconnect_fallback_cannot_jump_to_same_named_hardware(self) -> None:
        source = read("helpers/WinMidiInput.cpp")
        fallback = re.search(
            r"if \(foldedDeviceName\(device\.identity\.name\).*?fallback = &device;",
            source,
            re.S,
        )
        self.assertIsNotNone(fallback)
        self.assertIn("manufacturerId", fallback.group(0))
        self.assertIn("productId", fallback.group(0))

    def test_vst3_automation_uses_preallocated_spsc_and_changes(self) -> None:
        header = read("helpers/VSTPluginInstance.h")
        source = read("helpers/VSTPluginInstance.cpp")
        self.assertNotIn("pendingVST3ParameterChanges", header)
        self.assertIn("VST3ParameterChangeQueue", header)
        self.assertIn("vst3InputParameterChanges", header)
        for process_name in ("processDoubleReplacing", "processReplacing"):
            process = re.search(
                rf"void\s+VSTPluginInstance::{process_name}\s*\([^)]*\)\s*\{{(.*?)\n\}}",
                source,
                re.S,
            )
            self.assertIsNotNone(process)
            self.assertNotIn("ParameterChanges inputChanges", process.group(1))

    def test_vst3_parameter_changes_are_prewarmed_for_one_point_per_parameter(self) -> None:
        source = read("helpers/VSTPluginInstance.cpp")
        sdk = read("third_party/vst3sdk/public.sdk/source/vst/hosting/parameterchanges.cpp")
        self.assertIn("prewarmVST3InputParameterChanges();", source)
        prewarm = re.search(
            r"void\s+VSTPluginInstance::prewarmVST3InputParameterChanges\(\)\s*\{(.*?)\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(prewarm)
        self.assertIn("addParameterData", prewarm.group(1))
        self.assertIn("addPoint(0,", prewarm.group(1))
        self.assertIn("clearQueue", prewarm.group(1))
        prepare = re.search(
            r"void\s+VSTPluginInstance::prepareVST3InputParameterChanges\(\)\s*\{(.*?)\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(prepare)
        self.assertIn("findVST3Parameter(change.id)", prepare.group(1))
        self.assertIn("addPoint(0,", prepare.group(1))
        self.assertRegex(sdk, r"constexpr\s+int32\s+kQueueReservedPoints\s*=\s*5")
        self.assertIn("setMaxParameters (maxParameters)", sdk)

    def test_restored_state_refreshes_toggle_seed_before_runtime_configure(self) -> None:
        instance = read("helpers/VSTPluginInstance.cpp")
        filter_source = read("filters/VSTPluginFilter.cpp")
        write = instance[
            instance.index("void VSTPluginInstance::writeToEffect") :
            instance.index("void VSTPluginInstance::readFromEffect")
        ]
        self.assertGreaterEqual(write.count("rebuildParameterDescriptors();"), 2)
        prepare = filter_source[
            filter_source.index("void VSTPluginFilter::prepareForProcessing") :
            filter_source.index("#pragma AVRT_CODE_BEGIN")
        ]
        initialize = filter_source[
            filter_source.index("std::vector<std::wstring> VSTPluginFilter::initialize") :
            filter_source.index("void VSTPluginFilter::prepareForProcessing")
        ]
        self.assertLess(prepare.index("writeToEffect"), prepare.index("effect->prepareForProcessing"))
        self.assertLess(initialize.index("prepareForProcessing"), initialize.index("midiRuntime.configure"))

    def test_factory_destroys_placement_new_filter_as_concrete_type(self) -> None:
        factory = read("Editor/guis/VSTPluginFilterGUIFactory.cpp")
        self.assertNotIn("f->~IFilter();", factory)
        self.assertIn("vstFilter->~VSTPluginFilter();", factory)
        self.assertIn("MemoryHelper::free(vstFilter);", factory)

    def test_mapping_dialog_surfaces_nonblocking_broker_states(self) -> None:
        header = read("Editor/guis/VSTMidiMappingDialog.h")
        source = read("Editor/guis/VSTMidiMappingDialog.cpp")
        self.assertIn("updateConnectionStatus", header)
        self.assertIn("midiInput.connectionState()", source)
        self.assertIn("VSTMidiConnectionState::Connected", source)
        self.assertIn("VSTMidiConnectionState::Busy", source)
        self.assertIn("VSTMidiConnectionState::DeviceUnavailable", source)
        self.assertIn("VSTMidiConnectionState::BrokerCapacityExceeded", source)
        self.assertIn("automatic retry", source)
        self.assertIn("Existing mappings are unchanged", source)
        self.assertIn("cancel without saving", source)
        self.assertIn("midiPollTimer.start();", source)
        self.assertIn("VSTMidiBindingCodec::sourcesConflict(current, binding)", source)
        select_device = source[
            source.index("bool VSTMidiMappingDialog::selectConfiguredDevice") :
            source.index("const VSTParameterDescriptor* VSTMidiMappingDialog::selectedParameter")
        ]
        self.assertIn("sameDevice", select_device)
        self.assertIn("sameStableDevice", select_device)
        self.assertIn("configuration.device = devices[index].identity", select_device)
        status = re.search(
            r"void\s+VSTMidiMappingDialog::updateConnectionStatus\([^)]*\)\s*\{(.*?)\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(status)
        for forbidden in ("WaitForSingleObject", "Sleep(", "midiInOpen", "processEvents"):
            with self.subTest(token=forbidden):
                self.assertNotIn(forbidden, status.group(1))


if __name__ == "__main__":
    unittest.main()

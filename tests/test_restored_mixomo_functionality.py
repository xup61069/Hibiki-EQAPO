#!/usr/bin/env python3
"""Regression contracts for restored Mixomo controls and runtime wiring."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class RestoredMixomoFunctionalityTests(unittest.TestCase):
    def test_chorus_unit_bearing_and_legacy_parameters_use_the_same_scan(self) -> None:
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")
        factory = read("filters/ChorusFilterFactory.cpp")

        self.assertIn(
            'QString("Rate %1 Hz Depth %2 ms Mix %3 % Feedback %4 %")', gui
        )
        self.assertRegex(factory, r"for \([^\n]+; \+\+i\)")
        self.assertNotIn("i += 2", factory)
        for key in ("Rate", "Depth", "Mix", "Feedback"):
            self.assertIn(f'parts[i] == L"{key}"', factory)

    def test_reverb_percent_parameters_do_not_shift_runtime_fields(self) -> None:
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")
        factory = read("filters/ReverbFilterFactory.cpp")

        self.assertIn(
            'QString("RoomSize %1 % Damping %2 % Wet %3 % Dry %4 % Width %5 %")',
            gui,
        )
        self.assertRegex(factory, r"for \([^\n]+; \+\+i\)")
        self.assertNotIn("i += 2", factory)
        for key in ("RoomSize", "Damping", "Wet", "Dry", "Width"):
            self.assertIn(f'parts[i] == L"{key}"', factory)

    def test_zero_wet_reverb_still_applies_the_configured_dry_gain(self) -> None:
        source = read("filters/ReverbFilter.cpp")
        zero_wet = source[
            source.index("if (wet <= 0.0)") : source.index(
                "const double feedback", source.index("if (wet <= 0.0)")
            )
        ]

        self.assertIn("if (dry == 1.0)", zero_wet)
        self.assertIn("memcpy(output[c], input[c]", zero_wet)
        self.assertIn("output[c][frame] = input[c][frame] * dry;", zero_wet)

    def test_reverb_dry_control_preserves_the_documented_150_percent_range(self) -> None:
        helper = read("filters/AudioToolsHelper.h")
        source = read("filters/ReverbFilter.cpp")

        self.assertIn("percentToUnit(double value, double maximum = 1.0)", helper)
        self.assertIn("(std::min)(maximum, value / 100.0)", helper)
        self.assertIn("dry(AudioTools::percentToUnit(dryPercent, 1.5))", source)

    def test_reverb_width_changes_only_the_wet_return(self) -> None:
        source = read("filters/ReverbFilter.cpp")

        self.assertIn("const double drySignal = in * dry;", source)
        self.assertIn("const double wetSignal = acc * wet;", source)
        self.assertIn("const double wetMid = 0.5 * (wetLeft + wetRight);", source)
        self.assertIn(
            "output[0][frame] = dryLeft + wetMid + (wetLeft - wetMid) * width;",
            source,
        )
        self.assertNotIn("const double left = output[0][frame];", source)

    def test_vu_meter_id_round_trips_and_routes_both_panels(self) -> None:
        header = read("Editor/guis/AudioToolFilterGUIFactory.h")
        source = read("Editor/guis/AudioToolFilterGUIFactory.cpp")

        self.assertIn('QString meterId = "default";', header)
        self.assertIn(
            'const QString configuredMeterId = pairedTokenValue(parameters, "MeterId", QString()).trimmed();',
            source,
        )
        self.assertEqual(source.count("setMeterId(meterId)"), 2)
        self.assertNotIn('setMeterId("default")', source)
        self.assertIn('QString("MeterId %1 Channels %2 RMS', source)
        self.assertIn(".arg(quotedConfigToken(meterId))", source)
        self.assertEqual(source.count("vuMeterObjectName(meterId)"), 2)

    def test_vu_meter_parser_and_object_id_are_canonical_on_both_sides(self) -> None:
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")
        runtime = read("filters/VUMeterFilter.cpp")
        factory = read("filters/VUMeterFilterFactory.cpp")
        protocol = read("filters/VUMeterProtocol.h")

        self.assertIn("for (size_t i = 0; i + 1 < parts.size(); i += 2)", gui)
        self.assertIn("VUMeterNormalizedId(value.toStdWString())", gui)
        self.assertIn(": meterId(VUMeterNormalizedId(meterId))", runtime)
        self.assertIn("const wstring key = StringHelper::toLowerCase(parts[i]);", factory)
        self.assertIn('if (key == L"meterid")', factory)
        self.assertIn("const std::wstring normalized = VUMeterTrimToken(value);", protocol)
        self.assertIn('Global\\\\EqAPO_VUMeter_v3_', gui)
        self.assertIn('Global\\\\EqAPO_VUMeter_v3_', runtime)

    def test_vu_meter_shared_state_uses_a_consistent_seqlock_snapshot(self) -> None:
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")
        runtime = read("filters/VUMeterFilter.cpp")
        protocol = read("filters/VUMeterProtocol.h")

        self.assertIn("static const std::uint32_t VUMETER_VERSION = 3;", protocol)
        self.assertIn("sizeof(VUMeterSharedData) == 888", protocol)
        self.assertIn("InterlockedCompareExchange64(sequence, observed + 1, observed)", runtime)
        self.assertIn("endSharedWrite();", runtime)
        self.assertIn("InterlockedIncrement64(sequenceAddress(shared));", runtime)
        self.assertIn("static bool readVUMeterSnapshot", gui)
        self.assertIn("const LONG64 before = InterlockedCompareExchange64(sequence, 0, 0);", gui)
        self.assertIn("memcpy(&snapshot, shared, sizeof(snapshot));", gui)
        self.assertIn("if (before == after && (after & 1) == 0)", gui)
        self.assertNotIn("memcpy(&data, shared, sizeof(data));", gui)
        self.assertEqual(gui.count("\t\tvalid = false;\n\t\tif (!connectMeter())"), 2)

    def test_vu_meter_existing_mapping_is_not_destroyed_and_writer_handoff_is_owned(self) -> None:
        header = read("filters/VUMeterFilter.h")
        runtime = read("filters/VUMeterFilter.cpp")

        create = runtime[runtime.index("void VUMeterFilter::openSharedData()") : runtime.index("void VUMeterFilter::cleanup()")]
        self.assertIn("Page-file-backed mappings start zeroed", create)
        self.assertNotIn("memset(shared", create)
        self.assertIn("HANDLE ownerMutex = NULL;", header)
        self.assertIn("HANDLE ownerThread = NULL;", header)
        self.assertIn("volatile LONG activeUsers = 0;", header)
        self.assertIn("CreateMutexW", create)
        self.assertIn("CreateThread(NULL, 0, ownerThreadProc", create)
        self.assertIn("WaitForMultipleObjects(2, waits, FALSE, INFINITE)", runtime)
        self.assertIn("WAIT_ABANDONED_0 + 1", runtime)
        self.assertIn("ReleaseMutex(ownerMutex);", runtime)
        self.assertIn("InterlockedExchange(&ownerActive, 0);", runtime)
        self.assertIn("while (InterlockedCompareExchange(&activeUsers, 0, 0) != 0)", runtime)
        self.assertIn("SwitchToThread();", runtime)

        process = runtime[runtime.index("void VUMeterFilter::process") :]
        self.assertNotIn("WaitFor", process)
        self.assertNotIn("CreateMutex", process)

        begin = runtime[runtime.index("bool VUMeterFilter::beginSharedWrite()") : runtime.index("void VUMeterFilter::endSharedWrite()")]
        self.assertLess(begin.index("InterlockedIncrement(&activeUsers);"), begin.index("InterlockedCompareExchange(&ownerActive, 0, 0)"))
        self.assertGreaterEqual(begin.count("InterlockedDecrement(&activeUsers);"), 3)

    def test_vu_meter_owner_recovers_an_abandoned_odd_sequence_before_publishing(self) -> None:
        runtime = read("filters/VUMeterFilter.cpp")
        owner = runtime[runtime.index("DWORD VUMeterFilter::runOwnerThread()") : runtime.index("bool VUMeterFilter::beginSharedWrite()")]

        self.assertIn("The mutex proves that the previous writer is gone", owner)
        self.assertIn("InterlockedExchange64(sequence, abandonedSequence | 1);", owner)
        self.assertNotIn("if ((abandonedSequence & 1) != 0)", owner)
        self.assertLess(owner.index("InterlockedExchange64(sequence, abandonedSequence | 1);"), owner.index("shared->magic = VUMETER_MAGIC;"))
        self.assertLess(owner.index("resetMeasurements();"), owner.index("InterlockedExchange(&ownerActive, 1);"))

    def test_vu_meter_reset_is_atomic_and_clears_all_accumulators(self) -> None:
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")
        runtime = read("filters/VUMeterFilter.cpp")

        reset = runtime[runtime.index("void VUMeterFilter::resetMeasurements()") : runtime.index("#pragma AVRT_CODE_BEGIN")]
        self.assertIn("InterlockedIncrement(vuMeterResetAddress(shared));", gui)
        self.assertIn("InterlockedExchange(resetRequestAddress(shared), 0)", runtime)
        for field in (
            "momentaryMean = 0.0;",
            "shortMean = 0.0;",
            "integratedMean = 0.0;",
            "integratedWeight = 0.0;",
            "channelMomentaryMean[c] = 0.0;",
            "channelShortMean[c] = 0.0;",
            "channelIntegratedMean[c] = 0.0;",
            "channelIntegratedWeight[c] = 0.0;",
            "shared->peak[c] = 0.0;",
            "shared->peakHold[c] = 0.0;",
            "shared->rms[c] = 0.0;",
            "shared->clip[c] = 0;",
            "shared->lufsMomentary = -INFINITY;",
            "shared->lufsShortTerm = -INFINITY;",
            "shared->lufsIntegrated = -INFINITY;",
        ):
            self.assertIn(field, reset)
        process = runtime[runtime.index("void VUMeterFilter::process") :]
        self.assertLess(process.index("beginSharedWrite()"), process.index("resetMeasurements();"))
        self.assertLess(process.index("resetMeasurements();"), process.index("endSharedWrite();"))

    def test_vu_meter_custom_channel_selectors_round_trip_without_hidden_loss(self) -> None:
        header = read("Editor/guis/AudioToolFilterGUIFactory.h")
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")

        self.assertIn("QString originalChannelSelector", header)
        self.assertIn("QStringList preservedChannelTokens", header)
        self.assertIn("bool channelSelectionEdited", header)
        self.assertIn('QStringLiteral("[,;\\\\s]+")', gui)
        self.assertIn("if (commandName == \"VUMeter\" && !channelSelectionEdited)", gui)
        self.assertIn("selected.append(preservedChannelTokens);", gui)
        self.assertIn('? (commandName == "VUMeter" ? "none" : "all")', gui)
        self.assertIn('.arg(quotedConfigToken(selectedChannels()))', gui)

        # Exercise representative selectors against the same visible-channel boundary.
        standard = {"L", "R", "C", "LFE", "RL", "RR", "SL", "SR"}
        cases = {
            "9": ["9"],
            "16": ["16"],
            "RC": ["RC"],
            "L, R, 9, RC": ["9", "RC"],
        }
        for selector, expected_hidden in cases.items():
            tokens = [token for token in re.split(r"[,;\s]+", selector) if token]
            hidden = [token for token in tokens if token.upper() not in standard]
            self.assertEqual(hidden, expected_hidden)

    def test_vu_meter_uses_one_quote_codec_and_one_cross_process_id_codec(self) -> None:
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")
        runtime = read("filters/VUMeterFilter.cpp")
        protocol = read("filters/VUMeterProtocol.h")

        self.assertIn("StringHelper::splitQuoted(parameters.toStdWString()", gui)
        self.assertNotIn('QRegularExpression re("\\\"([^\\\"]*)', gui)
        self.assertIn("VUMeterCanonicalId(normalizedMeterId(value).toStdWString())", gui)
        self.assertIn("VUMeterCanonicalId(meterId)", runtime)
        self.assertIn("inline std::wstring VUMeterCanonicalId", protocol)
        self.assertIn("codeUnit >> 12", protocol)
        self.assertIn("codeUnit == L'-'", protocol)

    def test_new_vu_meters_get_unique_ids_without_rewriting_explicit_ids(self) -> None:
        source = read("Editor/guis/AudioToolFilterGUIFactory.cpp")
        constructor = source[
            source.index("AudioToolFilterGUI::AudioToolFilterGUI") : source.index(
                "AudioToolFilterGUI::~AudioToolFilterGUI"
            )
        ]
        template = source[
            source.index("VUMeterFilterGUIFactory::createFilterTemplates") :
            source.index("VUMeterFilterGUIFactory::createFilterGUI")
        ]

        self.assertNotIn("MeterId default", template)
        self.assertIn("const bool generatedMeterId = configuredMeterId.isEmpty();", constructor)
        self.assertIn("QUuid::createUuid().toString(QUuid::WithoutBraces)", constructor)
        self.assertIn(": normalizedMeterId(configuredMeterId);", constructor)
        self.assertIn("QTimer::singleShot(0, this", constructor)

    def test_cloned_vu_meters_get_independent_shared_memory_ids(self) -> None:
        table = read("Editor/FilterTable.cpp")
        clone = table[table.index("QString cloneFilterLineWithNewInstanceIds") :]

        self.assertIn("tokenizeFilterParameters(parameters)", clone)
        self.assertIn('command == QStringLiteral("VUMeter")', clone)
        self.assertIn('clonedText.insert(separator + 1, " MeterId " +', clone)
        self.assertIn("QUuid::createUuid()", clone)
        self.assertIn('QStringLiteral("MeterId"), Qt::CaseInsensitive', clone)
        self.assertIn("removeFilterParameterPairs(", clone)
        self.assertIn("i < tokens.size(); i += 2", clone)
        self.assertIn("return clonedText;", clone)

        # The native restored-tools fixture exercises compact, duplicate,
        # empty, and dangling MeterId tokens. Keeping the new authoritative
        # pair first prevents a malformed trailing key from consuming it.
        self.assertIn(
            'clonedLines[11].startsWith(QStringLiteral("VUMeter: MeterId "))',
            read("Editor/MainWindow.cpp"),
        )

    def test_vu_meter_passes_all_endpoint_channels_while_metering_is_bounded(self) -> None:
        header = read("filters/VUMeterFilter.h")
        source = read("filters/VUMeterFilter.cpp")

        self.assertIn("unsigned totalChannelCount = 0;", header)
        self.assertIn("unsigned measuredChannelCount = 0;", header)
        self.assertIn(
            "totalChannelCount = static_cast<unsigned>(channelNames.size());", source
        )
        self.assertIn(
            "measuredChannelCount = min<unsigned>(totalChannelCount, VUMETER_MAX_CHANNELS);",
            source,
        )
        self.assertIn("shared->channelCount = measuredChannelCount;", source)
        self.assertIn("c < totalChannelCount", source)
        self.assertEqual(source.count("channel >= measuredChannelCount"), 2)
        self.assertNotIn("channelCount = min<unsigned>", source)

    def test_vu_meter_does_not_expose_nonfunctional_loudness_standards_or_true_peak(self) -> None:
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")

        self.assertIn("lufsStandardComboBox->setEnabled(false);", gui)
        self.assertIn("Compatibility label only.", gui)
        self.assertIn('"Sample peak"', gui)
        self.assertIn('"Sample peak %1 dBFS', gui)
        self.assertNotIn('"True peak', gui)

    def test_vu_panel_translation_contexts_are_runtime_reachable(self) -> None:
        gui = read("Editor/guis/AudioToolFilterGUIFactory.cpp")

        self.assertEqual(
            gui.count('QCoreApplication::translate("VUMeterPanel",'), 2
        )
        self.assertEqual(
            gui.count('QCoreApplication::translate("VUMeterStatsPanel",'), 3
        )

    def test_headphone_fir_export_accepts_unicode_paths(self) -> None:
        source = read("Editor/guis/HeadphoneCalibrationFilterGUIFactory.cpp")

        self.assertIn(
            "sf_wchar_open(path.toStdWString().c_str(), SFM_WRITE, &info)", source
        )
        self.assertNotIn("sf_open(path.toStdString().c_str()", source)

    def test_restored_editors_bound_width_and_scroll_their_own_content(self) -> None:
        parametric = read("Editor/guis/ParametricEQFilterGUIFactory.cpp")
        headphone = read("Editor/guis/HeadphoneCalibrationFilterGUIFactory.cpp")

        for source, legacy_widths in (
            (parametric, ("setMinimumWidth(1480)", "setMinimumWidth(1180)")),
            (headphone, ("setMinimumWidth(1180)",)),
        ):
            self.assertIn("setMinimumWidth(0)", source)
            self.assertIn("QAbstractScrollArea::AdjustIgnored", source)
            self.assertIn("Qt::ScrollBarAsNeeded", source)
            for legacy_width in legacy_widths:
                self.assertNotIn(legacy_width, source)

        self.assertIn("headphoneCalibrationScrollArea", headphone)
        self.assertIn("QSizePolicy::Ignored", parametric)
        self.assertIn("QSizePolicy::Ignored", headphone)
        self.assertNotRegex(parametric, re.compile(r"Button->setMinimumWidth\("))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Static contracts for the Voicemeeter DAW monitoring path."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


CLIENT = read("VoicemeeterClient/VoicemeeterClient.cpp")
APO_INFO = read("VoicemeeterAPOInfo.cpp")


def section(source: str, start: str, end: str) -> str:
    return source[source.index(start) : source.index(end, source.index(start))]


class VoicemeeterMonitorContractTests(unittest.TestCase):
    def test_output_a1_is_processed_by_the_output_bus_insert(self) -> None:
        init = section(
            CLIENT,
            "void VoicemeeterClient::initSoftware()",
            "void VoicemeeterClient::detectVoicemeeterType()",
        )
        callback = section(
            CLIENT,
            "void VoicemeeterClient::handle(long nCommand",
            "void VoicemeeterClient::initSoftware()",
        )
        detection = section(
            CLIENT,
            "void VoicemeeterClient::detectVoicemeeterType()",
            "void VoicemeeterClient::endSoftware()",
        )

        self.assertIn(
            "VBVMR_AudioCallbackRegister(VBVMR_AUDIOCALLBACK_OUT",
            init,
        )
        self.assertIn("case VBVMR_CBCOMMAND_BUFFER_OUT:", callback)
        self.assertIn("audioBuffer->audiobuffer_w + 8 * i", callback)
        self.assertIn("audioBuffer->audiobuffer_r + 8 * i", callback)
        self.assertIn("engine->process(", callback)
        self.assertIn('sstream << "Output A" << (i + 1)', detection)
        self.assertIn(
            "find(outputs.begin(), outputs.end(), output) != outputs.end()",
            detection,
        )

        buffer_processing = callback[
            callback.index("case VBVMR_CBCOMMAND_BUFFER_OUT:") :
        ]
        for forbidden in (
            "Sleep(",
            "LogF(",
            "WaitForSingleObject(",
            "EnterCriticalSection(",
            "RegistryHelper::",
            "new FilterEngine",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, buffer_processing)

    def test_stream_change_defers_a_stop_then_start_to_the_main_thread(self) -> None:
        callback = section(
            CLIENT,
            "void VoicemeeterClient::handle(long nCommand",
            "void VoicemeeterClient::initSoftware()",
        )
        change_case = callback[
            callback.index("case VBVMR_CBCOMMAND_CHANGE:") :
            callback.index("case VBVMR_CBCOMMAND_BUFFER_OUT:")
        ]
        command = section(
            CLIENT,
            "void VoicemeeterClient::handleCommand(",
            "bool VoicemeeterClient::isBufferSilent(",
        )

        self.assertIn("PostThreadMessage(mainThreadId, WM_COMMAND, IDM_RESTART", change_case)
        self.assertNotIn("VBVMR_AudioCallbackStop", change_case)
        self.assertNotIn("VBVMR_AudioCallbackStart", change_case)
        self.assertIn("case IDM_RESTART:", command)
        self.assertIn("VBVMR_AudioCallbackStop()", command)
        self.assertIn("VBVMR_AudioCallbackStart()", command)
        self.assertLess(
            command.index("VBVMR_AudioCallbackStop()"),
            command.index("VBVMR_AudioCallbackStart()"),
        )

    def test_initial_start_is_checked_and_failure_is_not_silent(self) -> None:
        run = section(
            CLIENT,
            "void VoicemeeterClient::run()",
            "void VoicemeeterClient::handle(long nCommand",
        )
        command = section(
            CLIENT,
            "void VoicemeeterClient::handleCommand(",
            "bool VoicemeeterClient::isBufferSilent(",
        )

        self.assertIn("if (wTimer == 0)", run)
        self.assertIn("handleCommand(IDM_START, 0)", run)
        self.assertIn("getMessageResult = GetMessage", run)
        self.assertIn("if (getMessageResult == -1)", run)
        self.assertIn("long result = vmr.VBVMR_AudioCallbackStart()", command)
        self.assertIn("if (result != 0)", command)
        self.assertIn("throw InitError", command)

    def test_teardown_releases_the_callback_before_logout(self) -> None:
        teardown = section(
            CLIENT,
            "void VoicemeeterClient::endSoftware()",
            "void VoicemeeterClient::handleCommand(",
        )

        self.assertIn("VBVMR_AudioCallbackStop()", teardown)
        self.assertIn("VBVMR_AudioCallbackUnregister()", teardown)
        self.assertIn("VBVMR_Logout()", teardown)
        self.assertLess(
            teardown.index("VBVMR_AudioCallbackStop()"),
            teardown.index("VBVMR_AudioCallbackUnregister()"),
        )
        self.assertLess(
            teardown.index("VBVMR_AudioCallbackUnregister()"),
            teardown.index("VBVMR_Logout()"),
        )

    def test_process_inspection_uses_win32_failure_values_and_closes_handles(self) -> None:
        ensure = section(
            APO_INFO,
            "void VoicemeeterAPOInfo::ensureVoicemeeterClientRunning()",
            "void VoicemeeterAPOInfo::saveVoicemeeterSampleRate(",
        )

        self.assertIn("SCOPE_EXIT{CloseHandle(tokenHandle); };", ensure)
        self.assertIn("if (processHandle == NULL)", ensure)
        self.assertNotIn("processHandle == INVALID_HANDLE_VALUE", ensure)
        self.assertNotIn("PROCESS_TERMINATE", ensure)
        self.assertIn("SCOPE_EXIT{CloseHandle(processHandle); };", ensure)
        self.assertIn("delete[] cmdLineBuf", ensure)
        empty_guard = ensure.index("if (processArgs.empty())")
        self.assertLess(empty_guard, ensure.index("processArgs.front()"))

    def test_output_insert_ownership_and_a1_autostart_remain_explicit(self) -> None:
        init = section(
            CLIENT,
            "void VoicemeeterClient::initSoftware()",
            "void VoicemeeterClient::detectVoicemeeterType()",
        )
        prepend = section(
            APO_INFO,
            "void VoicemeeterAPOInfo::prependInfos(",
            "VoicemeeterAPOInfo::VoicemeeterAPOInfo(",
        )
        install = section(
            APO_INFO,
            "void VoicemeeterAPOInfo::install()",
            "void VoicemeeterAPOInfo::uninstall()",
        )
        ensure = section(
            APO_INFO,
            "void VoicemeeterAPOInfo::ensureVoicemeeterClientRunning()",
            "void VoicemeeterAPOInfo::saveVoicemeeterSampleRate(",
        )

        self.assertIn("if (rep == 1)", init)
        self.assertIn("Output Insert already in use by", init)
        self.assertIn('sstream << "Output A" << (i + 1)', prepend)
        self.assertIn("createLink(startupFilePath, clientPath, argString)", install)
        self.assertIn("if (!matchingProcessExists && !args.empty())", ensure)
        self.assertIn("ShellExecuteW(", ensure)

    def test_startup_link_uses_hibiki_name_and_migrates_legacy_link(self) -> None:
        install = section(
            APO_INFO,
            "void VoicemeeterAPOInfo::install()",
            "void VoicemeeterAPOInfo::uninstall()",
        )
        uninstall = section(
            APO_INFO,
            "void VoicemeeterAPOInfo::uninstall()",
            "void VoicemeeterAPOInfo::reinstall()",
        )

        self.assertIn('L"Hibiki EQAPO Voicemeeter Client.lnk"', APO_INFO)
        self.assertIn('L"Equalizer APO Voicemeeter Client.lnk"', APO_INFO)
        self.assertIn("getStartupLinkArgs", APO_INFO)
        self.assertIn("getStartupPath()", install)
        self.assertIn("DeleteFileW(getLegacyStartupPath().c_str())", install)
        self.assertIn("getStartupPath()", uninstall)
        self.assertIn("DeleteFileW(getLegacyStartupPath().c_str())", uninstall)


if __name__ == "__main__":
    unittest.main()

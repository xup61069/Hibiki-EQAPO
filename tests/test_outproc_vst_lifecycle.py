#!/usr/bin/env python3
"""Source contracts for the out-of-process VST cold-start lifecycle."""

from __future__ import annotations

import ctypes
import os
import pathlib
import re
import struct
import subprocess
import tempfile
import unittest
from ctypes import wintypes


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class OutProcVSTLifecycleTests(unittest.TestCase):
    def test_named_runtime_cold_starts_on_a_monitor_thread(self) -> None:
        runtime = read("filters/OutProcVSTPluginFilter.cpp")
        process_body = runtime[
            runtime.index("void OutProcVSTPluginFilter::process") : runtime.index(
                "void OutProcVSTPluginFilter::closeHost"
            )
        ]

        self.assertIn("writeConfigFile()", runtime)
        self.assertIn("namedHostMonitorProc", runtime)
        self.assertIn("CreateThread(NULL, 0, namedHostMonitorProc", runtime)
        self.assertIn('L" --session \\""', runtime)
        self.assertIn('L" --vst-config \\""', runtime)
        self.assertIn('L" --parent-pid " << GetCurrentProcessId()', runtime)
        self.assertNotIn("CreateProcessW", process_body)
        self.assertNotIn("CreateThread", process_body)
        self.assertIn("WaitForSingleObject(hostReadyEvent, 0)", process_body)

        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")
        named_host = host[
            host.index("static int runNamedVstHost") : host.index(
                "static int runHostMain"
            )
        ]
        initialize = named_host.index("initializeVst(session.header, dspState.vst)")
        publish_ready = named_host.index("SetEvent(session.hostReady)")
        self.assertLess(initialize, publish_ready)

    def test_headless_and_gui_hosts_share_one_lease_and_handoff(self) -> None:
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")

        for token in ("HostLease", "HostHandoff", "HostReady"):
            self.assertIn(token, host)
        self.assertIn("runNamedVstHost(vstConfigPath, sessionId, ownerProcessId)", host)
        self.assertIn("WAIT_ABANDONED_0 + 3", host)
        self.assertIn("OpenProcess(SYNCHRONIZE, FALSE, ownerProcessId)", host)

        runtime = read("filters/OutProcVSTPluginFilter.cpp")
        self.assertIn(
            'CreateEventW(&securityAttributes, TRUE, FALSE, makeObjectName(L"HostHandoff")',
            runtime,
        )

        reset_ready = runtime.index("ResetEvent(context->hostReadyEvent)")
        release_lease = runtime.index(
            "ReleaseMutex(context->hostLeaseMutex)", reset_ready
        )
        self.assertLess(reset_ready, release_lease)

        handoff = editor.index('signalOutProcPanel(L"HostHandoff")')
        launch = editor.index("QProcess::startDetached", handoff)
        self.assertLess(handoff, launch)

    def test_runtime_owner_is_a_cross_thread_lifetime_token(self) -> None:
        runtime = read("filters/OutProcVSTPluginFilter.cpp")
        header = read("filters/OutProcVSTPluginFilter.h")

        self.assertIn('makeObjectName(L"RuntimeOwner")', runtime)
        self.assertIn("ownerError == ERROR_ALREADY_EXISTS", runtime)
        self.assertIn("CloseHandle(runtimeOwnerToken)", runtime)
        self.assertNotIn("ReleaseMutex(runtimeOwner", runtime)
        self.assertNotIn("runtimeOwnerAcquired", header)

    def test_security_attributes_are_caller_local(self) -> None:
        runtime = read("filters/OutProcVSTPluginFilter.cpp")
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")

        self.assertNotIn("static SECURITY_ATTRIBUTES attributes", runtime)
        self.assertNotIn("static SECURITY_ATTRIBUTES attributes", host)
        self.assertIn("SECURITY_ATTRIBUTES securityAttributes = {};", runtime)
        self.assertIn("SECURITY_ATTRIBUTES securityAttributes = {};", host)

    def test_gui_audio_host_reconnects_across_runtime_reload(self) -> None:
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")
        audio_thread = host[
            host.index("static DWORD WINAPI guiAudioThreadProc") : host.index(
                "static void resizeGuiWindowToEditor"
            )
        ]

        shutdown_case = audio_thread[audio_thread.index("case WAIT_OBJECT_0 + 1:") :]
        self.assertIn("closeGuiAudioSession();", shutdown_case)
        self.assertIn("WaitForSingleObject(context->stopEvent, 50)", shutdown_case)
        self.assertIn("SetEvent(context->hostReadyEvent)", audio_thread)
        self.assertIn("ResetEvent(context->hostReadyEvent)", shutdown_case)

    def test_parameter_parsers_are_bounds_checked_and_numeric_strict(self) -> None:
        parser = read("helpers/VSTParameterParser.h")
        factories = (
            read("filters/VSTPluginFilterFactory.cpp"),
            read("filters/OutProcVSTPluginFilterFactory.cpp"),
            read("Editor/guis/VSTPluginFilterGUIFactory.cpp"),
        )

        self.assertIn("index + 2 < parts.size()", parser)
        self.assertIn("index += 3", parser)
        self.assertIn("VSTParseFiniteFloat", parser)
        self.assertIn("<index> <name> <value>", parser)
        for factory in factories:
            self.assertIn("VSTConsumeParameter(parts, i, paramMap)", factory)
            self.assertNotIn("x <= parts.size()", factory)
            self.assertNotIn("isdigit(value", factory)
            self.assertNotIn("wcstof(", factory)
            self.assertIn("++i;", factory)

    def test_gui_exit_gets_a_grace_period_before_verified_force_kill(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")
        protocol = read("outproc/OutProcAudioProtocol.h")

        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("void VSTPluginFilterGUI::applyDialog")
        ]
        stop = editor[
            editor.index("static bool stopOutProcProcess") : editor.index(
                "static OutProcProcessIdentity resolveOutProcProcessIdentity"
            )
        ]
        self.assertLess(
            terminate.index('signalOutProcPanel(L"GuiExit")'),
            terminate.index("stopOutProcProcess"),
        )
        self.assertLess(
            stop.index("WaitForSingleObject(process, outProcGuiExitGraceMs)"),
            stop.index("TerminateProcess(process, 23)"),
        )
        self.assertIn("processCreationTime", protocol)
        self.assertIn("info->processCreationTime = processCreationTime", host)
        self.assertIn(
            'stream << GetCurrentProcessId() << L" " << processCreationTime', host
        )
        self.assertIn("tokens.size() != 2", editor)
        self.assertIn("file.size() > 128", editor)
        self.assertIn(
            "queryOutProcProcessCreationTime(process) != identity.processCreationTime",
            stop,
        )
        self.assertIn("isExpectedOutProcHostProcess(process)", stop)

    def test_unconfirmed_gui_exit_preserves_state_and_blocks_mutations(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]

        failure_guard = terminate.index("if (!processStopped)")
        sidecar_read = terminate.index("OutProcVSTConfig updatedConfig")
        self.assertLess(failure_guard, sidecar_read)
        self.assertIn("return false;", terminate[failure_guard:sidecar_read])
        self.assertIn("!exitEventExists && !exitEventLookupFailed", terminate)
        success_cleanup = terminate.index(
            "removeExistingFileChecked(outProcGuiConfigPath)"
        )
        self.assertGreater(success_cleanup, failure_guard)
        for preserved in (
            "removeExistingFileChecked(outProcGuiConfigPath)",
            "outProcGuiConfigPath.clear()",
        ):
            with self.subTest(preserved=preserved):
                self.assertIn(preserved, terminate[success_cleanup:])

        mark_stopped = editor[
            editor.index(
                "bool VSTPluginFilterGUI::markOutProcPanelStoppedPreservingState"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]
        pid_remove = mark_stopped.index("removeExistingFileChecked(pidPath)")
        pid_failure = mark_stopped.index("if (!pidRemoved)")
        clear_tracking = mark_stopped.index("outProcGuiRunning = false;")
        self.assertLess(pid_remove, pid_failure)
        self.assertLess(pid_failure, clear_tracking)
        self.assertIn("return false;", mark_stopped[pid_failure:clear_tracking])

        ensure = editor[
            editor.index(
                "bool VSTPluginFilterGUI::ensureOutProcPanelStopped"
            ) : editor.index("void VSTPluginFilterGUI::applyDialog")
        ]
        self.assertIn("bool failureReported = false;", ensure)
        self.assertIn("&stateChanged, &failureMessage, &failureReported", ensure)
        self.assertIn("if (!failureReported)", ensure)
        self.assertIn("This row was left unchanged", terminate)
        self.assertIn("ui->statusLabel->setText(failureMessage);", ensure)
        self.assertIn("QMessageBox::warning", ensure)

        handlers = (
            (
                editor[
                    editor.index(
                        "void VSTPluginFilterGUI::on_reloadButton_clicked"
                    ) : editor.index("void VSTPluginFilterGUI::on_midiButton_clicked")
                ],
                "hostId = QUuid::createUuid()",
            ),
            (
                editor[
                    editor.index(
                        "void VSTPluginFilterGUI::on_midiButton_clicked"
                    ) : editor.index(
                        "void VSTPluginFilterGUI::on_vst3ClassComboBox_currentIndexChanged"
                    )
                ],
                "beginTemporaryFilterConfiguration",
            ),
            (
                editor[
                    editor.index(
                        "void VSTPluginFilterGUI::on_vst3ClassComboBox_currentIndexChanged"
                    ) : editor.index("void VSTPluginFilterGUI::openOutProcPanel")
                ],
                "vst3ClassIndex = index",
            ),
            (
                editor[
                    editor.index(
                        "void VSTPluginFilterGUI::on_pathLineEdit_editingFinished"
                    ) : editor.index(
                        "bool VSTPluginFilterGUI::capturePluginStateIfChanged"
                    )
                ],
                "library = VSTPluginLibrary::getInstance",
            ),
        )
        for handler, mutation in handlers:
            with self.subTest(mutation=mutation):
                gate = handler.index("ensureOutProcPanelStopped")
                self.assertLess(gate, handler.index(mutation))
                self.assertIn("return;", handler[gate : handler.index(mutation)])

        midi_handler = handlers[1][0]
        self.assertIn("if (outProcMode && !midiConfig.empty())", midi_handler)
        self.assertNotIn("outProcGuiRunning && !midiConfig.empty()", midi_handler)

    def test_stopped_host_with_unreadable_state_sidecar_vetoes_data_loss(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]
        recovery_guard = terminate.index("if (!OutProcReadVSTConfig")
        sidecar_delete = terminate.index(
            "removeExistingFileChecked(outProcGuiConfigPath)"
        )
        self.assertLess(recovery_guard, sidecar_delete)
        failed_recovery = terminate[recovery_guard:sidecar_delete]
        self.assertIn("return false;", failed_recovery)
        self.assertIn("state recovery failed; preserving sidecar", failed_recovery)
        self.assertIn("temporary state file were left unchanged", failed_recovery)

    def test_readable_sidecar_requires_successful_final_write_ack(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")
        host_run = host[
            host.index("static int runVstGuiHost") : host.index(
                "static bool validateHeader"
            )
        ]
        self.assertIn("if (!OutProcWriteVSTConfig(vstConfigPath, config))", host_run)
        self.assertIn("exitCode = 19;", host_run)

        stop = editor[
            editor.index("static bool stopOutProcProcess") : editor.index(
                "static OutProcProcessIdentity resolveOutProcProcessIdentity"
            )
        ]
        self.assertIn("bool& finalStateCommitted", stop)
        self.assertIn("GetExitCodeProcess(process, &processExitCode)", stop)
        self.assertIn("stopped && !forcedTermination", stop)
        self.assertIn("processExitCode == ERROR_SUCCESS", stop)

        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]
        ack_guard = terminate.index(
            "if (!outProcGuiConfigPath.isEmpty() && !finalStateCommitted)"
        )
        sidecar_read = terminate.index("OutProcVSTConfig updatedConfig")
        self.assertLess(ack_guard, sidecar_read)
        self.assertIn(
            "return resolveUnconfirmedState(",
            terminate[ack_guard:sidecar_read],
        )
        self.assertIn(
            "markOutProcPanelStoppedPreservingState(failureMessage)",
            terminate[ack_guard:sidecar_read],
        )

        recovery = terminate[
            terminate.index("auto resolveUnconfirmedState") : terminate.index(
                "if (identityLookupFailed"
            )
        ]
        self.assertIn("OutProcReadVSTConfig", recovery)
        self.assertIn('tr("Use last readable snapshot")', recovery)
        self.assertIn("QMessageBox::Discard", recovery)
        self.assertIn("QMessageBox::Cancel", recovery)
        self.assertIn("recoveryBox.setDefaultButton(cancelButton)", recovery)
        self.assertLess(
            recovery.index("removeExistingFileChecked(preservedPath)"),
            recovery.index("chunkData = snapshot.chunkData"),
        )
        self.assertIn("outProcGuiConfigPath.clear();", recovery)
        self.assertIn("*stateChanged = recoveredStateChanged;", recovery)

        open_panel = editor[
            editor.index("void VSTPluginFilterGUI::openOutProcPanel") : editor.index(
                "bool VSTPluginFilterGUI::signalOutProcPanel"
            )
        ]
        self.assertIn("recoveringPreservedState", open_panel)
        self.assertIn("launchPreservedState", open_panel)
        self.assertIn("OutProcReadVSTConfig(", open_panel)
        self.assertIn("Discard it and reopen the panel", open_panel)

    def test_gui_audio_worker_is_joined_before_shared_state_teardown(self) -> None:
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")
        host_run = host[
            host.index("static int runVstGuiHost") : host.index(
                "static bool validateHeader"
            )
        ]
        signal_stop = host_run.index("SetEvent(audioContext.stopEvent)")
        join = host_run.index(
            "const DWORD audioThreadWait = WaitForSingleObject(audioThread, 1000)"
        )
        join_guard = host_run.index("if (audioThreadWait != WAIT_OBJECT_0)")
        fail_fast = host_run.index("TerminateProcess(", join_guard)
        close_thread = host_run.index("CloseHandle(audioThread)", join_guard)
        close_stop = host_run.index("CloseHandle(audioContext.stopEvent)", close_thread)
        effect_read = host_run.index("readFromEffect(config.chunkData", close_stop)
        teardown = host_run.index("dspState.vst.effects.clear()", effect_read)

        self.assertLess(signal_stop, join)
        self.assertLess(join, join_guard)
        self.assertLess(join_guard, fail_fast)
        self.assertLess(fail_fast, close_thread)
        self.assertLess(close_thread, close_stop)
        self.assertLess(close_stop, effect_read)
        self.assertLess(effect_read, teardown)
        self.assertIn("guiAudioThreadJoinFailureExitCode = 24", host)
        self.assertIn("Sleep(INFINITE);", host_run[join_guard:close_thread])

    def test_runtime_forced_teardown_never_reports_a_clean_exit(self) -> None:
        protocol = read("outproc/OutProcAudioProtocol.h")
        sources = (
            read("filters/OutProcVSTPluginFilter.cpp"),
            read("filters/OutProcGainFilter.cpp"),
            read("filters/OutProcBiquadFilter.cpp"),
        )

        self.assertIn("OUTPROC_RUNTIME_FORCED_TERMINATION_EXIT_CODE = 25", protocol)
        self.assertEqual(
            sum(
                source.count("OUTPROC_RUNTIME_FORCED_TERMINATION_EXIT_CODE")
                for source in sources
            ),
            4,
        )
        for source in sources:
            self.assertIsNone(
                re.search(r"TerminateProcess\s*\([^,]+,\s*0\s*\)", source)
            )

    def test_editor_uses_the_same_ascii_session_id_normalization_as_runtime(
        self,
    ) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        runtime = read("filters/OutProcVSTPluginFilter.cpp")
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")

        sanitizer = editor[
            editor.index("static QString makeSafeOutProcSessionId") : editor.index(
                "static QString makeOutProcObjectName"
            )
        ]
        for ascii_range in (
            "code >= '0' && code <= '9'",
            "code >= 'a' && code <= 'z'",
            "code >= 'A' && code <= 'Z'",
            "code == '-'",
            "code == '_'",
        ):
            self.assertIn(ascii_range, sanitizer)
        self.assertNotIn("isLetterOrNumber", editor)
        self.assertIn("makeSafeOutProcSessionId(hostId)", editor)
        self.assertIn("ch >= L'0' && ch <= L'9'", runtime)
        self.assertIn("ch >= L'0' && ch <= L'9'", host)

        constructor = editor[
            editor.index("VSTPluginFilterGUI::VSTPluginFilterGUI") : editor.index(
                "VSTPluginFilterGUI::~VSTPluginFilterGUI"
            )
        ]
        self.assertIn("!isCanonicalOutProcSessionId(this->hostId)", constructor)
        self.assertIn("QUuid::createUuid()", constructor)
        self.assertIn("QTimer::singleShot(0, this", constructor)
        self.assertIn("emit updateModel();", constructor)

    def test_unverifiable_existing_pid_file_fails_identity_lookup_closed(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        pid_reader = editor[
            editor.index(
                "static OutProcProcessIdentity readOutProcProcessIdentityPidFile"
            ) : editor.index("static QString canonicalExecutablePath")
        ]
        resolver = editor[
            editor.index(
                "static OutProcProcessIdentity resolveOutProcProcessIdentity"
            ) : editor.index("void VSTPluginFilterGUI::closeOutProcPanel")
        ]
        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]

        self.assertIn("bool& lookupFailed", pid_reader)
        self.assertIn("GetFileAttributesW", pid_reader)
        self.assertIn("attributeError != ERROR_FILE_NOT_FOUND", pid_reader)
        self.assertIn("attributeError != ERROR_PATH_NOT_FOUND", pid_reader)
        self.assertGreaterEqual(pid_reader.count("lookupFailed = true;"), 4)
        self.assertGreaterEqual(resolver.count("if (pidLookupFailed)"), 2)
        self.assertIn("lookupFailed = pidLookupFailed;", resolver)
        live_pid_reader = editor[
            editor.index(
                "static OutProcProcessIdentity readLiveOutProcProcessIdentityPidFile"
            ) : editor.index("static bool stopOutProcProcess")
        ]
        identity_probe = editor[
            editor.index(
                "static OutProcProcessIdentityStatus probeOutProcProcessIdentity"
            ) : editor.index(
                "static OutProcProcessIdentity readLiveOutProcProcessIdentityPidFile"
            )
        ]
        self.assertIn("WaitForSingleObject(process, 0)", identity_probe)
        self.assertIn(
            "actualCreationTime != identity.processCreationTime", identity_probe
        )
        self.assertIn("isExpectedOutProcHostProcess(", identity_probe)
        self.assertIn("probeOutProcProcessIdentity(", live_pid_reader)
        self.assertIn("stale pid file ignored", live_pid_reader)
        self.assertIn(
            "reused or exited pid file retained for commit cleanup", live_pid_reader
        )
        self.assertGreaterEqual(
            resolver.count("readLiveOutProcProcessIdentityPidFile"), 3
        )
        self.assertLess(
            terminate.index("if (identityLookupFailed || exitEventLookupFailed"),
            terminate.index('signalOutProcPanel(L"GuiExit")'),
        )

    def test_launched_identity_cannot_be_redirected_by_named_mapping(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        resolver = editor[
            editor.index(
                "static OutProcProcessIdentity resolveOutProcProcessIdentity"
            ) : editor.index("void VSTPluginFilterGUI::closeOutProcPanel")
        ]
        launched_branch = resolver.index("if (launched.processId != 0)")
        mapped_fallback = resolver.index(
            "\n\tif (mapped.processId != 0)", launched_branch
        )
        self.assertLess(launched_branch, mapped_fallback)
        launched = resolver[launched_branch:mapped_fallback]
        self.assertLess(
            launched.index("probeOutProcProcessIdentity(launched"),
            launched.index("if (identitiesConflict(launched, mapped))"),
        )
        self.assertIn("OutProcProcessIdentityStatus::Unverifiable", launched)
        self.assertIn("OutProcProcessIdentityStatus::Stale", launched)
        self.assertIn("launchedIdentityStale = true;", launched)
        self.assertIn("if (identitiesConflict(launched, mapped))", launched)
        self.assertIn("return failIdentityConflict();", launched)
        self.assertLess(
            launched.index("if (launched.processCreationTime != 0)"),
            launched.index("readLiveOutProcProcessIdentityPidFile"),
        )

        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]
        self.assertIn(
            "bool processStopped = launchedIdentityStale && !exitEventExists;",
            terminate,
        )
        self.assertLess(
            terminate.index("bool processStopped = launchedIdentityStale"),
            terminate.index(
                "if (!outProcGuiConfigPath.isEmpty() && !finalStateCommitted)"
            ),
        )

    def test_reconnected_host_without_state_sidecar_is_not_deletable(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]
        unverifiable = terminate.index("outProcGuiConfigPath.isEmpty()")
        exit_signal = terminate.index('signalOutProcPanel(L"GuiExit"')
        self.assertLess(unverifiable, exit_signal)
        guard = terminate[unverifiable:exit_signal]
        self.assertIn("outProcGuiPid <= 0", guard)
        self.assertIn("processIdentity.processId != 0 || exitEventExists", guard)
        self.assertIn("return false;", guard)

    def test_closed_reconnected_host_clears_stale_running_state(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        probe = editor[
            editor.index("static bool outProcPanelEventExists") : editor.index(
                "bool VSTPluginFilterGUI::consumeOutProcPanelSignal"
            )
        ]
        self.assertIn("OpenEventW(", probe)
        self.assertIn("SYNCHRONIZE", probe)
        self.assertNotIn("SetEvent", probe)

        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]
        stale_clear = terminate.index("processIdentity.processId == 0 &&")
        self.assertLess(stale_clear, terminate.index("if (!processStopped)"))
        self.assertIn("markOutProcPanelStoppedPreservingState", terminate[stale_clear:])

    def test_cleanup_failures_retain_retry_state_and_host_logs_truthfully(self) -> None:
        editor = read("Editor/guis/VSTPluginFilterGUI.cpp")
        header = read("Editor/guis/VSTPluginFilterGUI.h")
        host = read("EqApoOutProcHost/EqApoOutProcHost.cpp")

        terminate = editor[
            editor.index(
                "bool VSTPluginFilterGUI::terminateOutProcPanel"
            ) : editor.index("bool VSTPluginFilterGUI::ensureOutProcPanelStopped")
        ]
        sidecar_remove = terminate.index(
            "!removeExistingFileChecked(outProcGuiConfigPath)"
        )
        sidecar_failure = terminate.index("{", sidecar_remove)
        sidecar_clear = terminate.index("outProcGuiConfigPath.clear()", sidecar_failure)
        self.assertLess(sidecar_remove, sidecar_failure)
        self.assertLess(sidecar_failure, sidecar_clear)
        self.assertIn("return false;", terminate[sidecar_failure:sidecar_clear])

        checked_remove = editor[
            editor.index("static bool removeExistingFileChecked") : editor.index(
                "VSTPluginFilterGUI::~VSTPluginFilterGUI"
            )
        ]
        self.assertIn("if (QFile::remove(path))", checked_remove)
        self.assertIn("GetFileAttributesW", checked_remove)
        self.assertIn("error == ERROR_FILE_NOT_FOUND", checked_remove)
        self.assertIn("error == ERROR_PATH_NOT_FOUND", checked_remove)
        self.assertNotIn("QFile::exists(path)", checked_remove)
        self.assertIn("outProcFinalStateCommitted", header)
        self.assertIn('state.insert("outProcFinalStateCommitted"', editor)
        self.assertIn('state.value(\n\t\t"outProcFinalStateCommitted")', editor)

        delete_pid = host[
            host.index("static void deleteSessionPidFile") : host.index(
                "static void closeGuiAudioSession"
            )
        ]
        self.assertIn("if (DeleteFileW(path.c_str()))", delete_pid)
        self.assertIn("failed to delete pid file", delete_pid)
        self.assertNotIn(
            'DeleteFileW(path.c_str());\n\t\tappendDebugLog(L"deleted pid file',
            delete_pid,
        )

    def test_load_failures_reach_vst_diagnostics(self) -> None:
        in_process = read("filters/VSTPluginFilterFactory.cpp")
        out_process = read("EqApoOutProcHost/EqApoOutProcHost.cpp")

        self.assertIn("VSTDiag(", in_process)
        self.assertIn("out-of-process VST load failed", out_process)
        self.assertIn("out-of-process GUI VST load failed", out_process)

    @unittest.skipUnless(os.name == "nt", "Win32 named IPC smoke test")
    def test_built_host_cold_starts_and_hands_off(self) -> None:
        host_path = ROOT / "EqApoOutProcHost/x64/Release/EqApoOutProcHost.exe"
        if not host_path.is_file():
            self.skipTest(
                "build EqApoOutProcHost Release x64 to run the IPC smoke test"
            )

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileMappingW.argtypes = (
            wintypes.HANDLE,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPCWSTR,
        )
        kernel32.CreateFileMappingW.restype = wintypes.HANDLE
        kernel32.CreateEventW.argtypes = (
            wintypes.LPVOID,
            wintypes.BOOL,
            wintypes.BOOL,
            wintypes.LPCWSTR,
        )
        kernel32.CreateEventW.restype = wintypes.HANDLE
        kernel32.CreateMutexW.argtypes = (
            wintypes.LPVOID,
            wintypes.BOOL,
            wintypes.LPCWSTR,
        )
        kernel32.CreateMutexW.restype = wintypes.HANDLE
        kernel32.MapViewOfFile.argtypes = (
            wintypes.HANDLE,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_size_t,
        )
        kernel32.MapViewOfFile.restype = wintypes.LPVOID
        kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        kernel32.SetEvent.argtypes = (wintypes.HANDLE,)
        kernel32.SetEvent.restype = wintypes.BOOL
        kernel32.ReleaseMutex.argtypes = (wintypes.HANDLE,)
        kernel32.ReleaseMutex.restype = wintypes.BOOL
        kernel32.UnmapViewOfFile.argtypes = (wintypes.LPCVOID,)
        kernel32.UnmapViewOfFile.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL

        class AudioHeader(ctypes.Structure):
            _fields_ = (
                ("magic", ctypes.c_uint32),
                ("version", ctypes.c_uint32),
                ("sample_rate", ctypes.c_uint32),
                ("channel_count", ctypes.c_uint32),
                ("max_frames", ctypes.c_uint32),
                ("frame_count", ctypes.c_uint32),
                ("smoothing_samples", ctypes.c_uint32),
                ("dsp_type", ctypes.c_uint32),
                ("gain_db", ctypes.c_double),
                ("biquad_a0", ctypes.c_double),
                ("biquad_a1", ctypes.c_double),
                ("biquad_a2", ctypes.c_double),
                ("biquad_b1", ctypes.c_double),
                ("biquad_b2", ctypes.c_double),
                ("request_seq", ctypes.c_uint64),
                ("response_seq", ctypes.c_uint64),
                ("status", ctypes.c_uint32),
                ("host_state", ctypes.c_uint32),
            )

        session_id = f"test-{os.getpid()}"
        prefix = f"Global\\EqApoOutProcVST_{session_id}_"
        channel_count = 2
        max_frames = 16
        mapping_size = ctypes.sizeof(AudioHeader) + channel_count * max_frames * 16
        handles: list[int] = []
        view = None
        child: subprocess.Popen[bytes] | None = None

        def checked(handle: int, label: str) -> int:
            if not handle:
                self.skipTest(
                    f"could not create {label}, Win32 error {ctypes.get_last_error()}"
                )
            handles.append(handle)
            return handle

        try:
            mapping = checked(
                kernel32.CreateFileMappingW(
                    wintypes.HANDLE(-1), None, 0x04, 0, mapping_size, prefix + "Map"
                ),
                "global mapping",
            )
            request = checked(
                kernel32.CreateEventW(None, False, False, prefix + "Request"),
                "request event",
            )
            response = checked(
                kernel32.CreateEventW(None, False, False, prefix + "Response"),
                "response event",
            )
            shutdown = checked(
                kernel32.CreateEventW(None, True, False, prefix + "Shutdown"),
                "shutdown event",
            )
            handoff = checked(
                kernel32.CreateEventW(None, True, False, prefix + "HostHandoff"),
                "handoff event",
            )
            lease = checked(
                kernel32.CreateMutexW(None, False, prefix + "HostLease"), "host lease"
            )
            ready = checked(
                kernel32.CreateEventW(None, True, False, prefix + "HostReady"),
                "ready event",
            )

            view = kernel32.MapViewOfFile(mapping, 0xF001F, 0, 0, mapping_size)
            self.assertTrue(view, f"MapViewOfFile failed: {ctypes.get_last_error()}")
            ctypes.memset(view, 0, mapping_size)
            header = AudioHeader.from_address(view)
            header.magic = 0x4F504147
            header.version = 2
            header.sample_rate = 48000
            header.channel_count = channel_count
            header.max_frames = max_frames
            header.frame_count = max_frames
            header.dsp_type = 3

            with tempfile.TemporaryDirectory(prefix="eqapo-vst-host-") as temp_dir:
                config_path = pathlib.Path(temp_dir) / "smoke.opvs"
                library = "Z:\\EqApo-smoke-does-not-exist.dll".encode("utf-16-le")
                payload = (
                    struct.pack("<II", 0x4F505653, 2)
                    + struct.pack("<I", len(library) // 2)
                    + library
                    + struct.pack("<i", 0)
                    + struct.pack("<I", 0)
                    + struct.pack("<I", 0)
                )
                config_path.write_bytes(payload)

                child = subprocess.Popen(
                    [
                        str(host_path),
                        "--session",
                        session_id,
                        "--vst-config",
                        str(config_path),
                    ],
                    cwd=host_path.parent,
                    creationflags=0x08000000,
                )

                self.assertEqual(kernel32.WaitForSingleObject(ready, 5000), 0)
                self.assertEqual(kernel32.WaitForSingleObject(lease, 0), 258)

                header.request_seq = 1
                self.assertTrue(kernel32.SetEvent(request))
                self.assertEqual(kernel32.WaitForSingleObject(response, 5000), 0)
                self.assertEqual(header.status, 1)

                self.assertTrue(kernel32.SetEvent(handoff))
                self.assertEqual(child.wait(timeout=5), 20)
                child = None

                lease_result = kernel32.WaitForSingleObject(lease, 1000)
                self.assertIn(lease_result, (0, 128))
                self.assertTrue(kernel32.ReleaseMutex(lease))
        finally:
            if child is not None and child.poll() is None:
                kernel32.SetEvent(shutdown)
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
            if view:
                kernel32.UnmapViewOfFile(view)
            for handle in reversed(handles):
                kernel32.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()

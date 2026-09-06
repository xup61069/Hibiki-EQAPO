Unicode true
SilentInstall silent
RequestExecutionLevel user

!ifndef HARNESS_OUTPUT
  !error "HARNESS_OUTPUT must point to the harness executable"
!endif

!ifndef HARNESS_FIXTURE_A
  !error "HARNESS_FIXTURE_A must point to an AMD64 PE DLL fixture"
!endif

!ifndef HARNESS_FIXTURE_B
  !error "HARNESS_FIXTURE_B must point to an AMD64 PE DLL fixture"
!endif

!ifndef HARNESS_RUN_ID
  !error "HARNESS_RUN_ID must be unique for this harness build"
!endif

OutFile "${HARNESS_OUTPUT}"

!include "LogicLib.nsh"
!include "FileFunc.nsh"
!addincludedir "..\Setup"

!define ASIO_DISCOVERY_HARNESS
!define ASIO_ENUM_ROOT "Software\ASIO"
!define ASIO_PROXY_CLSID "{D47C55C9-3F7D-422F-86E9-32E170815D53}"
!define ASIO_PROXY_TARGET_REGPATH "Software\EqualizerAPO\ASIOProxy"
!define HARNESS_FIXTURE_A_CLSID "{11111111-1111-4111-8111-111111111111}"
!define HARNESS_FIXTURE_B_CLSID "{22222222-2222-4222-8222-222222222222}"
!define HARNESS_REGISTRY_ROOT "Software\HibikiEQAPO\Tests\AsioDiscovery\${HARNESS_RUN_ID}"
!define WIN32_HKEY_CURRENT_USER 0x80000001
!define WIN32_HKEY_LOCAL_MACHINE 0x80000002
!define MAXIMUM_ALLOWED 0x02000000
!define IMAGE_FILE_MACHINE_AMD64 34404
!define IMAGE_FILE_DLL 8192

Var AsioProxyRequested
Var AsioSelectedTargetClsid
Var AsioSelectedTargetName
Var AsioCandidateCount
Var AsioSingleCandidateClsid
Var AsioCommandLineTargetClsid
Var AsioNoProxyOption
Var AsioTargetValid
Var AsioEntryKey
Var AsioEntryClsid
Var AsioEntryName
Var AsioEntryValid
Var AsioBinaryPath
Var AsioBinaryValid
Var AsioBinaryFailure
Var InstallFailureReason
Var InstallRecoveryFailed
Var HarnessDiscoverCount
Var HarnessDiscoverSingle
Var HarnessExitCode
Var HarnessOverrideActive
Var HarnessRegistryHandle
Var HarnessResultPath

!include "..\Setup\AsioProxy.nsh"

Section
  SetRegView 64
  StrCpy $HarnessExitCode "0"
  StrCpy $HarnessOverrideActive "0"
  StrCpy $HarnessRegistryHandle "0"

  ${GetParameters} $0
  ClearErrors
  ${GetOptions} "$0" "/RESULT=" $HarnessResultPath
  ${If} ${Errors}
  ${OrIf} $HarnessResultPath == ""
    StrCpy $HarnessExitCode "10"
    Goto harnessCleanup
  ${EndIf}

  ; The fixtures are data only. The production parser reads their PE headers
  ; without loading or executing either file.
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File /oname=fixture-alpha.dll "${HARNESS_FIXTURE_A}"
  File /oname=fixture-beta.dll "${HARNESS_FIXTURE_B}"

  ; Build a private HKCU subtree first, then map HKLM to its open handle only
  ; inside this process. A crash drops the override automatically; the unique
  ; HKCU key is also deleted on the normal path and by the Python test's guard.
  DeleteRegKey HKCU "${HARNESS_REGISTRY_ROOT}"
  ClearErrors
  WriteRegStr HKCU "${HARNESS_REGISTRY_ROOT}\Software\ASIO\Fixture Alpha" \
    "CLSID" "${HARNESS_FIXTURE_A_CLSID}"
  WriteRegStr HKCU "${HARNESS_REGISTRY_ROOT}\Software\ASIO\Fixture Alpha" \
    "Description" "Fixture ASIO Alpha"
  WriteRegStr HKCU "${HARNESS_REGISTRY_ROOT}\Software\Classes\CLSID\${HARNESS_FIXTURE_A_CLSID}\InprocServer32" \
    "" "$PLUGINSDIR\fixture-alpha.dll"
  WriteRegStr HKCU "${HARNESS_REGISTRY_ROOT}\Software\ASIO\Fixture Beta" \
    "CLSID" "${HARNESS_FIXTURE_B_CLSID}"
  WriteRegStr HKCU "${HARNESS_REGISTRY_ROOT}\Software\ASIO\Fixture Beta" \
    "Description" "Fixture ASIO Beta"
  WriteRegStr HKCU "${HARNESS_REGISTRY_ROOT}\Software\Classes\CLSID\${HARNESS_FIXTURE_B_CLSID}\InprocServer32" \
    "" "$PLUGINSDIR\fixture-beta.dll"
  ${If} ${Errors}
    StrCpy $HarnessExitCode "11"
    Goto harnessCleanup
  ${EndIf}

  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_CURRENT_USER}, w "${HARNESS_REGISTRY_ROOT}", i 0, i ${MAXIMUM_ALLOWED}, *p .r0) i .r1'
  ${If} $1 != 0
  ${OrIf} $0 == 0
    StrCpy $HarnessExitCode "12"
    Goto harnessCleanup
  ${EndIf}
  StrCpy $HarnessRegistryHandle "$0"
  System::Call 'advapi32::RegOverridePredefKey(p ${WIN32_HKEY_LOCAL_MACHINE}, p r0) i .r1'
  ${If} $1 != 0
    StrCpy $HarnessExitCode "13"
    Goto harnessCleanup
  ${EndIf}
  StrCpy $HarnessOverrideActive "1"

  ; Exercise the exact production discovery entry point independently, then
  ; exercise ResolveInitialAsioSelection (which performs discovery again).
  Call DiscoverEligibleAsioTargets
  StrCpy $HarnessDiscoverCount "$AsioCandidateCount"
  StrCpy $HarnessDiscoverSingle "$AsioSingleCandidateClsid"
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $InstallFailureReason ""
  StrCpy $AsioTargetValid ""
  Call ResolveInitialAsioSelection

  ClearErrors
  FileOpen $0 "$HarnessResultPath" w
  ${If} ${Errors}
    StrCpy $HarnessExitCode "14"
    Goto harnessCleanup
  ${EndIf}
  FileWrite $0 "discover_count=$HarnessDiscoverCount$\r$\n"
  FileWrite $0 "discover_single=$HarnessDiscoverSingle$\r$\n"
  FileWrite $0 "resolve_count=$AsioCandidateCount$\r$\n"
  FileWrite $0 "resolve_single=$AsioSingleCandidateClsid$\r$\n"
  FileWrite $0 "requested=$AsioProxyRequested$\r$\n"
  FileWrite $0 "selected=$AsioSelectedTargetClsid$\r$\n"
  FileWrite $0 "selected_name=$AsioSelectedTargetName$\r$\n"
  FileWrite $0 "target_valid=$AsioTargetValid$\r$\n"
  FileWrite $0 "recovery_failed=$InstallRecoveryFailed$\r$\n"
  FileWrite $0 "failure_reason=$InstallFailureReason$\r$\n"
  FileClose $0
  ${If} ${Errors}
    StrCpy $HarnessExitCode "15"
  ${EndIf}

  harnessCleanup:
  ${If} $HarnessOverrideActive == "1"
    System::Call 'advapi32::RegOverridePredefKey(p ${WIN32_HKEY_LOCAL_MACHINE}, p 0) i .r0'
    ${If} $0 != 0
    ${AndIf} $HarnessExitCode == "0"
      StrCpy $HarnessExitCode "16"
    ${EndIf}
  ${EndIf}
  ${If} $HarnessRegistryHandle != 0
    StrCpy $0 "$HarnessRegistryHandle"
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
    ${AndIf} $HarnessExitCode == "0"
      StrCpy $HarnessExitCode "17"
    ${EndIf}
  ${EndIf}
  ClearErrors
  DeleteRegKey HKCU "${HARNESS_REGISTRY_ROOT}"
  ${If} ${Errors}
  ${AndIf} $HarnessExitCode == "0"
    StrCpy $HarnessExitCode "18"
  ${EndIf}
  SetErrorLevel $HarnessExitCode
SectionEnd

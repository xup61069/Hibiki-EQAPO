!include "LogicLib.nsh"
!include "MUI2.nsh"
!include "StrFunc.nsh"
!include "FileFunc.nsh"
!include "WinVer.nsh"
!include "x64.nsh"
!include "nsDialogs.nsh"
!include "WinMessages.nsh"
!include "InstallerRecoveryManifest.nsh"
!include "InstallRootAclCheck.nsh"
!include "EmbeddedPowerShellHelpers.nsh"

${StrTrimNewLines}
${StrTok}

Unicode true
ManifestDPIAware true
CRCCheck force
;Use more efficient compression
SetCompressor /SOLID lzma

!searchparse /file ..\version.h `#define MAJOR ` MAJOR
!searchparse /file ..\version.h `#define MINOR ` MINOR
!searchparse /file ..\version.h `#define REVISION ` REVISION
!define VERSION ${MAJOR}.${MINOR}.${REVISION}
!define PRODUCT_LABEL "Hibiki EQAPO"
!define PRODUCT_FULL_LABEL "${PRODUCT_LABEL} (unofficial Equalizer APO fork)"

!ifndef ENABLE_ASIO_PROXY
!define ENABLE_ASIO_PROXY 0
!endif
!define ASIO_PROXY_LABEL "Hibiki EQAPO"
!define ASIO_PROXY_CLSID "{D47C55C9-3F7D-422F-86E9-32E170815D53}"
!define ASIO_PROXY_DLL "HibikiEQAPODriver.dll"
!define ASIO_ENUM_ROOT "Software\ASIO"
!define ASIO_PROXY_ENUM_REGPATH "${ASIO_ENUM_ROOT}\${ASIO_PROXY_LABEL}"
!define ASIO_PROXY_COM_REGPATH "Software\Classes\CLSID\${ASIO_PROXY_CLSID}"
!define ASIO_PROXY_TARGET_REGPATH "Software\EqualizerAPO\ASIOProxy"
!define BRAND_MIGRATION_VERSION 1

!define REGPATH "Software\EqualizerAPO"
!define UNINST_REGPATH "Software\Microsoft\Windows\CurrentVersion\Uninstall\EqualizerAPO"
!define INSTALLER_RECOVERY_REGPATH "${REGPATH}\InstallerRecovery"
!define INSTALLER_APP_RECOVERY_REGPATH "${REGPATH}\InstallerAppRecovery"
!define UNINSTALLER_RECOVERY_REGPATH "Software\EqualizerAPOUninstallRecovery"
!define CSIDL_COMMON_APPDATA 0x23
!define FILE_ATTRIBUTE_DIRECTORY 0x10
!define FILE_ATTRIBUTE_REPARSE_POINT 0x400
!define INVALID_FILE_ATTRIBUTES -1
!define ERROR_FILE_NOT_FOUND 2
!define ERROR_PATH_NOT_FOUND 3
!define REG_SZ 1
!define INVALID_HANDLE_VALUE -1
!define FILE_READ_ATTRIBUTES 0x00000080
!define FILE_TYPE_DISK 1
!define FILE_SHARE_READ_WRITE 3
!define FILE_SHARE_READ_WRITE_DELETE 7
!define OPEN_EXISTING 3
!define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
!define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
!define DELETE_ACCESS 0x00010000
!define FILE_DISPOSITION_INFO_CLASS 4
!define WIN32_HKEY_LOCAL_MACHINE 0x80000002
!define KEY_QUERY_VALUE 0x0001
!define KEY_WOW64_64KEY 0x0100
!define KEY_QUERY_VALUE_64 0x0101
!define IMAGE_FILE_MACHINE_AMD64 34404 ; 0x8664
!define IMAGE_FILE_DLL 8192 ; 0x2000
!define INSTALL_RECOVERY_JOURNAL_VERSION 2
!define UNINSTALLER_RECOVERY_JOURNAL_VERSION 2

VIProductVersion "${MAJOR}.${MINOR}.${REVISION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT_FULL_LABEL}"
VIAddVersionKey /LANG=1033 "FileDescription" "${PRODUCT_FULL_LABEL} ${TARGET_ARCH} Installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Equalizer APO contributors"

;--------------------------------
;General

  ;Name and file
  Name "${PRODUCT_FULL_LABEL} ${VERSION}"

  ;Request application privileges for Windows Vista
  RequestExecutionLevel admin

;--------------------------------
;Variables

  Var StartMenuFolder
  Var OldStartMenuFolder
  Var OLDINSTDIR
  Var ProtectedAudioValueExisted
  Var ProtectedAudioValue
  Var ProtectedAudioOverrideActive
  Var MissingAsset
  Var InstallRollbackDirectory
  Var InstallRecoveryCommonAppData
  Var InstallRecoveryProductRoot
  Var InstallRollbackFiles
  Var InstallRollbackState
  Var InstallRollbackCopyCode
  Var InstallRecoveryPhase
  Var InstallRecoveryFailed
  Var InstallRecoveryMarkerPath
  Var InstallRecoveryAclPath
  Var InstallRecoveryTaskXmlPath
  Var InstallRecoveryPathToCheck
  Var InstallRecoveryPathRequired
  Var InstallRecoveryPathExists
  Var InstallRecoveryAclTarget
  Var InstallRecoveryPhysicalPath
  Var InstallRecoveryPhysicalInstallPath
  Var InstallFailureReason
  Var InstallOperationCode
  Var InstallRootAllowMissing
  Var RemoveUserConfigurationRequested
  Var PreviousApoPresent
  Var VerifiedPreviousInstall
  Var NewApoRegistrationAttempted
  Var RenameManifestPath
  Var RenameManifestHandle
  Var RenameManifestWriteFailed
  Var RenameCleanupInstallPrefix
  Var LegacyRenameRepairPath
  Var RenameIdentityPath
  Var RenameIdentityHandle
  Var RenameIdentityFailed
  Var RenameIdentityVolumeSerial
  Var RenameIdentityFileIndexHigh
  Var RenameIdentityFileIndexLow
  Var RenameExpectedVolumeSerial
  Var RenameExpectedFileIndexHigh
  Var RenameExpectedFileIndexLow
  Var ConfigRootIdentityHandle
  Var ConfigRootCreated
  Var ApoRollbackRegistrationCode
  Var ApoRollbackUnregistrationCode
  Var ApoRollbackStatus
  Var FailedRegistrationCode
  Var DeviceSelectorResult
  Var InstallLogPath
  Var DiagnosticIssues
  Var UninstallRecoveryPhase
  Var UninstallRecoveryInstallPath
  Var UninstallUpdateCheckerDone
  Var UninstallDeviceSelectorDone
  Var UninstallApoUnregistered
  Var UninstallAsioProxyUnregistered
  Var UninstallRecoveryStepName
  Var NewAsioRegAttempted
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
  Var AsioProxyEnableCheckbox
  Var AsioTargetCombo
  Var AsioTargetStatusLabel

;--------------------------------
;Interface Settings

  !define MUI_ABORTWARNING
  !define MUI_COMPONENTSPAGE_NODESC
  !define MUI_WELCOMEPAGE_TITLE_3LINES
  !define MUI_LANGDLL_REGISTRY_ROOT "HKLM"
  !define MUI_LANGDLL_REGISTRY_KEY ${REGPATH}
  !define MUI_LANGDLL_REGISTRY_VALUENAME "Installer Language"

;--------------------------------
;Pages

  !insertmacro MUI_PAGE_WELCOME
  !insertmacro MUI_PAGE_LICENSE ..\LICENSE
  !insertmacro MUI_PAGE_DIRECTORY

;Start Menu Folder Page Configuration
  !define MUI_STARTMENUPAGE_REGISTRY_ROOT "HKLM"
  !define MUI_STARTMENUPAGE_REGISTRY_KEY ${REGPATH}
  !define MUI_STARTMENUPAGE_REGISTRY_VALUENAME "Start Menu Folder"

  !insertmacro MUI_PAGE_STARTMENU Application $StartMenuFolder
  !insertmacro MUI_PAGE_COMPONENTS
  !if ${ENABLE_ASIO_PROXY} == 1
    Page Custom AsioProxyPageCreate AsioProxyPageLeave
  !endif
  !insertmacro MUI_PAGE_INSTFILES
  !insertmacro MUI_PAGE_FINISH

  !insertmacro MUI_UNPAGE_WELCOME
  !insertmacro MUI_UNPAGE_CONFIRM
  !insertmacro MUI_UNPAGE_COMPONENTS
  !insertmacro MUI_UNPAGE_INSTFILES
  !insertmacro MUI_UNPAGE_FINISH

;--------------------------------
;Languages

  !insertmacro MUI_LANGUAGE "English"
  !insertmacro MUI_LANGUAGE "Spanish"
  !insertmacro MUI_LANGUAGE "German"
  !insertmacro MUI_LANGUAGE "TradChinese"
  !insertmacro MUI_LANGUAGE "SimpChinese"

;--------------------------------
;Macros
Var renamePath
Var renameIndex
!macro RenameAndDelete path
  ${If} ${FileExists} "${path}"
    StrCpy $renamePath "${path}.old"
    StrCpy $renameIndex "0"
    ${While} ${FileExists} "$renamePath"
      StrCpy $renamePath "${path}.old.$renameIndex"
      IntOp $renameIndex $renameIndex + 1
    ${EndWhile}
    ; Capture the source file identity while holding a share-delete handle. The
    ; cleanup record is therefore tied to the file object, not merely its path.
    StrCpy $RenameIdentityPath "${path}"
    Call QueryRenameFileIdentityAndHold
    ${If} $RenameIdentityFailed == "1"
      DetailPrint "Could not identify an application file before renaming it: ${path}"
      Call RollbackInstallTransaction
      Abort
    ${EndIf}
    ClearErrors
    Rename "${path}" "$renamePath"
    ${If} ${Errors}
      Call CloseRenameIdentityHandle
      DetailPrint "Could not rename an in-use application file: ${path}"
      Call RollbackInstallTransaction
      Abort
    ${EndIf}
    ; Record only a Rename which the operating system confirmed. A crash before
    ; this append can leak a harmless .old file, but cleanup can never adopt a
    ; path which this transaction only planned to create.
    !insertmacro AppendInstallerRecoveryManifestLine \
      $RenameManifestHandle "$RenameManifestPath" \
      "C|$RenameIdentityVolumeSerial|$RenameIdentityFileIndexHigh|$RenameIdentityFileIndexLow|$renamePath|C" \
      $RenameManifestWriteFailed
    ${If} $RenameManifestWriteFailed == "1"
      Call CloseRenameIdentityHandle
      DetailPrint "Could not confirm a renamed application file: $renamePath"
      Call RollbackInstallTransaction
      Abort
    ${EndIf}
    ; Keep the original file object alive until its identity record has been
    ; closed, so its file ID cannot be recycled during the confirmation window.
    Call CloseRenameIdentityHandle
  ${EndIf}
!macroend

!macro RetireLegacyRenameRecoveryArtifact path
  System::Call 'kernel32::GetFileAttributesW(w "${path}") i .r0 ?e'
  Pop $1
  ${If} $0 != ${INVALID_FILE_ATTRIBUTES}
    IntOp $1 $0 & ${FILE_ATTRIBUTE_DIRECTORY}
    ${If} $1 != 0
      Goto retireLegacyRenameManifestFailed
    ${EndIf}
    IntOp $1 $0 & ${FILE_ATTRIBUTE_REPARSE_POINT}
    ${If} $1 != 0
      Goto retireLegacyRenameManifestFailed
    ${EndIf}
    ClearErrors
    Delete "${path}"
    ${If} ${Errors}
      Goto retireLegacyRenameManifestFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto retireLegacyRenameManifestFailed
  ${EndIf}
!macroend

!macro DeleteProductShortcuts folder
  StrCpy $InstallOperationCode "0"
  ${If} "${folder}" != ""
    Delete "$SMPROGRAMS\${folder}\Hibiki EQAPO Configuration Editor.lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Hibiki EQAPO Configuration Editor.lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    Delete "$SMPROGRAMS\${folder}\Hibiki EQAPO Device Selector.lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Hibiki EQAPO Device Selector.lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    Delete "$SMPROGRAMS\${folder}\Equalizer APO Configuration Editor.lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Equalizer APO Configuration Editor.lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    Delete "$SMPROGRAMS\${folder}\Configuration tutorial (online).lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Configuration tutorial (online).lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    Delete "$SMPROGRAMS\${folder}\Configuration reference (online).lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Configuration reference (online).lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    Delete "$SMPROGRAMS\${folder}\Equalizer APO Device Selector.lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Equalizer APO Device Selector.lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    Delete "$SMPROGRAMS\${folder}\Benchmark.lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Benchmark.lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    Delete "$SMPROGRAMS\${folder}\Check for updates.lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Check for updates.lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    Delete "$SMPROGRAMS\${folder}\Uninstall.lnk"
    ${If} ${FileExists} "$SMPROGRAMS\${folder}\Uninstall.lnk"
      StrCpy $InstallOperationCode "1"
    ${EndIf}
    ; Keep unrelated user shortcuts and remove the folder only when empty.
    RMDir "$SMPROGRAMS\${folder}"
    ; A non-empty folder is expected when it contains unrelated shortcuts.
    ClearErrors
  ${EndIf}
!macroend

; Delete or schedule deletion of a known installer-owned payload. Callers check
; InstallRecoveryFailed before removing user data, Uninstall.exe or ARP metadata.
!macro RemoveUninstallPayloadFile path description
  ClearErrors
  Delete /REBOOTOK "${path}"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    StrCpy $InstallFailureReason "removing ${description}"
  ${EndIf}
!macroend

!macro RemoveUninstallPayloadDirectory path description
  ClearErrors
  RMDir /REBOOTOK "${path}"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    StrCpy $InstallFailureReason "removing ${description}"
  ${EndIf}
!macroend

; Execute security-sensitive PowerShell helpers directly from bytes embedded in
; the signed installer. Never extract executable script text to $PLUGINSDIR: a
; medium-integrity process with the same user SID could replace or redirect that
; path between extraction and the elevated PowerShell open.
!macro RunEmbeddedProcessStopper protectInteractive
  StrCpy $InstallOperationCode "process-stop environment setup failed"
  StrCpy $2 1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_PROCESS_INSTALL_ROOT", w "$INSTDIR") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_PROCESS_PROTECT_INTERACTIVE", w "${protectInteractive}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_STOP_CODE_0", w "${STOP_PRODUCT_PROCESSES_CHUNK_0}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_STOP_CODE_1", w "${STOP_PRODUCT_PROCESSES_CHUNK_1}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_STOP_CODE_2", w "${STOP_PRODUCT_PROCESSES_CHUNK_2}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_STOP_CODE_3", w "${STOP_PRODUCT_PROCESSES_CHUNK_3}") i .r1'
  IntOp $2 $2 & $1

  ${If} $2 != 0
    StrCpy $0 "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
    ${IfNot} ${FileExists} "$0"
      StrCpy $0 "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
    ${EndIf}
    ${IfNot} ${FileExists} "$0"
      StrCpy $InstallOperationCode "powershell not found"
    ${Else}
      ; The nsExec argument is single-quoted at the NSIS layer. Keep the
      ; PowerShell predicate quote-free so its comparison operand survives.
      nsExec::ExecToLog '"$0" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$$z=$$env:EQAPO_STOP_CODE_0+$$env:EQAPO_STOP_CODE_1+$$env:EQAPO_STOP_CODE_2+$$env:EQAPO_STOP_CODE_3;$$b=[Convert]::FromBase64String($$z);$$m=[IO.MemoryStream]::new($$b);$$g=[IO.Compression.GzipStream]::new($$m,[IO.Compression.CompressionMode]::Decompress);$$r=[IO.StreamReader]::new($$g,[Text.Encoding]::UTF8);$$p=@{InstallRoot=$$env:EQAPO_PROCESS_INSTALL_ROOT};if([int]$$env:EQAPO_PROCESS_PROTECT_INTERACTIVE -eq 1){$$p.ProtectInteractiveApplications=$$true};&([ScriptBlock]::Create($$r.ReadToEnd())) @p"'
      Pop $InstallOperationCode
    ${EndIf}
  ${EndIf}

  StrCpy $2 1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_PROCESS_INSTALL_ROOT", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_PROCESS_PROTECT_INTERACTIVE", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_STOP_CODE_0", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_STOP_CODE_1", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_STOP_CODE_2", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_STOP_CODE_3", p 0) i .r1'
  IntOp $2 $2 & $1
  ${If} $2 == 0
  ${AndIf} $InstallOperationCode == 0
    StrCpy $InstallOperationCode "process-stop environment cleanup failed"
  ${EndIf}
!macroend

!macro RunEmbeddedX64LoadCheck
  StrCpy $InstallOperationCode "x64-load-check environment setup failed"
  StrCpy $3 1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_X64_DLL_PATH", w "$INSTDIR\EqualizerAPO.dll") i .r1'
  IntOp $3 $3 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_X64_LOG_PATH", w "$InstallLogPath") i .r1'
  IntOp $3 $3 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_X64_CODE_0", w "${X64_LOAD_CHECK_CHUNK_0}") i .r1'
  IntOp $3 $3 & $1

  ${If} $3 != 0
    StrCpy $0 "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
    ${IfNot} ${FileExists} "$0"
      StrCpy $0 "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
    ${EndIf}
    ${IfNot} ${FileExists} "$0"
      StrCpy $InstallOperationCode "powershell not found"
    ${Else}
      nsExec::ExecToLog '"$0" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$$z=$$env:EQAPO_X64_CODE_0;$$b=[Convert]::FromBase64String($$z);$$m=[IO.MemoryStream]::new($$b);$$g=[IO.Compression.GzipStream]::new($$m,[IO.Compression.CompressionMode]::Decompress);$$r=[IO.StreamReader]::new($$g,[Text.Encoding]::UTF8);&([ScriptBlock]::Create($$r.ReadToEnd())) $$env:EQAPO_X64_DLL_PATH $$env:EQAPO_X64_LOG_PATH"'
      Pop $InstallOperationCode
    ${EndIf}
  ${EndIf}

  StrCpy $3 1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_X64_DLL_PATH", p 0) i .r1'
  IntOp $3 $3 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_X64_LOG_PATH", p 0) i .r1'
  IntOp $3 $3 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_X64_CODE_0", p 0) i .r1'
  IntOp $3 $3 & $1
  ${If} $3 == 0
  ${AndIf} $InstallOperationCode == 0
    StrCpy $InstallOperationCode "x64-load-check environment cleanup failed"
  ${EndIf}
  StrCpy $2 "$InstallOperationCode"
!macroend

!macro RequireInstalledAsset path
  ${IfNot} ${FileExists} "${path}"
    StrCpy $MissingAsset "${path}"
    Goto missingRequiredAsset
  ${EndIf}
!macroend

; Remove only files owned by this installer. config and VSTPlugins are
; deliberately outside the transaction because they may contain user data.
!macro DeleteTransactionFile path
  Delete "${path}"
  ${If} ${FileExists} "${path}"
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
!macroend

; Persist previous registry values in the app-tree recovery journal. The
; journal lives in a peer key so protected-audio recovery can be completed and
; cleared independently.
!macro JournalPreviousString root key name journalName
  ClearErrors
  ReadRegStr $0 ${root} "${key}" "${name}"
  ${If} ${Errors}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Existed" 0
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
    ${EndIf}
  ${Else}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Existed" 1
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
    ${Else}
      ClearErrors
      WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Value" "$0"
      ${If} ${Errors}
        StrCpy $InstallRecoveryFailed "1"
      ${EndIf}
    ${EndIf}
  ${EndIf}
!macroend

!macro JournalPreviousDWORD root key name journalName
  ClearErrors
  ReadRegDWORD $0 ${root} "${key}" "${name}"
  ${If} ${Errors}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Existed" 0
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
    ${EndIf}
  ${Else}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Existed" 1
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
    ${Else}
      ClearErrors
      WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Value" $0
      ${If} ${Errors}
        StrCpy $InstallRecoveryFailed "1"
      ${EndIf}
    ${EndIf}
  ${EndIf}
!macroend

!macro RestorePreviousString root key name journalName
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Existed"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
  ${ElseIf} $0 == 1
    ClearErrors
    ReadRegStr $1 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Value"
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
    ${Else}
      ClearErrors
      WriteRegStr ${root} "${key}" "${name}" "$1"
      ${If} ${Errors}
        StrCpy $InstallRecoveryFailed "1"
      ${Else}
        ClearErrors
        ReadRegStr $2 ${root} "${key}" "${name}"
        ${If} ${Errors}
          StrCpy $InstallRecoveryFailed "1"
        ${ElseIf} $2 != $1
          StrCpy $InstallRecoveryFailed "1"
        ${EndIf}
      ${EndIf}
    ${EndIf}
  ${ElseIf} $0 == 0
    DeleteRegValue ${root} "${key}" "${name}"
    ClearErrors
    ReadRegStr $1 ${root} "${key}" "${name}"
    ${IfNot} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
    ${EndIf}
  ${Else}
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
!macroend

!macro RestorePreviousDWORD root key name journalName
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Existed"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
  ${ElseIf} $0 == 1
    ClearErrors
    ReadRegDWORD $1 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "${journalName}Value"
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
    ${Else}
      ClearErrors
      WriteRegDWORD ${root} "${key}" "${name}" $1
      ${If} ${Errors}
        StrCpy $InstallRecoveryFailed "1"
      ${Else}
        ClearErrors
        ReadRegDWORD $2 ${root} "${key}" "${name}"
        ${If} ${Errors}
          StrCpy $InstallRecoveryFailed "1"
        ${ElseIf} $2 != $1
          StrCpy $InstallRecoveryFailed "1"
        ${EndIf}
      ${EndIf}
    ${EndIf}
  ${ElseIf} $0 == 0
    DeleteRegValue ${root} "${key}" "${name}"
    ClearErrors
    ReadRegDWORD $1 ${root} "${key}" "${name}"
    ${IfNot} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
    ${EndIf}
  ${Else}
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
!macroend

!macro WriteRequiredRegStr root key name value description
  ClearErrors
  WriteRegStr ${root} "${key}" "${name}" "${value}"
  ${If} ${Errors}
    StrCpy $InstallFailureReason "${description}"
    Goto installTransactionFailed
  ${EndIf}
!macroend

!macro WriteRequiredRegDWORD root key name value description
  ClearErrors
  WriteRegDWORD ${root} "${key}" "${name}" ${value}
  ${If} ${Errors}
    StrCpy $InstallFailureReason "${description}"
    Goto installTransactionFailed
  ${EndIf}
!macroend

!macro CreateRequiredShortcut link target
  ClearErrors
  CreateShortCut "${link}" "${target}"
  ${If} ${Errors}
    StrCpy $InstallFailureReason "creating shortcut ${link}"
    Goto installTransactionFailed
  ${EndIf}
!macroend

!macro LogLoadLibrary dll
  FileWrite $9 "Checking ${dll}... "
  ${IfNot} ${FileExists} "$INSTDIR\${dll}"
    FileWrite $9 "missing$\r$\n"
    StrCpy $DiagnosticIssues "$DiagnosticIssues- ${dll}: missing$\r$\n"
  ${Else}
    FileWrite $9 "present$\r$\n"
  ${EndIf}
!macroend

!macro LogFileExists file
  FileWrite $9 "Checking ${file}... "
  ${IfNot} ${FileExists} "$INSTDIR\${file}"
    FileWrite $9 "missing$\r$\n"
    StrCpy $DiagnosticIssues "$DiagnosticIssues- ${file}: missing$\r$\n"
  ${Else}
    FileWrite $9 "present$\r$\n"
  ${EndIf}
!macroend

LangString VersionError ${LANG_ENGLISH} "This installer is only supposed to be run on {0} Windows. Please use the {1} installer."
LangString VersionError ${LANG_SPANISH} "Este instalador solo debe ejecutarse en Windows {0}. Use el instalador {1}."
LangString VersionError ${LANG_GERMAN} "Dieses Installationsprogramm kann nur auf einem {0}-Windows verwendet werden. Bitte nutzen Sie die {1}-Version."
LangString VersionError ${LANG_TRADCHINESE} "此安裝程式僅能在 {0} Windows 上執行。請使用 {1} 安裝程式。"
LangString VersionError ${LANG_SIMPCHINESE} "此安装程序仅能在 {0} Windows 上运行。请使用 {1} 安装程序。"

LangString UCRTError ${LANG_ENGLISH} "Your Windows installation is missing required updates to use this program. Please install remaining Windows updates or the Visual C++ Redistributable for Visual Studio 2015 - 2022.$\n$\nDo you want to download the Visual C++ Redistributable now?"
LangString UCRTError ${LANG_SPANISH} "A su instalacion de Windows le faltan actualizaciones necesarias para usar este programa. Instale las actualizaciones pendientes de Windows o Visual C++ Redistributable para Visual Studio 2015 - 2022.$\n$\nDesea descargar Visual C++ Redistributable ahora?"
LangString UCRTError ${LANG_TRADCHINESE} "您的 Windows 系統缺少必要的更新以執行此程式。請安裝最新 Windows 更新或 Visual C++ 可轉散發套件。$\r$\n$\r$\n您要現在下載 Visual C++ 可轉散發套件嗎？"
LangString UCRTError ${LANG_SIMPCHINESE} "您的 Windows 系统缺少运行此程序所需的更新。请安装最新 Windows 更新或 Visual C++ 可再发行组件。$\r$\n$\r$\n您要现在下载 Visual C++ 可再发行组件吗？"
LangString UCRTError ${LANG_GERMAN} "Ihrer Windows-Installation fehlen benötigte Updates, um dieses Programm zu verwenden. Bitte installieren Sie ausstehende Windows-Updates oder das Visual C++ Redistributable für Visual Studio 2015 - 2022.$\n$\nMöchten Sie jetzt das Visual C++ Redistributable herunterladen?"
LangString CloseAppsPrompt ${LANG_ENGLISH} "Setup can close running Hibiki EQAPO applications before installing. Unsaved configuration editor changes may be lost.$\n$\nDo you want setup to close them now?"
LangString CloseAppsPrompt ${LANG_SPANISH} "El instalador puede cerrar aplicaciones de Hibiki EQAPO antes de instalar. Los cambios no guardados del editor de configuracion pueden perderse.$\n$\nDesea que el instalador las cierre ahora?"
LangString CloseAppsPrompt ${LANG_TRADCHINESE} "安裝程式可以在安裝前關閉正在執行的 Hibiki EQAPO 應用程式。未儲存的設定檔編輯器變更可能會遺失。$\r$\n$\r$\n您要現在關閉它們嗎？"
LangString CloseAppsPrompt ${LANG_SIMPCHINESE} "安装程序可以在安装前关闭正在运行的 Hibiki EQAPO 应用程序。未保存的配置编辑器更改可能会丢失。$\r$\n$\r$\n您要现在关闭它们吗？"
LangString CloseAppsPrompt ${LANG_GERMAN} "Das Setup kann laufende Hibiki-EQAPO-Anwendungen vor der Installation schließen. Nicht gespeicherte Änderungen im Konfigurationseditor können verloren gehen.$\n$\nSollen sie jetzt geschlossen werden?"
LangString UninstallCloseAppsPrompt ${LANG_ENGLISH} "Uninstall found running Hibiki EQAPO applications. Unsaved configuration editor changes may be lost.$\r$\n$\r$\nClose them and continue uninstalling?"
LangString UninstallCloseAppsPrompt ${LANG_SPANISH} "La desinstalacion encontro aplicaciones de Hibiki EQAPO en ejecucion. Los cambios no guardados del editor de configuracion pueden perderse.$\r$\n$\r$\nDesea cerrarlas y continuar?"
LangString UninstallCloseAppsPrompt ${LANG_GERMAN} "Das Deinstallationsprogramm hat laufende Hibiki-EQAPO-Anwendungen gefunden. Nicht gespeicherte Änderungen im Konfigurationseditor können verloren gehen.$\r$\n$\r$\nAnwendungen schließen und Deinstallation fortsetzen?"
LangString UninstallCloseAppsPrompt ${LANG_TRADCHINESE} "解除安裝程式發現仍在執行的 Hibiki EQAPO 應用程式；未儲存的設定檔編輯器變更可能會遺失。$\r$\n$\r$\n要關閉它們並繼續解除安裝嗎？"
LangString UninstallCloseAppsPrompt ${LANG_SIMPCHINESE} "卸载程序发现仍在运行的 Hibiki EQAPO 应用程序；未保存的配置编辑器更改可能会丢失。$\r$\n$\r$\n要关闭它们并继续卸载吗？"
LangString RestorePointWarning ${LANG_ENGLISH} "Setup could not create a Windows restore point.$\n$\nThis can happen when System Protection is disabled, or when a restore point already exists from the last 24 hours. By default, Windows policy may block creating more than one restore point within the same 24-hour period.$\n$\nInstallation will continue."
LangString RestorePointWarning ${LANG_SPANISH} "El instalador no pudo crear un punto de restauracion de Windows.$\n$\nEsto puede ocurrir si Proteccion del sistema esta desactivada, o si ya existe un punto de restauracion creado en las ultimas 24 horas. De forma predeterminada, las politicas de Windows pueden bloquear la creacion de mas de un punto de restauracion dentro del mismo periodo de 24 horas.$\n$\nLa instalacion continuara."
LangString RestorePointWarning ${LANG_TRADCHINESE} "安裝程式無法建立 Windows 系統還原點。$\r$\n$\r$\n系統保護停用或過去 24 小時內已有還原點時，可能發生此情況。Windows 預設可能限制 24 小時內只能建立一個還原點。$\r$\n$\r$\n安裝將繼續。"
LangString RestorePointWarning ${LANG_SIMPCHINESE} "安装程序无法创建 Windows 系统还原点。$\r$\n$\r$\n系统保护被禁用或过去 24 小时内已有还原点时，可能发生此情况。Windows 默认可能限制 24 小时内只能创建一个还原点。$\r$\n$\r$\n安装将继续。"
LangString RestorePointWarning ${LANG_GERMAN} "Das Setup konnte keinen Windows-Wiederherstellungspunkt erstellen.$\n$\nDies kann passieren, wenn der Computerschutz deaktiviert ist oder wenn bereits ein Wiederherstellungspunkt aus den letzten 24 Stunden existiert. Standardmäßig kann Windows verhindern, dass innerhalb desselben 24-Stunden-Zeitraums mehr als ein Wiederherstellungspunkt erstellt wird.$\n$\nDie Installation wird fortgesetzt."

LangString AssetValidationError ${LANG_ENGLISH} "A required installation file is missing or could not be extracted. Setup will stop before registering Hibiki EQAPO or modifying audio devices.$\r$\n$\r$\nMissing file: $MissingAsset"
LangString AssetValidationError ${LANG_SPANISH} "Falta un archivo de instalación necesario o no se pudo extraer. El instalador se detendrá antes de registrar Hibiki EQAPO o modificar dispositivos de audio.$\r$\n$\r$\nArchivo faltante: $MissingAsset"
LangString AssetValidationError ${LANG_GERMAN} "Eine erforderliche Installationsdatei fehlt oder konnte nicht extrahiert werden. Das Setup wird beendet, bevor Hibiki EQAPO registriert oder Audiogeräte geändert werden.$\r$\n$\r$\nFehlende Datei: $MissingAsset"
LangString AssetValidationError ${LANG_TRADCHINESE} "必要的安裝檔案遺失或無法解壓縮。安裝程式將在註冊 Hibiki EQAPO 或修改音訊裝置前停止。$\r$\n$\r$\n遺失檔案：$MissingAsset"
LangString AssetValidationError ${LANG_SIMPCHINESE} "必要的安装文件缺失或无法解压缩。安装程序将在注册 Hibiki EQAPO 或修改音频设备前停止。$\r$\n$\r$\n缺失文件：$MissingAsset"

!if ${ENABLE_ASIO_PROXY} == 1
!include "AsioProxy.nsh"
!endif

;--------------------------------
;Functions
Function BlockIfUninstallRecoveryJournalExists
  ; A partial uninstall may have removed the helpers whose completion flags are
  ; recorded here. Setup must not repair/re-register the product while those
  ; flags remain replayable, or a retry could skip cleanup of the repaired state.
  ; Any existing key, including a malformed/partial one, is fail-closed and is
  ; retained for the original uninstaller or explicit administrator inspection.
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $InstallFailureReason ""
  !if ${LIBPATH} != "lib32"
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  !else
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE}, *p .r0) i .r1'
  !endif
  ${If} $1 == ${ERROR_FILE_NOT_FOUND}
    Return
  ${ElseIf} $1 != 0
    StrCpy $InstallFailureReason "the uninstall recovery journal could not be inspected (registry error: $1)"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  ${If} $1 != 0
    StrCpy $InstallFailureReason "the uninstall recovery journal could not be closed safely (registry error: $1)"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $0 HKLM ${UNINSTALLER_RECOVERY_REGPATH} "JournalVersion"
  ${If} ${Errors}
  ${OrIf} $0 != ${UNINSTALLER_RECOVERY_JOURNAL_VERSION}
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ClearErrors
  ReadRegStr $UninstallRecoveryInstallPath HKLM ${UNINSTALLER_RECOVERY_REGPATH} "InstallPath"
  ${If} ${Errors}
  ${OrIf} $UninstallRecoveryInstallPath == ""
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Pending"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ClearErrors
  ReadRegStr $UninstallRecoveryPhase HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ClearErrors
  ReadRegDWORD $UninstallUpdateCheckerDone HKLM ${UNINSTALLER_RECOVERY_REGPATH} "UpdateCheckerDone"
  ${If} ${Errors}
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ClearErrors
  ReadRegDWORD $UninstallDeviceSelectorDone HKLM ${UNINSTALLER_RECOVERY_REGPATH} "DeviceSelectorDone"
  ${If} ${Errors}
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ClearErrors
  ReadRegDWORD $UninstallApoUnregistered HKLM ${UNINSTALLER_RECOVERY_REGPATH} "ApoUnregistered"
  ${If} ${Errors}
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ClearErrors
  ReadRegDWORD $UninstallAsioProxyUnregistered HKLM ${UNINSTALLER_RECOVERY_REGPATH} "AsioProxyUnregistered"
  ${If} ${Errors}
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ${If} $UninstallUpdateCheckerDone != 0
  ${AndIf} $UninstallUpdateCheckerDone != 1
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ${If} $UninstallDeviceSelectorDone != 0
  ${AndIf} $UninstallDeviceSelectorDone != 1
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ${If} $UninstallApoUnregistered != 0
  ${AndIf} $UninstallApoUnregistered != 1
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}
  ${If} $UninstallAsioProxyUnregistered != 0
  ${AndIf} $UninstallAsioProxyUnregistered != 1
    Goto malformedUninstallRecoveryBeforeSetup
  ${EndIf}

  ${If} $UninstallRecoveryPhase == "critical"
    Goto validUninstallRecoveryBeforeSetup
  ${ElseIf} $UninstallRecoveryPhase == "payload"
    ${If} $UninstallUpdateCheckerDone != 1
    ${OrIf} $UninstallDeviceSelectorDone != 1
    ${OrIf} $UninstallApoUnregistered != 1
    ${OrIf} $UninstallAsioProxyUnregistered != 1
      Goto malformedUninstallRecoveryBeforeSetup
    ${EndIf}
    Goto validUninstallRecoveryBeforeSetup
  ${EndIf}
  Goto malformedUninstallRecoveryBeforeSetup

  validUninstallRecoveryBeforeSetup:
  StrCpy $InstallFailureReason "a pending uninstall transaction (phase: $UninstallRecoveryPhase)"
  StrCpy $InstallRecoveryFailed "1"
  Return

  malformedUninstallRecoveryBeforeSetup:
  StrCpy $InstallFailureReason "an unknown, malformed or partial uninstall recovery journal"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function .onInit
  !if ${LIBPATH} != "lib32"
    SetRegView 64
  !endif
  ; This registry-only gate must precede every installer recovery mutation.
  StrCpy $InstallRecoveryFailed "0"
  Call BlockIfUninstallRecoveryJournalExists
  ${If} $InstallRecoveryFailed == "1"
    DetailPrint "Setup refused $InstallFailureReason. The uninstall recovery journal was retained."
    ${IfNot} ${Silent}
      MessageBox MB_ICONSTOP|MB_OK "Setup cannot safely repair or install the product while an earlier uninstall is incomplete. Run the installed Uninstall.exe to complete the existing uninstall, then start Setup again.$\r$\n$\r$\nThe uninstall recovery journal was retained."
    ${EndIf}
    SetErrorLevel 1
    Abort
  ${EndIf}
  ; Repair an interrupted prior registration attempt before reading any other
  ; installer state. The journal is written before the temporary audio override.
  Call RecoverProtectedAudioSetting
  ${If} $ProtectedAudioOverrideActive == "1"
    ${IfNot} ${Silent}
      MessageBox MB_ICONSTOP|MB_OK "Setup could not recover the protected-audio setting left by an interrupted installation. No installation files will be changed."
    ${EndIf}
    Abort
  ${EndIf}
  ; The application snapshot has its own durable journal and fixed recovery
  ; directory. Recover it before reading or overwriting normal installer state.
  Call RecoverInstallTransaction
  ${If} $InstallRecoveryFailed == "1"
    ${IfNot} ${Silent}
      MessageBox MB_ICONSTOP|MB_OK "Setup found an interrupted installation but could not recover it safely. The recovery journal and snapshot were retained; no new installation files will be changed."
    ${EndIf}
    Abort
  ${EndIf}
  !insertmacro MUI_LANGDLL_DISPLAY
  ;Get installation folder from registry if available
  ReadRegStr $INSTDIR HKLM ${REGPATH} "InstallPath"

  ;Use default installation folder otherwise
  ${If} $INSTDIR == ""
    StrCpy $INSTDIR "$PROGRAMFILES64\EqualizerAPO"
  ${EndIf}

  !insertmacro MUI_STARTMENU_GETFOLDER Application $StartMenuFolder
  ${If} ${IsNativeIA32}
    StrCpy $0 "x86"
  ${ElseIf} ${IsNativeAMD64}
    StrCpy $0 "x64"
  ${ElseIf} ${IsNativeARM64}
    StrCpy $0 "ARM64"
  ${EndIf}

  ${If} $0 != ${TARGET_ARCH}
    ${IfNot} ${Silent}
      MessageBox MB_OK|MB_ICONSTOP "This installer is only supposed to be run on ${TARGET_ARCH} Windows. Please use the $0 installer."
    ${EndIf}
    Abort
  ${EndIf}

  ${IfNot} ${AtLeastWin10}
    System::Call 'KERNEL32::LoadLibrary(t "ucrtbase.dll")p.r0'
    ${If} $0 P= 0
      ${IfNot} ${Silent}
        MessageBox MB_YESNO|MB_ICONSTOP $(UCRTError) IDNO skipDownload
        ExecShell "open" "${VCREDIST_URL}"
      ${EndIf}
      skipDownload:
      Abort
    ${EndIf}
  ${EndIf}
  !if ${ENABLE_ASIO_PROXY} == 1
    StrCpy $InstallRecoveryFailed "0"
    Call ResolveInitialAsioSelection
    ${If} $InstallRecoveryFailed == "1"
      DetailPrint "Setup stopped while $InstallFailureReason."
      ${IfNot} ${Silent}
        MessageBox MB_ICONSTOP|MB_OK "Setup could not configure Hibiki EQAPO DAW monitoring while $InstallFailureReason. No installation files were changed."
      ${EndIf}
      SetErrorLevel 1
      Abort
    ${EndIf}
  !endif
FunctionEnd

Function CloseRunningApplications
  ${If} ${FileExists} "$INSTDIR"
    ${IfNot} ${Silent}
      MessageBox MB_YESNO|MB_ICONQUESTION $(CloseAppsPrompt) IDNO done
    ${EndIf}
    Call StopInstalledProductProcesses
    ${If} $InstallOperationCode != 0
      ${IfNot} ${Silent}
        MessageBox MB_ICONSTOP|MB_OK "Setup could not safely stop an installed Hibiki EQAPO process. No installation files were changed. Close the application and try again."
      ${EndIf}
      Abort
    ${EndIf}
  ${EndIf}
  done:
FunctionEnd

Function StopInstalledProductProcesses
  Push $OUTDIR
  SetOutPath "$SYSDIR"
  !insertmacro RunEmbeddedProcessStopper "0"
  Pop $OUTDIR
  SetOutPath $OUTDIR
FunctionEnd

!macro ValidateInstallRootAclBody
  ; Pass the path through the child process environment so quotes or metacharacters
  ; in a custom path can never change the PowerShell command line. The embedded
  ; validator uses .NET directly and does not auto-load modules from PSModulePath.
  StrCpy $InstallOperationCode "install-root ACL validator could not start"
  StrCpy $2 1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_INSTALL_ROOT", w "$INSTDIR") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ALLOW_MISSING_INSTALL_ROOT", w "$InstallRootAllowMissing") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_0", w "${INSTALL_ROOT_ACL_CHECK_CHUNK_0}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_1", w "${INSTALL_ROOT_ACL_CHECK_CHUNK_1}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_2", w "${INSTALL_ROOT_ACL_CHECK_CHUNK_2}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_3", w "${INSTALL_ROOT_ACL_CHECK_CHUNK_3}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_4", w "${INSTALL_ROOT_ACL_CHECK_CHUNK_4}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_5", w "${INSTALL_ROOT_ACL_CHECK_CHUNK_5}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_6", w "${INSTALL_ROOT_ACL_CHECK_CHUNK_6}") i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_7", w "${INSTALL_ROOT_ACL_CHECK_CHUNK_7}") i .r1'
  IntOp $2 $2 & $1

  ${If} $2 == 0
    StrCpy $InstallOperationCode "install-root environment setup failed"
  ${Else}
    StrCpy $0 "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
    ${IfNot} ${FileExists} "$0"
      StrCpy $0 "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
    ${EndIf}
    ${IfNot} ${FileExists} "$0"
      StrCpy $InstallOperationCode "powershell not found"
    ${Else}
      nsExec::ExecToLog '"$0" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$$z=$$env:EQAPO_ACL_CODE_0+$$env:EQAPO_ACL_CODE_1+$$env:EQAPO_ACL_CODE_2+$$env:EQAPO_ACL_CODE_3+$$env:EQAPO_ACL_CODE_4+$$env:EQAPO_ACL_CODE_5+$$env:EQAPO_ACL_CODE_6+$$env:EQAPO_ACL_CODE_7;$$b=[Convert]::FromBase64String($$z);$$m=[IO.MemoryStream]::new($$b);$$g=[IO.Compression.GzipStream]::new($$m,[IO.Compression.CompressionMode]::Decompress);$$r=[IO.StreamReader]::new($$g,[Text.Encoding]::UTF8);&([ScriptBlock]::Create($$r.ReadToEnd()))"'
      Pop $InstallOperationCode
    ${EndIf}
  ${EndIf}

  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_INSTALL_ROOT", p 0) i .r1'
  StrCpy $2 $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ALLOW_MISSING_INSTALL_ROOT", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_0", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_1", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_2", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_3", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_4", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_5", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_6", p 0) i .r1'
  IntOp $2 $2 & $1
  System::Call 'kernel32::SetEnvironmentVariableW(w "EQAPO_ACL_CODE_7", p 0) i .r1'
  IntOp $2 $2 & $1
  ${If} $2 == 0
  ${AndIf} $InstallOperationCode == 0
    StrCpy $InstallOperationCode "install-root environment cleanup failed"
  ${EndIf}
!macroend

Function ValidateInstallRootAcl
  !insertmacro ValidateInstallRootAclBody
FunctionEnd

Function CreateRestorePoint
  DetailPrint "Creating Windows restore point..."
  StrCpy $0 "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  ${IfNot} ${FileExists} "$0"
    StrCpy $0 "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
  ${EndIf}

  ${If} ${FileExists} "$0"
    nsExec::ExecToLog '"$0" -NoProfile -ExecutionPolicy Bypass -Command "try { Checkpoint-Computer -Description Hibiki_EQAPO_${VERSION}_PreInstall -RestorePointType APPLICATION_INSTALL -ErrorAction Stop; exit 0 } catch { exit 1 }"'
    Pop $1
    ${If} $1 == 0
      DetailPrint "Windows restore point created."
    ${Else}
      DetailPrint "Windows restore point was not created. PowerShell exit code: $1"
      ${IfNot} ${Silent}
        MessageBox MB_ICONEXCLAMATION|MB_OK $(RestorePointWarning)
      ${EndIf}
    ${EndIf}
  ${Else}
    DetailPrint "PowerShell was not found. Skipping restore point creation."
    ${IfNot} ${Silent}
      MessageBox MB_ICONEXCLAMATION|MB_OK $(RestorePointWarning)
    ${EndIf}
  ${EndIf}
FunctionEnd

Function SaveProtectedAudioSetting
  ClearErrors
  ReadRegDWORD $ProtectedAudioValue HKLM "Software\Microsoft\Windows\CurrentVersion\Audio" "DisableProtectedAudioDG"
  ${If} ${Errors}
    StrCpy $ProtectedAudioValueExisted "0"
    StrCpy $ProtectedAudioValue "0"
  ${Else}
    StrCpy $ProtectedAudioValueExisted "1"
  ${EndIf}
FunctionEnd

Function BeginProtectedAudioOverride
  ; Never overwrite a still-pending journal from an earlier attempt in this
  ; process. If it cannot be restored, the caller must not run regsvr32.
  ${If} $ProtectedAudioOverrideActive == "1"
    Call RestoreProtectedAudioSetting
    ${If} $ProtectedAudioOverrideActive == "1"
      Return
    ${EndIf}
  ${EndIf}

  Call SaveProtectedAudioSetting
  ; Journal value data first and mark it pending last. The protected-audio value
  ; is changed only after the durable pending marker exists.
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_RECOVERY_REGPATH} "ValueExisted" $ProtectedAudioValueExisted
  ${If} ${Errors}
    Goto protectedAudioJournalFailed
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_RECOVERY_REGPATH} "Value" $ProtectedAudioValue
  ${If} ${Errors}
    Goto protectedAudioJournalFailed
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_RECOVERY_REGPATH} "Pending" 1
  ${If} ${Errors}
    Goto protectedAudioJournalFailed
  ${EndIf}
  StrCpy $ProtectedAudioOverrideActive "1"
  ClearErrors
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Audio" "DisableProtectedAudioDG" 1
  ${If} ${Errors}
    Call RestoreProtectedAudioSetting
  ${EndIf}
  Return

  protectedAudioJournalFailed:
  DeleteRegKey HKLM ${INSTALLER_RECOVERY_REGPATH}
  DeleteRegKey /ifempty HKLM ${REGPATH}
  StrCpy $ProtectedAudioOverrideActive "0"
  DetailPrint "Could not create the protected-audio recovery journal."
FunctionEnd

Function RestoreProtectedAudioSetting
  StrCpy $ProtectedAudioOverrideActive "1"
  ${If} $ProtectedAudioValueExisted == "1"
    ClearErrors
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Audio" "DisableProtectedAudioDG" $ProtectedAudioValue
    ${If} ${Errors}
      DetailPrint "Could not restore the protected-audio registry value; the recovery journal was retained."
      Return
    ${EndIf}
    ClearErrors
    ReadRegDWORD $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Audio" "DisableProtectedAudioDG"
    ${If} ${Errors}
      DetailPrint "Could not verify the restored protected-audio registry value; the recovery journal was retained."
      Return
    ${ElseIf} $0 != $ProtectedAudioValue
      DetailPrint "The protected-audio registry value did not verify; the recovery journal was retained."
      Return
    ${EndIf}
  ${Else}
    DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Audio" "DisableProtectedAudioDG"
    ; Verify absence rather than trusting DeleteRegValue's error flag; deleting
    ; a value which is already absent is also a successful restoration.
    ClearErrors
    ReadRegDWORD $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Audio" "DisableProtectedAudioDG"
    ${IfNot} ${Errors}
      DetailPrint "Could not remove the temporary protected-audio registry value; the recovery journal was retained."
      Return
    ${EndIf}
  ${EndIf}

  DeleteRegKey HKLM ${INSTALLER_RECOVERY_REGPATH}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_RECOVERY_REGPATH} "Pending"
  ${IfNot} ${Errors}
    DetailPrint "Could not clear the protected-audio recovery journal."
    Return
  ${EndIf}
  DeleteRegKey /ifempty HKLM ${REGPATH}
  StrCpy $ProtectedAudioOverrideActive "0"
FunctionEnd

Function RecoverProtectedAudioSetting
  StrCpy $ProtectedAudioOverrideActive "0"
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_RECOVERY_REGPATH} "Pending"
  ${If} ${Errors}
    Return
  ${EndIf}
  ${If} $0 != 1
    ; A present but malformed pending marker must not be overwritten because its
    ; intended recovery state is unknown.
    StrCpy $ProtectedAudioOverrideActive "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $1 HKLM ${INSTALLER_RECOVERY_REGPATH} "ValueExisted"
  ${If} ${Errors}
    StrCpy $ProtectedAudioOverrideActive "1"
    Return
  ${EndIf}
  ${If} $1 != 0
  ${AndIf} $1 != 1
    StrCpy $ProtectedAudioOverrideActive "1"
    Return
  ${EndIf}
  ReadRegDWORD $2 HKLM ${INSTALLER_RECOVERY_REGPATH} "Value"
  ${If} ${Errors}
    StrCpy $ProtectedAudioOverrideActive "1"
    Return
  ${EndIf}
  StrCpy $ProtectedAudioValueExisted "$1"
  StrCpy $ProtectedAudioValue "$2"
  StrCpy $ProtectedAudioOverrideActive "1"
  Call RestoreProtectedAudioSetting
  ${If} $ProtectedAudioOverrideActive == "0"
    DetailPrint "Recovered the protected-audio setting from an interrupted installation."
  ${EndIf}
FunctionEnd

Function InitializeInstallRecoveryPaths
  ; Never load these paths from the journal. Keeping the recovery root fixed
  ; prevents a stale or malformed registry value from selecting a broad tree for
  ; recursive cleanup.
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $InstallRecoveryCommonAppData ""
  StrCpy $InstallRecoveryProductRoot ""
  StrCpy $InstallRollbackDirectory ""
  System::Call 'shell32::SHGetFolderPathW(p 0, i ${CSIDL_COMMON_APPDATA}, p 0, i 0, w .r0) i .r1'
  ${If} $1 != 0
  ${OrIf} $0 == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  GetFullPathName $2 "$0"
  ${If} ${Errors}
  ${OrIf} $2 == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryCommonAppData "$2"
  StrCpy $InstallRecoveryProductRoot "$InstallRecoveryCommonAppData\EqualizerAPO"
  StrCpy $InstallRollbackDirectory "$InstallRecoveryProductRoot\InstallerRecovery"
  StrCpy $InstallRollbackFiles "$InstallRollbackDirectory\files"
  StrCpy $RenameManifestPath "$InstallRollbackDirectory\renamed-files.txt"
  StrCpy $InstallRecoveryMarkerPath "$InstallRollbackDirectory\app-tree.marker"
  StrCpy $InstallRecoveryAclPath "$InstallRollbackDirectory\config-acl.txt"
  StrCpy $InstallRecoveryTaskXmlPath "$InstallRollbackDirectory\update-task.xml"
FunctionEnd

Function ValidateInstallRecoveryComponent
  ; Inputs are carried in dedicated variables so callers cannot accidentally
  ; trust a path loaded from the recovery journal. Missing optional descendants
  ; are allowed; access errors and every existing reparse point fail closed.
  StrCpy $InstallRecoveryPathExists "0"
  System::Call 'kernel32::GetFileAttributesW(w "$InstallRecoveryPathToCheck") i .r0 ?e'
  Pop $1
  ${If} $0 == ${INVALID_FILE_ATTRIBUTES}
    ${If} $InstallRecoveryPathRequired == "0"
      ${If} $1 == ${ERROR_FILE_NOT_FOUND}
      ${OrIf} $1 == ${ERROR_PATH_NOT_FOUND}
        Return
      ${EndIf}
    ${EndIf}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  IntOp $1 $0 & ${FILE_ATTRIBUTE_DIRECTORY}
  ${If} $1 == 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $1 $0 & ${FILE_ATTRIBUTE_REPARSE_POINT}
  ${If} $1 != 0
    DetailPrint "Refusing a recovery path containing a reparse point: $InstallRecoveryPathToCheck"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryPathExists "1"
FunctionEnd

Function ValidateInstallRecoveryComponents
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryCommonAppData"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackDirectory"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackFiles"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
FunctionEnd

Function HardenInstallRecoveryDirectory
  ; Apply a protected DACL in one Win32 operation. The SDDL grants full control
  ; only to SYSTEM and built-in Administrators and avoids an icacls reset window
  ; in which inherited Users permissions could briefly return.
  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryAclTarget"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  StrCpy $0 0
  System::Call 'advapi32::ConvertStringSecurityDescriptorToSecurityDescriptorW(w "O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", i 1, *p .r0, p 0) i .r1'
  ${If} $1 == 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call 'advapi32::SetFileSecurityW(w "$InstallRecoveryAclTarget", i 0x80000007, p r0) i .r2 ?e'
  Pop $3
  System::Call 'kernel32::LocalFree(p r0) p .r4'
  ${If} $2 == 0
    DetailPrint "Could not secure recovery ACL for $InstallRecoveryAclTarget (Win32 error $3)."
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryAclTarget"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
FunctionEnd

Function CreateInstallRecoveryDirectoryAtomically
  ; Official NSIS stubs are 32-bit, so SECURITY_ATTRIBUTES is 12 bytes. Pass a
  ; protected SDDL at CreateDirectoryW time: no inherited writable-ACL window is
  ; exposed between directory creation and hardening.
  StrCpy $0 0
  System::Call 'advapi32::ConvertStringSecurityDescriptorToSecurityDescriptorW(w "O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", i 1, *p .r0, p 0) i .r1'
  ${If} $1 == 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call '*(i 12, p r0, i 0) p .r2'
  ${If} $2 == 0
    System::Call 'kernel32::LocalFree(p r0) p .r5'
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call 'kernel32::CreateDirectoryW(w "$InstallRecoveryAclTarget", p r2) i .r3 ?e'
  Pop $4
  System::Free $2
  System::Call 'kernel32::LocalFree(p r0) p .r5'
  ${If} $3 == 0
    DetailPrint "Could not atomically create secure recovery directory $InstallRecoveryAclTarget (Win32 error $4)."
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryAclTarget"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
FunctionEnd

Function SecureExistingInstallRecoveryTree
  Call ValidateInstallRecoveryComponents
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ProductRootCreated"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ; Pending is persisted before filesystem creation. If power is lost in that
  ; interval, ProductRootCreated=0 and no tree is a clean, recoverable state.
  ; Conversely, a tree with no ownership marker is never adopted or deleted.
  ${If} $0 == 0
    StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
    StrCpy $InstallRecoveryPathRequired "0"
    Call ValidateInstallRecoveryComponent
    ${If} $InstallRecoveryFailed == "1"
      Return
    ${ElseIf} $InstallRecoveryPathExists == "0"
      Return
    ${EndIf}
    DetailPrint "A recovery product root exists without a verified ownership marker and was retained: $InstallRecoveryProductRoot"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${ElseIf} $0 != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ${If} $InstallRecoveryPathExists == "0"
    Return
  ${EndIf}

  ; A missing recovery child means recursive cleanup already finished. Its
  ; secured parent is handled non-recursively by the journaled cleanup caller.
  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackDirectory"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
  ${OrIf} $InstallRecoveryPathExists == "0"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryAclTarget "$InstallRecoveryProductRoot"
  Call HardenInstallRecoveryDirectory
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryAclTarget "$InstallRollbackDirectory"
  Call HardenInstallRecoveryDirectory
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackFiles"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ${If} $InstallRecoveryPathExists == "1"
    StrCpy $InstallRecoveryAclTarget "$InstallRollbackFiles"
    Call HardenInstallRecoveryDirectory
    ${If} $InstallRecoveryFailed == "1"
      Return
    ${EndIf}
  ${EndIf}
  Call ValidateInstallRecoveryComponents
FunctionEnd

Function ValidateActiveInstallRecoverySnapshot
  ; Destructive rollback is allowed only with a complete, authenticated active
  ; snapshot. Cleanup phases deliberately use the optional validator instead.
  Call SecureExistingInstallRecoveryTree
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ProductRootCreated"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryCommonAppData"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackDirectory"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackFiles"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  System::Call 'kernel32::GetFileAttributesW(w "$InstallRecoveryMarkerPath") i .r0 ?e'
  Pop $1
  ${If} $0 == ${INVALID_FILE_ATTRIBUTES}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $1 $0 & ${FILE_ATTRIBUTE_DIRECTORY}
  ${If} $1 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $1 $0 & ${FILE_ATTRIBUTE_REPARSE_POINT}
  ${If} $1 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  FileOpen $0 "$InstallRecoveryMarkerPath" r
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  FileRead $0 $1
  ${If} ${Errors}
    FileClose $0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  FileClose $0
  ${StrTrimNewLines} $1 "$1"
  ${If} $1 != "EqualizerAPO installer app-tree recovery v1"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ; Revalidate the protected directory chain immediately before the caller uses
  ; it. Its SYSTEM/Administrators-only ACL prevents a lower-privilege swap.
  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackFiles"
  StrCpy $InstallRecoveryPathRequired "1"
  Call ValidateInstallRecoveryComponent
FunctionEnd

Function CreateSecureInstallRecoveryTree
  ; The journal is already durable before this function is entered. An existing
  ; recovery directory is never adopted as ours.
  Call ValidateInstallRecoveryComponents
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackDirectory"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
  ${OrIf} $InstallRecoveryPathExists == "1"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ; The parent product root is also a security boundary. Never adopt a directory
  ; which could have been pre-created by an unprivileged user.
  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
  ${OrIf} $InstallRecoveryPathExists == "1"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryAclTarget "$InstallRecoveryProductRoot"
  Call CreateInstallRecoveryDirectoryAtomically
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ProductRootCreated" 1
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ProductRootCreated"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryAclTarget "$InstallRollbackDirectory"
  Call CreateInstallRecoveryDirectoryAtomically
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryAclTarget "$InstallRollbackFiles"
  Call CreateInstallRecoveryDirectoryAtomically
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  Call ValidateInstallRecoveryComponents
FunctionEnd

Function ResolveInstallRecoveryPhysicalPath
  ; Resolve every existing ancestor reparse point through a directory handle.
  ; GetFullPathName is only lexical and is not sufficient for the install versus
  ; recovery containment decision.
  StrCpy $InstallRecoveryPhysicalPath ""
  ClearErrors
  System::Call 'kernel32::CreateFileW(w "$InstallRecoveryPathToCheck", i 0, i ${FILE_SHARE_READ_WRITE_DELETE}, p 0, i ${OPEN_EXISTING}, i ${FILE_FLAG_BACKUP_SEMANTICS}, p 0) p .r0 ?e'
  Pop $1
  ${If} $0 == ${INVALID_HANDLE_VALUE}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  System::Alloc 4096
  Pop $2
  ${If} $2 == 0
    System::Call 'kernel32::CloseHandle(p r0) i .r3'
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call 'kernel32::GetFinalPathNameByHandleW(p r0, p r2, i 2048, i 0) i .r3 ?e'
  Pop $1
  System::Call 'kernel32::CloseHandle(p r0) i .r5'
  ${If} $3 == 0
  ${OrIf} $3 >= 1024
    System::Free $2
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call '*$2(&w1024 .r4)'
  System::Free $2
  ${If} $4 == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ; Normalize the Win32 extended prefixes before delimiter-aware comparison.
  StrCpy $5 "$4" 8
  ${If} $5 == "\\?\UNC\"
    StrCpy $6 "$4" "" 8
    StrCpy $4 "\\$6"
  ${Else}
    StrCpy $5 "$4" 4
    ${If} $5 == "\\?\"
      StrCpy $4 "$4" "" 4
    ${EndIf}
  ${EndIf}
  normalizePhysicalPathTail:
  StrCpy $5 "$4" 1 -1
  ${If} $5 == "\"
    ${GetRoot} "$4" $6
    ${If} $4 != $6
    ${AndIf} $4 != "$6\"
      StrCpy $4 "$4" -1
      Goto normalizePhysicalPathTail
    ${EndIf}
  ${EndIf}
  StrCpy $InstallRecoveryPhysicalPath "$4"
FunctionEnd

Function ValidateInstallRecoveryTarget
  StrCpy $InstallRecoveryFailed "0"
  ${If} $INSTDIR == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ClearErrors
  GetFullPathName $0 "$INSTDIR"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  normalizeInstallPathTail:
  StrCpy $6 "$0" 1 -1
  ${If} $6 == "\"
    ${GetRoot} "$0" $7
    ${If} $0 != $7
    ${AndIf} $0 != "$7\"
      StrCpy $0 "$0" -1
      Goto normalizeInstallPathTail
    ${EndIf}
  ${EndIf}
  StrCpy $INSTDIR "$0"
  ; GetFullPathName fails when the final directory does not exist, which is the
  ; normal state before the first recovery snapshot is created. Canonicalize the
  ; existing trusted Common AppData root, then append only fixed product-owned
  ; components. None of these descendants are loaded from installer input or the
  ; recovery journal.
  ClearErrors
  GetFullPathName $2 "$InstallRecoveryCommonAppData"
  ${If} ${Errors}
  ${OrIf} $2 == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  normalizeCommonAppDataTail:
  StrCpy $6 "$2" 1 -1
  ${If} $6 == "\"
    ${GetRoot} "$2" $7
    ${If} $2 != $7
    ${AndIf} $2 != "$7\"
      StrCpy $2 "$2" -1
      Goto normalizeCommonAppDataTail
    ${EndIf}
  ${EndIf}
  StrCpy $InstallRecoveryCommonAppData "$2"
  StrCpy $InstallRecoveryProductRoot "$InstallRecoveryCommonAppData\EqualizerAPO"
  StrCpy $InstallRollbackDirectory "$InstallRecoveryProductRoot\InstallerRecovery"
  StrCpy $InstallRollbackFiles "$InstallRollbackDirectory\files"
  StrCpy $RenameManifestPath "$InstallRollbackDirectory\renamed-files.txt"
  StrCpy $InstallRecoveryMarkerPath "$InstallRollbackDirectory\app-tree.marker"
  StrCpy $InstallRecoveryAclPath "$InstallRollbackDirectory\config-acl.txt"
  StrCpy $InstallRecoveryTaskXmlPath "$InstallRollbackDirectory\update-task.xml"
  StrLen $1 "$INSTDIR"
  ${If} $1 < 4
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ${GetRoot} "$INSTDIR" $0
  ${If} $INSTDIR == $0
  ${OrIf} $INSTDIR == "$0\"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ${If} $INSTDIR == "$WINDIR"
  ${OrIf} $INSTDIR == "$SYSDIR"
  ${OrIf} $INSTDIR == "$PROGRAMFILES"
  ${OrIf} $INSTDIR == "$PROGRAMFILES32"
  ${OrIf} $INSTDIR == "$PROGRAMFILES64"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ; Compare canonical paths with a delimiter on both sides. This rejects exact,
  ; ancestor, and descendant relationships without treating InstallerRecovery2
  ; as a child of InstallerRecovery.
  StrCpy $2 "$INSTDIR\"
  StrCpy $3 "$InstallRollbackDirectory\"
  StrLen $4 "$2"
  StrCpy $5 "$3" $4
  ${If} $5 == $2
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  StrLen $4 "$3"
  StrCpy $5 "$2" $4
  ${If} $5 == $3
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ; Lexical normalization cannot see an ancestor junction. Resolve the existing
  ; install directory and Common AppData through handles, then repeat the same
  ; exact/ancestor/descendant comparison against their physical paths.
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR"
  Call ResolveInstallRecoveryPhysicalPath
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryPhysicalInstallPath "$InstallRecoveryPhysicalPath"
  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryCommonAppData"
  Call ResolveInstallRecoveryPhysicalPath
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  StrCpy $2 "$InstallRecoveryPhysicalInstallPath\"
  StrCpy $3 "$InstallRecoveryPhysicalPath\EqualizerAPO\InstallerRecovery\"
  StrLen $4 "$2"
  StrCpy $5 "$3" $4
  ${If} $5 == $2
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  StrLen $4 "$3"
  StrCpy $5 "$2" $4
  ${If} $5 == $3
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ; Once a transaction is journaled, the physical target must remain stable.
  ; A junction retarget between installer runs is rejected before any cleanup or
  ; rollback file operation.
  ClearErrors
  ReadRegStr $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PhysicalInstallPath"
  ${IfNot} ${Errors}
  ${AndIf} $0 != "$InstallRecoveryPhysicalInstallPath"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
FunctionEnd

Function SaveInstallMetadataJournal
  StrCpy $InstallRecoveryFailed "0"
  !insertmacro JournalPreviousString HKLM "${REGPATH}" "InstallPath" "PreviousInstallPath"
  !insertmacro JournalPreviousString HKLM "${REGPATH}" "ConfigPath" "PreviousConfigPath"
  !insertmacro JournalPreviousString HKLM "${REGPATH}" "EnableTrace" "PreviousEnableTrace"
  !insertmacro JournalPreviousString HKLM "${REGPATH}" "Start Menu Folder" "PreviousStartMenuFolder"
  !insertmacro JournalPreviousString HKLM "${UNINST_REGPATH}" "DisplayName" "PreviousDisplayName"
  !insertmacro JournalPreviousString HKLM "${UNINST_REGPATH}" "DisplayVersion" "PreviousDisplayVersion"
  !insertmacro JournalPreviousString HKLM "${UNINST_REGPATH}" "UninstallString" "PreviousUninstallString"
  !insertmacro JournalPreviousDWORD HKLM "${UNINST_REGPATH}" "NoModify" "PreviousNoModify"
  !insertmacro JournalPreviousDWORD HKLM "${UNINST_REGPATH}" "NoRepair" "PreviousNoRepair"
  !insertmacro JournalPreviousDWORD HKLM "${REGPATH}" "BrandMigrationVersion" "PreviousBrandMigrationVersion"
  !if ${ENABLE_ASIO_PROXY} == 1
    !insertmacro JournalPreviousString HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID" "PreviousAsioEnumClsid"
    !insertmacro JournalPreviousString HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description" "PreviousAsioEnumDescription"
    !insertmacro JournalPreviousString HKLM "${ASIO_PROXY_COM_REGPATH}" "" "PreviousAsioComDescription"
    !insertmacro JournalPreviousString HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "" "PreviousAsioInprocServer"
    !insertmacro JournalPreviousString HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel" "PreviousAsioThreadingModel"
    !insertmacro JournalPreviousString HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID" "PreviousAsioTargetClsid"
  !endif
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  ClearErrors
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "InstallPath" "$INSTDIR"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ${If} $InstallRecoveryPhysicalInstallPath == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PhysicalInstallPath" "$InstallRecoveryPhysicalInstallPath"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  Call PersistInstallRecoveryJournalVersion
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "RenameCleanupStarted" 0
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "OldStartMenuFolder" "$OldStartMenuFolder"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewStartMenuFolder" "$StartMenuFolder"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousApoPresent" $PreviousApoPresent
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewApoRegistrationAttempted" 0
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  !if ${ENABLE_ASIO_PROXY} == 1
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewAsioRegistrationAttempted" 0
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
  !endif
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "UpdaterOperationStarted" 0
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousUpdateTaskXmlSaved" 0
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ProductRootCreated" 0
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "DeviceSelectorOperationStarted" 0
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "DeviceSelectorMode" 0
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function RestoreInstallMetadata
  StrCpy $InstallRecoveryFailed "0"
  !insertmacro RestorePreviousString HKLM "${REGPATH}" "InstallPath" "PreviousInstallPath"
  !insertmacro RestorePreviousString HKLM "${REGPATH}" "ConfigPath" "PreviousConfigPath"
  !insertmacro RestorePreviousString HKLM "${REGPATH}" "EnableTrace" "PreviousEnableTrace"
  !insertmacro RestorePreviousString HKLM "${REGPATH}" "Start Menu Folder" "PreviousStartMenuFolder"
  !insertmacro RestorePreviousString HKLM "${UNINST_REGPATH}" "DisplayName" "PreviousDisplayName"
  !insertmacro RestorePreviousString HKLM "${UNINST_REGPATH}" "DisplayVersion" "PreviousDisplayVersion"
  !insertmacro RestorePreviousString HKLM "${UNINST_REGPATH}" "UninstallString" "PreviousUninstallString"
  !insertmacro RestorePreviousDWORD HKLM "${UNINST_REGPATH}" "NoModify" "PreviousNoModify"
  !insertmacro RestorePreviousDWORD HKLM "${UNINST_REGPATH}" "NoRepair" "PreviousNoRepair"
  !insertmacro RestorePreviousDWORD HKLM "${REGPATH}" "BrandMigrationVersion" "PreviousBrandMigrationVersion"
  DeleteRegKey /ifempty HKLM ${UNINST_REGPATH}
FunctionEnd

Function RestoreInstallShortcuts
  ClearErrors
  ReadRegStr $StartMenuFolder HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewStartMenuFolder"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ReadRegStr $OldStartMenuFolder HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "OldStartMenuFolder"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ClearErrors
  !insertmacro DeleteProductShortcuts $StartMenuFolder
  ${If} $InstallOperationCode != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousInstallPathExisted"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ${If} $0 == 1
  ${AndIf} $OldStartMenuFolder != ""
    ReadRegStr $OLDINSTDIR HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousInstallPathValue"
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
    ClearErrors
    CreateDirectory "$SMPROGRAMS\$OldStartMenuFolder"
    StrCpy $2 "0"
    ClearErrors
    ReadRegDWORD $3 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousBrandMigrationVersionExisted"
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${ElseIf} $3 == 1
      ClearErrors
      ReadRegDWORD $3 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousBrandMigrationVersionValue"
      ${If} ${Errors}
        StrCpy $InstallRecoveryFailed "1"
        Return
      ${ElseIf} $3 >= ${BRAND_MIGRATION_VERSION}
        StrCpy $2 "1"
      ${EndIf}
    ${ElseIf} $3 != 0
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
    ${If} $2 == "1"
      CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Hibiki EQAPO Configuration Editor.lnk" "$OLDINSTDIR\Editor.exe"
    ${Else}
      CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Equalizer APO Configuration Editor.lnk" "$OLDINSTDIR\Editor.exe"
    ${EndIf}
    CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Configuration tutorial (online).lnk" "$OLDINSTDIR\Configuration tutorial (online).url"
    CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Configuration reference (online).lnk" "$OLDINSTDIR\Configuration reference (online).url"
    ${If} $2 == "1"
      CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Hibiki EQAPO Device Selector.lnk" "$OLDINSTDIR\DeviceSelector.exe"
    ${Else}
      CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Equalizer APO Device Selector.lnk" "$OLDINSTDIR\DeviceSelector.exe"
    ${EndIf}
    CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Benchmark.lnk" "$OLDINSTDIR\Benchmark.exe"
    CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Check for updates.lnk" "$OLDINSTDIR\UpdateChecker.exe"
    CreateShortCut "$SMPROGRAMS\$OldStartMenuFolder\Uninstall.lnk" "$OLDINSTDIR\Uninstall.exe"
    ${If} ${Errors}
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
  ${ElseIf} $0 != 0
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function CloseRenameIdentityHandle
  ${If} $RenameIdentityHandle != ""
  ${AndIf} $RenameIdentityHandle != ${INVALID_HANDLE_VALUE}
    System::Call 'kernel32::CloseHandle(p $RenameIdentityHandle) i .r0'
  ${EndIf}
  StrCpy $RenameIdentityHandle ""
FunctionEnd

Function QueryRenameFileIdentityAndHold
  Call CloseRenameIdentityHandle
  StrCpy $RenameIdentityFailed "1"
  StrCpy $1 0
  System::Call 'kernel32::CreateFileW(w "$RenameIdentityPath", i ${FILE_READ_ATTRIBUTES}, i ${FILE_SHARE_READ_WRITE_DELETE}, p 0, i ${OPEN_EXISTING}, i ${FILE_FLAG_OPEN_REPARSE_POINT}|${FILE_FLAG_BACKUP_SEMANTICS}, p 0) p .r0 ?e'
  Pop $2
  ${If} $0 == ${INVALID_HANDLE_VALUE}
    Return
  ${EndIf}
  StrCpy $RenameIdentityHandle "$0"

  System::Call 'kernel32::GetFileType(p r0) i .r6'
  ${If} $6 != ${FILE_TYPE_DISK}
    Goto queryRenameIdentityFailed
  ${EndIf}
  System::Call '*(i,&v24,i,&v12,i,i) p .r1'
  ${If} $1 == 0
    Goto queryRenameIdentityFailed
  ${EndIf}
  System::Call 'kernel32::GetFileInformationByHandle(p r0, p r1) i .r6 ?e'
  Pop $7
  ${If} $6 == 0
    Goto queryRenameIdentityFailed
  ${EndIf}
  ; BY_HANDLE_FILE_INFORMATION: attributes, six FILETIME DWORDs, volume,
  ; size high/low, link count, then file-index high/low.
  System::Call '*$1(i .r2, &v24, i .r3, &v12, i .r4, i .r5)'
  IntOp $6 $2 & ${FILE_ATTRIBUTE_DIRECTORY}
  ${If} $6 != 0
    Goto queryRenameIdentityFailed
  ${EndIf}
  IntOp $6 $2 & ${FILE_ATTRIBUTE_REPARSE_POINT}
  ${If} $6 != 0
    Goto queryRenameIdentityFailed
  ${EndIf}
  ${If} $4 == 0
  ${AndIf} $5 == 0
    Goto queryRenameIdentityFailed
  ${EndIf}

  StrCpy $RenameIdentityVolumeSerial "$3"
  StrCpy $RenameIdentityFileIndexHigh "$4"
  StrCpy $RenameIdentityFileIndexLow "$5"
  System::Free $1
  StrCpy $RenameIdentityFailed "0"
  Return

  queryRenameIdentityFailed:
  ${If} $1 != 0
    System::Free $1
  ${EndIf}
  Call CloseRenameIdentityHandle
FunctionEnd

Function DeleteRenameFileByIdentity
  ; Open the candidate without following a reparse point, compare its stable file
  ; identity, and mark that same handle for deletion. No path-based TOCTOU window
  ; remains between the identity check and deletion.
  Call CloseRenameIdentityHandle
  StrCpy $RenameIdentityFailed "1"
  StrCpy $1 0
  StrCpy $6 0
  System::Call 'kernel32::CreateFileW(w "$RenameIdentityPath", i ${DELETE_ACCESS}|${FILE_READ_ATTRIBUTES}, i ${FILE_SHARE_READ_WRITE}, p 0, i ${OPEN_EXISTING}, i ${FILE_FLAG_OPEN_REPARSE_POINT}|${FILE_FLAG_BACKUP_SEMANTICS}, p 0) p .r0 ?e'
  Pop $2
  ${If} $0 == ${INVALID_HANDLE_VALUE}
    ${If} $2 == ${ERROR_FILE_NOT_FOUND}
    ${OrIf} $2 == ${ERROR_PATH_NOT_FOUND}
      StrCpy $RenameIdentityFailed "0"
    ${EndIf}
    Return
  ${EndIf}
  StrCpy $RenameIdentityHandle "$0"

  System::Call 'kernel32::GetFileType(p r0) i .r7'
  ${If} $7 != ${FILE_TYPE_DISK}
    Goto deleteRenameIdentityFailed
  ${EndIf}
  System::Call '*(i,&v24,i,&v12,i,i) p .r1'
  ${If} $1 == 0
    Goto deleteRenameIdentityFailed
  ${EndIf}
  System::Call 'kernel32::GetFileInformationByHandle(p r0, p r1) i .r7 ?e'
  Pop $8
  ${If} $7 == 0
    Goto deleteRenameIdentityFailed
  ${EndIf}
  System::Call '*$1(i .r2, &v24, i .r3, &v12, i .r4, i .r5)'
  IntOp $7 $2 & ${FILE_ATTRIBUTE_DIRECTORY}
  ${If} $7 != 0
    Goto deleteRenameIdentityFailed
  ${EndIf}
  IntOp $7 $2 & ${FILE_ATTRIBUTE_REPARSE_POINT}
  ${If} $7 != 0
    Goto deleteRenameIdentityFailed
  ${EndIf}
  ${If} $4 == 0
  ${AndIf} $5 == 0
    Goto deleteRenameIdentityFailed
  ${EndIf}
  ${If} $3 != "$RenameExpectedVolumeSerial"
  ${OrIf} $4 != "$RenameExpectedFileIndexHigh"
  ${OrIf} $5 != "$RenameExpectedFileIndexLow"
    DetailPrint "A renamed-file path now refers to a different file identity and was retained: $RenameIdentityPath"
    Goto deleteRenameIdentityFailed
  ${EndIf}

  System::Call '*(&i1 1) p .r6'
  ${If} $6 == 0
    Goto deleteRenameIdentityFailed
  ${EndIf}
  System::Call 'kernel32::SetFileInformationByHandle(p r0, i ${FILE_DISPOSITION_INFO_CLASS}, p r6, i 1) i .r7 ?e'
  Pop $8
  ${If} $7 == 0
    Goto deleteRenameIdentityFailed
  ${EndIf}

  System::Free $6
  System::Free $1
  Call CloseRenameIdentityHandle
  StrCpy $RenameIdentityFailed "0"
  Return

  deleteRenameIdentityFailed:
  ${If} $6 != 0
    System::Free $6
  ${EndIf}
  ${If} $1 != 0
    System::Free $1
  ${EndIf}
  Call CloseRenameIdentityHandle
FunctionEnd

Function FlushRenameCleanupJournal
  StrCpy $InstallRecoveryFailed "0"
  !if ${LIBPATH} != "lib32"
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${INSTALLER_APP_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  !else
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${INSTALLER_APP_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE}, *p .r0) i .r1'
  !endif
  ${If} $1 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call 'advapi32::RegFlushKey(p r0) i .r1'
  System::Call 'advapi32::RegCloseKey(p r0) i .r2'
  ${If} $1 != 0
  ${OrIf} $2 != 0
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function PersistInstallRecoveryJournalVersion
  StrCpy $InstallRecoveryFailed "0"
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "JournalVersion" ${INSTALL_RECOVERY_JOURNAL_VERSION}
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "JournalVersion"
  ${If} ${Errors}
  ${OrIf} $0 != ${INSTALL_RECOVERY_JOURNAL_VERSION}
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function RetireLegacyRenameManifest
  ; v3.0.2 overwrote the start of this manifest on every append. Its remaining
  ; path text cannot prove ownership, so never infer or delete any .old file from
  ; it. Retire only the exact protected manifest and preserve all application
  ; files; committed/rollback-cleanup are already irreversible decisions.
  StrCpy $InstallRecoveryFailed "0"
  ClearErrors
  ReadRegStr $InstallRecoveryPhase HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${ElseIf} $InstallRecoveryPhase != "committed"
  ${AndIf} $InstallRecoveryPhase != "rollback-cleanup"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  Call SecureExistingInstallRecoveryTree
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ProductRootCreated"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  StrCpy $LegacyRenameRepairPath "$InstallRollbackDirectory\renamed-files.v2"
  !insertmacro RetireLegacyRenameRecoveryArtifact "$LegacyRenameRepairPath"
  !insertmacro RetireLegacyRenameRecoveryArtifact "$RenameManifestPath"
  Call PersistInstallRecoveryJournalVersion
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "RenameCleanupStarted" 1
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "RenameCleanupStarted"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  DetailPrint "Retired the corrupted v3.0.2 rename manifest; unprovable .old files were retained."
  Return

  retireLegacyRenameManifestFailed:
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function PrepareRenameManifestForCleanup
  StrCpy $InstallRecoveryFailed "0"
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "JournalVersion"
  ${If} ${Errors}
    ; v3.0.2 did not write a journal version and used the broken append logic.
    Call RetireLegacyRenameManifest
    Return
  ${EndIf}
  ${If} $0 != ${INSTALL_RECOVERY_JOURNAL_VERSION}
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function ClearInstallRecoveryJournal
  ; This is always the final cleanup step. Snapshot and rename-manifest cleanup
  ; must finish first so a crash never leaves filesystem state without a journal.
  StrCpy $InstallRecoveryFailed "0"
  DeleteRegKey HKLM ${INSTALLER_APP_RECOVERY_REGPATH}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Pending"
  ${IfNot} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  DeleteRegKey /ifempty HKLM ${REGPATH}
FunctionEnd

Function DiscardInstallRecoverySnapshot
  Call CloseRenameManifest
  ; This is the only recursive cleanup target. It is reconstructed from a fixed
  ; Common AppData path, secured against non-admin replacement, and revalidated
  ; immediately before RMDir /r. Never call this for an unjournaled orphan.
  Call InitializeInstallRecoveryPaths
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  Call SecureExistingInstallRecoveryTree
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
  ${OrIf} $InstallRecoveryPathExists == "0"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackDirectory"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ${If} $InstallRecoveryPathExists == "0"
    ; Recursive child cleanup already finished. Because ProductRootCreated=1 was
    ; verified by SecureExistingInstallRecoveryTree, the empty secured parent is
    ; transaction-owned and may be removed non-recursively.
    StrCpy $InstallRecoveryAclTarget "$InstallRecoveryProductRoot"
    Call HardenInstallRecoveryDirectory
    ${If} $InstallRecoveryFailed == "1"
      Return
    ${EndIf}
    RMDir "$InstallRecoveryProductRoot"
    StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
    StrCpy $InstallRecoveryPathRequired "0"
    Call ValidateInstallRecoveryComponent
    ${If} $InstallRecoveryFailed == "1"
    ${OrIf} $InstallRecoveryPathExists == "1"
      StrCpy $InstallRecoveryFailed "1"
    ${EndIf}
    Return
  ${EndIf}
  Call ValidateInstallRecoveryComponents
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  RMDir /r "$InstallRollbackDirectory"
  StrCpy $InstallRecoveryPathToCheck "$InstallRollbackDirectory"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
  ${OrIf} $InstallRecoveryPathExists == "1"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  RMDir "$InstallRecoveryProductRoot"
  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
  ${OrIf} $InstallRecoveryPathExists == "1"
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function LoadInstallRecoveryTargetFromJournal
  StrCpy $InstallRecoveryFailed "0"
  ClearErrors
  ReadRegStr $INSTDIR HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "InstallPath"
  ${If} ${Errors}
  ${OrIf} $INSTDIR == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  Call ValidateInstallRecoveryTarget
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ; New journals always persist this before Pending. Recovery must not silently
  ; downgrade to a lexical-only legacy target when the value is missing.
  ClearErrors
  ReadRegStr $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PhysicalInstallPath"
  ${If} ${Errors}
  ${OrIf} $0 == ""
  ${OrIf} $0 != "$InstallRecoveryPhysicalInstallPath"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ; Recovery may execute or load the journaled installed payload. Apply the same
  ; fail-closed local-volume/ancestor/ACL gate before any rollback action.
  StrCpy $InstallRootAllowMissing "0"
  Call ValidateInstallRootAcl
  ${If} $InstallOperationCode == "error"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${ElseIf} $InstallOperationCode != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
FunctionEnd

Function CloseConfigRootIdentityHandle
  ${If} $ConfigRootIdentityHandle != ""
  ${AndIf} $ConfigRootIdentityHandle != ${INVALID_HANDLE_VALUE}
    System::Call 'kernel32::CloseHandle(p $ConfigRootIdentityHandle) i .r0'
  ${EndIf}
  StrCpy $ConfigRootIdentityHandle ""
FunctionEnd

Function ValidateAndHoldConfigRoot
  ; Hold a no-delete-share handle from validation through every direct config
  ; write. FILE_FLAG_OPEN_REPARSE_POINT makes a root junction/symlink visible to
  ; this check instead of silently opening its target. Descendants are never
  ; enumerated: they are user data and may legitimately contain reparse points.
  Call CloseConfigRootIdentityHandle
  StrCpy $InstallRecoveryFailed "1"
  StrCpy $1 0
  System::Call 'kernel32::CreateFileW(w "$INSTDIR\config", i ${FILE_READ_ATTRIBUTES}, i ${FILE_SHARE_READ_WRITE}, p 0, i ${OPEN_EXISTING}, i ${FILE_FLAG_OPEN_REPARSE_POINT}|${FILE_FLAG_BACKUP_SEMANTICS}, p 0) p .r0 ?e'
  Pop $2
  ${If} $0 == ${INVALID_HANDLE_VALUE}
    DetailPrint "Could not securely open the config root (Win32 error $2)."
    Return
  ${EndIf}
  StrCpy $ConfigRootIdentityHandle "$0"

  System::Call 'kernel32::GetFileType(p r0) i .r3'
  ${If} $3 != ${FILE_TYPE_DISK}
    DetailPrint "The config root is not on a disk filesystem."
    Goto invalidConfigRoot
  ${EndIf}
  System::Call '*(i,&v24,i,&v12,i,i) p .r1'
  ${If} $1 == 0
    DetailPrint "Could not allocate config-root validation state."
    Goto invalidConfigRoot
  ${EndIf}
  System::Call 'kernel32::GetFileInformationByHandle(p r0, p r1) i .r3 ?e'
  Pop $2
  ${If} $3 == 0
    DetailPrint "Could not inspect the opened config root (Win32 error $2)."
    System::Free $1
    Goto invalidConfigRoot
  ${EndIf}
  System::Call '*$1(i .r2)'
  System::Free $1
  IntOp $3 $2 & ${FILE_ATTRIBUTE_DIRECTORY}
  ${If} $3 == 0
    DetailPrint "The config root is not a directory."
    Goto invalidConfigRoot
  ${EndIf}
  IntOp $3 $2 & ${FILE_ATTRIBUTE_REPARSE_POINT}
  ${If} $3 != 0
    DetailPrint "Refusing a config root which is a reparse point."
    Goto invalidConfigRoot
  ${EndIf}

  StrCpy $InstallRecoveryFailed "0"
  Return

  invalidConfigRoot:
  Call CloseConfigRootIdentityHandle
FunctionEnd

Function CleanupCompletedInstallTransaction
  ; committed and rollback-cleanup are decision phases: recovery may only finish
  ; deleting transaction-owned .old entries and the secure snapshot, then clear
  ; the journal. It must never replay installation or rollback mutations.
  Call LoadInstallRecoveryTargetFromJournal
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  Call PrepareRenameManifestForCleanup
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  Call DiscardRenamedProductFiles
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  Call DiscardInstallRecoverySnapshot
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  Call ClearInstallRecoveryJournal
FunctionEnd

Function RecoverInstallTransaction
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $InstallRollbackState "0"
  StrCpy $RenameManifestHandle ""
  Call InitializeInstallRecoveryPaths
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  Call ValidateInstallRecoveryComponents
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Pending"
  ${If} ${Errors}
    ; A fixed name is not proof of ownership. Never recursively delete an
    ; unjournaled directory; preserve it for explicit administrator inspection.
    StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
    StrCpy $InstallRecoveryPathRequired "0"
    Call ValidateInstallRecoveryComponent
    ${If} $InstallRecoveryFailed == "1"
      Return
    ${ElseIf} $InstallRecoveryPathExists == "1"
      DetailPrint "An unjournaled installer recovery tree was found and retained: $InstallRecoveryProductRoot"
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
    ; A partial registry journal created before Pending is safe to discard only
    ; when no filesystem recovery tree exists.
    Call ClearInstallRecoveryJournal
    Return
  ${EndIf}
  ${If} $0 != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegStr $InstallRecoveryPhase HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ${If} $InstallRecoveryPhase == "initializing"
  ${OrIf} $InstallRecoveryPhase == "preparing"
  ${OrIf} $InstallRecoveryPhase == "prepared"
    ; No product mutation is allowed until the active phase is durably written.
    Call DiscardInstallRecoverySnapshot
    ${If} $InstallRecoveryFailed == "1"
      Return
    ${EndIf}
    Call ClearInstallRecoveryJournal
    Return
  ${ElseIf} $InstallRecoveryPhase == "committed"
    Call CleanupCompletedInstallTransaction
    Return
  ${ElseIf} $InstallRecoveryPhase == "rollback-cleanup"
    Call CleanupCompletedInstallTransaction
    Return
  ${ElseIf} $InstallRecoveryPhase != "active"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  Call ValidateActiveInstallRecoverySnapshot
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  Call LoadInstallRecoveryTargetFromJournal
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ReadRegDWORD $PreviousApoPresent HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousApoPresent"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ReadRegDWORD $NewApoRegistrationAttempted HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewApoRegistrationAttempted"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  !if ${ENABLE_ASIO_PROXY} == 1
    ClearErrors
    ReadRegDWORD $NewAsioRegAttempted HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewAsioRegistrationAttempted"
    ${If} ${Errors}
      ; Journal v2 predates the optional ASIO extension. Absence is legacy only
      ; when none of the ASIO snapshot fields exists; a partial new journal is
      ; malformed and must be retained.
      ClearErrors
      ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousAsioEnumClsidExisted"
      ${If} ${Errors}
        StrCpy $NewAsioRegAttempted "0"
      ${Else}
        StrCpy $InstallRecoveryFailed "1"
        Return
      ${EndIf}
    ${EndIf}
  !endif
  ${If} $PreviousApoPresent != 0
  ${AndIf} $PreviousApoPresent != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ${If} $NewApoRegistrationAttempted != 0
  ${AndIf} $NewApoRegistrationAttempted != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  StrCpy $InstallRollbackState "1"
  Call RollbackInstallTransaction
  ${If} $InstallRollbackState != "0"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  DetailPrint "Recovered the application tree from an interrupted installation."
FunctionEnd

Function RemoveInstalledProductFiles
  ; Current payload. Keep config and VSTPlugins intact even on a fresh-install
  ; failure: an existing folder may already contain user-created data.
  !insertmacro DeleteTransactionFile "$INSTDIR\EqualizerAPO.dll"
  !if ${ENABLE_ASIO_PROXY} == 1
    !insertmacro DeleteTransactionFile "$INSTDIR\${ASIO_PROXY_DLL}"
  !endif
  !insertmacro DeleteTransactionFile "$INSTDIR\EqApoOutProcHost.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\DeviceSelector.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\Benchmark.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\VoicemeeterClient.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\UpdateChecker.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\Editor.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\Uninstall.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\NOTICE.md"
  !insertmacro DeleteTransactionFile "$INSTDIR\LICENSE.txt"
  !insertmacro DeleteTransactionFile "$INSTDIR\install-diagnostics.log"
  !insertmacro DeleteTransactionFile "$INSTDIR\libfftw3.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\fftw3.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\sndfile.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\FLAC.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\libmp3lame.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\mpg123.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\ogg.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\opus.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\vorbis.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\vorbisenc.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\vorbisfile.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\msvcp140.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\msvcp140_1.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\vcruntime140.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\vcruntime140_1.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\d3dcompiler_47.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\dxcompiler.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\dxil.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\icuuc.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\Qt6Core.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\Qt6Gui.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\Qt6Network.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\Qt6Svg.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\Qt6Widgets.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\Configuration tutorial (online).url"
  !insertmacro DeleteTransactionFile "$INSTDIR\Configuration reference (online).url"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt.conf"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\generic\qtuiotouchplugin.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\iconengines\qsvgicon.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\imageformats\qico.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\imageformats\qsvg.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\networkinformation\qnetworklistmanager.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\platforms\qwindows.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\styles\qmodernwindowsstyle.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\tls\qcertonlybackend.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\tls\qschannelbackend.dll"

  ; Files retired by this release may have been removed or renamed before the
  ; failure. The snapshot restores them for an upgrade.
  !insertmacro DeleteTransactionFile "$INSTDIR\Configurator.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\EqApoJuceVST3Host.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\EqApoOutProcJuceHost.exe"
  !insertmacro DeleteTransactionFile "$INSTDIR\Qt5Core.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\Qt5Gui.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\Qt5Widgets.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\libfftw3-3.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\libsndfile-1.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\msvcp100.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\msvcr100.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\msvcp120.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\msvcr120.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\msvcp140_2.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\icudt.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\icuin.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\icudt78.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\icuin78.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\icuuc78.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\imageformats\qgif.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\imageformats\qjpeg.dll"
  !insertmacro DeleteTransactionFile "$INSTDIR\qt\styles\qwindowsvistastyle.dll"
  ; Remove only empty product directories. Never recursively delete a directory
  ; which may contain an unrelated file or a reparse point.
  RMDir "$INSTDIR\qt\generic"
  RMDir "$INSTDIR\qt\iconengines"
  RMDir "$INSTDIR\qt\imageformats"
  RMDir "$INSTDIR\qt\networkinformation"
  RMDir "$INSTDIR\qt\platforms"
  RMDir "$INSTDIR\qt\styles"
  RMDir "$INSTDIR\qt\tls"
  RMDir "$INSTDIR\qt"
FunctionEnd

Function CloseRenameManifest
  ${If} $RenameManifestHandle != ""
    FileClose $RenameManifestHandle
    StrCpy $RenameManifestHandle ""
  ${EndIf}
FunctionEnd

Function DiscardRenamedProductFiles
  Call CloseRenameManifest
  StrCpy $InstallRecoveryFailed "0"

  ; Delete only exact paths recorded after successful Rename calls. Cleanup gets
  ; one durable attempt: after any crash/failure, the manifest is retired without
  ; another product-file deletion so a replacement path can never be adopted.
  System::Call 'kernel32::GetFileAttributesW(w "$RenameManifestPath") i .r0 ?e'
  Pop $1
  ${If} $0 == ${INVALID_FILE_ATTRIBUTES}
    ${If} $1 == ${ERROR_FILE_NOT_FOUND}
    ${OrIf} $1 == ${ERROR_PATH_NOT_FOUND}
      Return
    ${EndIf}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $1 $0 & ${FILE_ATTRIBUTE_REPARSE_POINT}
  ${If} $1 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $1 $0 & ${FILE_ATTRIBUTE_DIRECTORY}
  ${If} $1 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $2 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "RenameCleanupStarted"
  ${If} ${Errors}
  ${OrIf} $2 != 0
    DetailPrint "Renamed-file cleanup was already attempted or its state is unavailable; retaining application files."
    Goto retireRenameManifest
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "RenameCleanupStarted" 1
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  ReadRegDWORD $2 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "RenameCleanupStarted"
  ${If} ${Errors}
  ${OrIf} $2 != 1
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  Call FlushRenameCleanupJournal
  ${If} $InstallRecoveryFailed == "1"
    DetailPrint "Renamed-file cleanup was not started because its durable marker could not be flushed."
    Return
  ${EndIf}

  ClearErrors
  GetFullPathName $5 "$INSTDIR"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ; Keep both values outside the general-purpose registers: the identity-delete
  ; helper uses $0-$8 internally and must not invalidate the open manifest or
  ; the containment prefix between records.
  StrCpy $RenameCleanupInstallPrefix "$5\"
  ClearErrors
  FileOpen $RenameManifestHandle "$RenameManifestPath" r
  ${If} ${Errors}
    StrCpy $RenameManifestHandle ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  readRenamedProductFile:
  ClearErrors
  FileReadUTF16LE $RenameManifestHandle $1
  ${If} ${Errors}
    Goto closeAndRetireRenameManifest
  ${EndIf}
  ${StrTrimNewLines} $1 "$1"
  ${If} $1 != ""
    ; Confirmation records include the source file identity and are framed at
    ; both ends. A partial append after power loss is never interpreted as a path.
    ${StrTok} $3 "$1" "|" "0" "0"
    ${StrTok} $RenameExpectedVolumeSerial "$1" "|" "1" "0"
    ${StrTok} $RenameExpectedFileIndexHigh "$1" "|" "2" "0"
    ${StrTok} $RenameExpectedFileIndexLow "$1" "|" "3" "0"
    ${StrTok} $RenameIdentityPath "$1" "|" "4" "0"
    ${StrTok} $7 "$1" "|" "5" "0"
    ${StrTok} $8 "$1" "|" "6" "0"
    ${If} $3 != "C"
    ${OrIf} $7 != "C"
    ${OrIf} $8 != ""
    ${OrIf} $RenameExpectedVolumeSerial == ""
    ${OrIf} $RenameExpectedFileIndexHigh == ""
    ${OrIf} $RenameExpectedFileIndexLow == ""
    ${OrIf} $RenameIdentityPath == ""
      DetailPrint "Retaining renamed files because the confirmation manifest is malformed."
      Goto closeAndRetireRenameManifest
    ${EndIf}

    ; Canonicalize the parent separately so a previously removed exact file is
    ; harmless. Lexical tricks such as .. cannot escape the validated tree.
    ${GetParent} "$RenameIdentityPath" $4
    ${GetFileName} "$RenameIdentityPath" $9
    ${If} $4 == ""
    ${OrIf} $9 == ""
      DetailPrint "Retaining renamed files because a confirmation path is incomplete."
      Goto closeAndRetireRenameManifest
    ${EndIf}
    ClearErrors
    GetFullPathName $4 "$4"
    ${If} ${Errors}
      DetailPrint "Retaining renamed files because a confirmation path could not be canonicalized."
      Goto closeAndRetireRenameManifest
    ${Else}
      StrCpy $4 "$4\$9"
      StrCpy $RenameIdentityPath "$4"
      StrLen $7 "$RenameCleanupInstallPrefix"
      StrCpy $8 "$4" $7
      ${If} $8 == "$RenameCleanupInstallPrefix"
        Call DeleteRenameFileByIdentity
        ${If} $RenameIdentityFailed == "1"
          DetailPrint "A confirmed renamed file could not be safely deleted and was retained: $RenameIdentityPath"
          Goto closeAndRetireRenameManifest
        ${EndIf}
      ${Else}
        DetailPrint "Refused an out-of-scope rename-manifest entry: $1"
        Goto closeAndRetireRenameManifest
      ${EndIf}
    ${EndIf}
  ${Else}
    DetailPrint "Retaining renamed files because the confirmation manifest contains a blank record."
    Goto closeAndRetireRenameManifest
  ${EndIf}
  Goto readRenamedProductFile

  closeAndRetireRenameManifest:
  Call CloseRenameManifest

  retireRenameManifest:
  ClearErrors
  Delete "$RenameManifestPath"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function PrepareInstallTransaction
  StrCpy $InstallRollbackState "0"
  StrCpy $PreviousApoPresent "0"
  StrCpy $VerifiedPreviousInstall "0"
  StrCpy $NewApoRegistrationAttempted "0"
  !if ${ENABLE_ASIO_PROXY} == 1
    StrCpy $NewAsioRegAttempted "0"
  !endif
  StrCpy $RenameManifestHandle ""
  StrCpy $InstallRecoveryFailed "0"
  Call InitializeInstallRecoveryPaths
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRollbackCopyCode "Common AppData recovery path resolution failed"
    Goto installBackupFailed
  ${EndIf}
  Call ValidateInstallRecoveryTarget
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRollbackCopyCode "unsafe installation path"
    Goto installBackupFailed
  ${EndIf}
  !if ${ENABLE_ASIO_PROXY} == 1
    ${If} $NewAsioRegAttempted != 0
    ${AndIf} $NewAsioRegAttempted != 1
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
  !endif
  ; The elevated installer will later execute and load files from this directory.
  ; Refuse a root or ancestor which a lower-privilege principal can replace.
  StrCpy $InstallRootAllowMissing "0"
  Call ValidateInstallRootAcl
  ${If} $InstallOperationCode == "error"
    StrCpy $InstallRollbackCopyCode "install-root ACL validation could not start"
    Goto installBackupFailed
  ${ElseIf} $InstallOperationCode != 0
    StrCpy $InstallRollbackCopyCode "unsafe install-root ACL or ancestor (exit code: $InstallOperationCode)"
    Goto installBackupFailed
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\EqualizerAPO.dll"
    StrCpy $PreviousApoPresent "1"
  ${EndIf}

  ; A directory containing a coincidentally named DLL is not enough authority to
  ; terminate applications. Bind process shutdown to the product-specific HKLM
  ; installation record and the canonical destination before journaling or
  ; changing any persistent state. A manually copied/stale tree is handled by
  ; the later transactional file checks without killing same-name applications.
  ${If} $PreviousApoPresent == "1"
    ClearErrors
    ReadRegStr $0 HKLM ${REGPATH} "InstallPath"
    ${IfNot} ${Errors}
    ${AndIf} $0 != ""
      ClearErrors
      GetFullPathName $0 "$0"
      ${IfNot} ${Errors}
        normalizeRegisteredInstallPathTail:
        StrCpy $6 "$0" 1 -1
        ${If} $6 == "\"
          ${GetRoot} "$0" $7
          ${If} $0 != $7
          ${AndIf} $0 != "$7\"
            StrCpy $0 "$0" -1
            Goto normalizeRegisteredInstallPathTail
          ${EndIf}
        ${EndIf}
        ${If} $0 == $INSTDIR
          StrCpy $VerifiedPreviousInstall "1"
        ${EndIf}
      ${EndIf}
    ${EndIf}
  ${EndIf}
  ${If} $VerifiedPreviousInstall == "1"
    Call CloseRunningApplications
  ${EndIf}
  !if ${ENABLE_ASIO_PROXY} == 1
    ; Never overwrite a foreign registration at the fixed product identity, and
    ; never kill a DAW which may have unsaved work merely to unlock the DLL.
    Call ValidateExistingAsioProxyRegistration
    ${If} $InstallRecoveryFailed == "1"
      StrCpy $InstallRollbackCopyCode "$InstallFailureReason"
      Goto installBackupFailed
    ${EndIf}
    Call CheckAsioProxyDllUnlocked
    ${If} $InstallOperationCode != 0
      StrCpy $InstallRollbackCopyCode "Hibiki EQAPO driver is in use; close every DAW and retry (Win32 error: $InstallOperationCode)"
      Goto installBackupFailed
    ${EndIf}
  !endif

  ; Never adopt or delete an unjournaled directory at the fixed recovery name.
  Call ValidateInstallRecoveryComponents
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRollbackCopyCode "unsafe Common AppData recovery component"
    Goto installBackupFailed
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$InstallRecoveryProductRoot"
  StrCpy $InstallRecoveryPathRequired "0"
  Call ValidateInstallRecoveryComponent
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRollbackCopyCode "recovery product-root validation failed"
    Goto installBackupFailed
  ${ElseIf} $InstallRecoveryPathExists == "1"
    StrCpy $InstallRollbackCopyCode "an unjournaled recovery product root already exists"
    Goto installBackupFailed
  ${EndIf}

  ; Persist all metadata and Pending before creating filesystem artifacts. This
  ; makes even a power loss during directory creation recoverable next run.
  Call SaveInstallMetadataJournal
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRollbackCopyCode "registry-state journal creation failed"
    Goto installBackupFailed
  ${EndIf}

  ClearErrors
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase" "initializing"
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "initial recovery phase journal creation failed"
    Goto installBackupFailed
  ${EndIf}
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Pending" 1
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "recovery pending marker creation failed"
    Goto installBackupFailed
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Pending"
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "recovery pending marker verification failed"
    Goto installBackupFailed
  ${ElseIf} $0 != 1
    StrCpy $InstallRollbackCopyCode "recovery pending marker did not verify"
    Goto installBackupFailed
  ${EndIf}

  Call CreateSecureInstallRecoveryTree
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRollbackCopyCode "secure recovery directory creation failed"
    Goto installBackupFailed
  ${EndIf}
  ClearErrors
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase" "preparing"
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "preparing recovery phase could not be written"
    Goto installBackupFailed
  ${EndIf}

  ; Export the exact task definition before UpdateChecker can mutate it. Presence
  ; alone is insufficient because trigger, principal, arguments and settings are
  ; all part of rollback state.
  nsExec::ExecToLog '"$SYSDIR\schtasks.exe" /Query /TN "EqualizerAPOUpdateChecker" /FO LIST'
  Pop $InstallOperationCode
  ${If} $InstallOperationCode == "error"
    StrCpy $InstallRollbackCopyCode "scheduled-task state query could not start"
    Goto installBackupFailed
  ${ElseIf} $InstallOperationCode == 0
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousUpdateTaskPresent" 1
    ${If} ${Errors}
      StrCpy $InstallRollbackCopyCode "scheduled-task presence journal creation failed"
      Goto installBackupFailed
    ${EndIf}
    nsExec::ExecToLog '"$SYSDIR\cmd.exe" /D /Q /S /C "$\"$SYSDIR\schtasks.exe$\" /Query /TN $\"EqualizerAPOUpdateChecker$\" /XML > $\"$InstallRecoveryTaskXmlPath$\""'
    Pop $InstallOperationCode
    ${If} $InstallOperationCode == "error"
      StrCpy $InstallRollbackCopyCode "scheduled-task XML export could not start"
      Goto installBackupFailed
    ${ElseIf} $InstallOperationCode != 0
      StrCpy $InstallRollbackCopyCode "scheduled-task XML export failed with exit code $InstallOperationCode"
      Goto installBackupFailed
    ${EndIf}
    System::Call 'kernel32::GetFileAttributesW(w "$InstallRecoveryTaskXmlPath") i .r0 ?e'
    Pop $2
    ${If} $0 == ${INVALID_FILE_ATTRIBUTES}
      StrCpy $InstallRollbackCopyCode "scheduled-task XML export is missing"
      Goto installBackupFailed
    ${EndIf}
    IntOp $1 $0 & ${FILE_ATTRIBUTE_REPARSE_POINT}
    ${If} $1 != 0
      StrCpy $InstallRollbackCopyCode "scheduled-task XML export is a reparse point"
      Goto installBackupFailed
    ${EndIf}
    ClearErrors
    FileOpen $0 "$InstallRecoveryTaskXmlPath" r
    ${If} ${Errors}
      StrCpy $InstallRollbackCopyCode "scheduled-task XML export could not be opened"
      Goto installBackupFailed
    ${EndIf}
    FileRead $0 $1
    FileClose $0
    ${StrTrimNewLines} $1 "$1"
    ${If} $1 == ""
      StrCpy $InstallRollbackCopyCode "scheduled-task XML export is empty"
      Goto installBackupFailed
    ${EndIf}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousUpdateTaskXmlSaved" 1
  ${ElseIf} $InstallOperationCode == 1
    ; schtasks also uses exit 1 for failures other than "not found". Accept an
    ; absent task only when its canonical Task Scheduler backing file is absent
    ; with a precise file/path-not-found Win32 result.
    System::Call 'kernel32::GetFileAttributesW(w "$WINDIR\System32\Tasks\EqualizerAPOUpdateChecker") i .r0 ?e'
    Pop $2
    ${If} $0 != ${INVALID_FILE_ATTRIBUTES}
      StrCpy $InstallRollbackCopyCode "scheduled-task query returned exit 1 but its backing file still exists"
      Goto installBackupFailed
    ${ElseIf} $2 != ${ERROR_FILE_NOT_FOUND}
    ${AndIf} $2 != ${ERROR_PATH_NOT_FOUND}
      StrCpy $InstallRollbackCopyCode "scheduled-task query returned exit 1 with Win32 error $2"
      Goto installBackupFailed
    ${EndIf}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousUpdateTaskPresent" 0
  ${Else}
    StrCpy $InstallRollbackCopyCode "scheduled-task state query failed with exit code $InstallOperationCode"
    Goto installBackupFailed
  ${EndIf}
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "scheduled-task state journal creation failed"
    Goto installBackupFailed
  ${EndIf}

  ; Preserve only the config root ACL. /L prevents a root reparse point from
  ; redirecting the read, and omitting /T keeps descendant junctions completely
  ; outside this elevated transaction.
  ${If} ${FileExists} "$INSTDIR\config"
    nsExec::ExecToLog '"$SYSDIR\icacls.exe" "$INSTDIR\config" /save "$InstallRecoveryAclPath" /L /Q'
    Pop $InstallOperationCode
    ${If} $InstallOperationCode == "error"
      StrCpy $InstallRollbackCopyCode "config ACL snapshot could not start"
      Goto installBackupFailed
    ${ElseIf} $InstallOperationCode != 0
      StrCpy $InstallRollbackCopyCode "config ACL snapshot failed with exit code $InstallOperationCode"
      Goto installBackupFailed
    ${EndIf}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ConfigAclSaved" 1
  ${Else}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ConfigAclSaved" 0
  ${EndIf}
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "config ACL journal creation failed"
    Goto installBackupFailed
  ${EndIf}

  ; Snapshot the old application tree outside $INSTDIR before changing files.
  ; config and VSTPlugins are excluded because they are user-data containers and
  ; are never deleted or overwritten by rollback.
  Call ValidateInstallRecoveryComponents
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRollbackCopyCode "recovery tree changed before application snapshot"
    Goto installBackupFailed
  ${EndIf}
  nsExec::ExecToLog '"$SYSDIR\robocopy.exe" "$INSTDIR" "$InstallRollbackFiles" /E /COPY:DATS /DCOPY:DAT /R:1 /W:1 /XJ /XD "$INSTDIR\config" "$INSTDIR\VSTPlugins" /NFL /NDL /NJH /NJS /NP'
  Pop $InstallRollbackCopyCode
  ${If} $InstallRollbackCopyCode == "error"
    Goto installBackupFailed
  ${ElseIf} $InstallRollbackCopyCode >= 8
    Goto installBackupFailed
  ${EndIf}
  Call ValidateInstallRecoveryComponents
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallRollbackCopyCode "recovery tree changed during application snapshot"
    Goto installBackupFailed
  ${EndIf}

  ClearErrors
  FileOpen $RenameManifestHandle "$RenameManifestPath" w
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "rename manifest creation failed"
    Goto installBackupFailed
  ${EndIf}
  FileClose $RenameManifestHandle
  StrCpy $RenameManifestHandle ""

  ClearErrors
  FileOpen $0 "$InstallRecoveryMarkerPath" w
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "recovery marker creation failed"
    Goto installBackupFailed
  ${EndIf}
  FileWrite $0 "EqualizerAPO installer app-tree recovery v1$\r$\n"
  ${If} ${Errors}
    FileClose $0
    StrCpy $InstallRollbackCopyCode "recovery marker write failed"
    Goto installBackupFailed
  ${EndIf}
  FileClose $0

  ClearErrors
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase" "prepared"
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "prepared recovery phase could not be written"
    Goto installBackupFailed
  ${EndIf}
  ; No application file may be changed until active is written and read back.
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase" "active"
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "active recovery phase could not be written"
    Goto installBackupFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $InstallRecoveryPhase HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
    StrCpy $InstallRollbackCopyCode "active recovery phase could not be verified"
    Goto installBackupFailed
  ${ElseIf} $InstallRecoveryPhase != "active"
    StrCpy $InstallRollbackCopyCode "active recovery phase did not verify"
    Goto installBackupFailed
  ${EndIf}

  StrCpy $InstallRollbackState "1"
  DetailPrint "Application-file rollback snapshot created at $InstallRollbackDirectory."
  Return

  installBackupFailed:
  DetailPrint "Could not create the application-file rollback snapshot. Error: $InstallRollbackCopyCode"
  ${If} $RenameManifestHandle != ""
    FileClose $RenameManifestHandle
    StrCpy $RenameManifestHandle ""
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Pending"
  ${IfNot} ${Errors}
  ${AndIf} $0 == 1
    Call DiscardInstallRecoverySnapshot
  ${Else}
    ; Without Pending, the fixed tree is not ours and must not be changed.
    StrCpy $InstallRecoveryFailed "0"
  ${EndIf}
  ${If} $InstallRecoveryFailed == "0"
    Call ClearInstallRecoveryJournal
  ${Else}
    DetailPrint "Unsafe or incomplete recovery artifacts were retained with their journal."
  ${EndIf}
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "The existing application files could not be backed up safely. Installation will stop before replacing them."
  ${EndIf}
  Abort
FunctionEnd

Function RollbackInstallTransaction
  ${If} $InstallRollbackState != "1"
    Return
  ${EndIf}

  Call InitializeInstallRecoveryPaths
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "The fixed recovery directory could not be resolved; the recovery journal was retained."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}

  ; Registry Phase is the durable transaction decision. Never let a stale
  ; process-local rollback flag undo a committed install or replay a completed
  ; rollback after a commit readback/cleanup failure.
  ClearErrors
  ReadRegStr $InstallRecoveryPhase HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
    StrCpy $ApoRollbackStatus "The durable installation phase is missing; no rollback mutations were attempted and the journal was retained."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  ${If} $InstallRecoveryPhase == "committed"
  ${OrIf} $InstallRecoveryPhase == "rollback-cleanup"
    StrCpy $InstallRollbackState "0"
    Call CleanupCompletedInstallTransaction
    ${If} $InstallRecoveryFailed == "1"
      StrCpy $ApoRollbackStatus "The installation decision was already durable; cleanup is deferred and no rollback mutations were attempted."
    ${Else}
      StrCpy $ApoRollbackStatus "The installation decision was already durable; transaction cleanup completed without rollback."
    ${EndIf}
    DetailPrint "$ApoRollbackStatus"
    Return
  ${ElseIf} $InstallRecoveryPhase != "active"
    StrCpy $ApoRollbackStatus "The durable installation phase is not active; no rollback mutations were attempted and the journal was retained."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}

  Call ValidateActiveInstallRecoverySnapshot
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "The active recovery snapshot is missing, incomplete, or unsafe; no rollback mutations were attempted and the journal was retained."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  DetailPrint "Installation did not commit; restoring the pre-install application files and metadata."
  StrCpy $ApoRollbackRegistrationCode "not attempted"
  StrCpy $ApoRollbackStatus "The new application files were removed. User configuration was preserved."
  Call CloseRenameManifest

  ; Restore the exact pre-install task definition without relying on a helper
  ; executable which may itself be replaced or removed during rollback.
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "UpdaterOperationStarted"
  ${If} ${Errors}
    StrCpy $ApoRollbackStatus "Updater recovery state is missing. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${ElseIf} $0 == 1
    ReadRegDWORD $1 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousUpdateTaskPresent"
    ${If} ${Errors}
      StrCpy $ApoRollbackStatus "Previous updater-task state is missing. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    ClearErrors
    ReadRegDWORD $2 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "PreviousUpdateTaskXmlSaved"
    ${If} ${Errors}
      StrCpy $ApoRollbackStatus "Previous updater-task XML state is missing. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    ${If} $1 == 1
      ${If} $2 != 1
        StrCpy $ApoRollbackStatus "Previous updater-task XML state is malformed. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
      System::Call 'kernel32::GetFileAttributesW(w "$InstallRecoveryTaskXmlPath") i .r3 ?e'
      Pop $4
      ${If} $3 == ${INVALID_FILE_ATTRIBUTES}
        StrCpy $ApoRollbackStatus "The previous updater-task XML is missing. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
      IntOp $5 $3 & ${FILE_ATTRIBUTE_REPARSE_POINT}
      ${If} $5 != 0
        StrCpy $ApoRollbackStatus "The previous updater-task XML is unsafe. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
      nsExec::ExecToLog '"$SYSDIR\schtasks.exe" /Create /TN "EqualizerAPOUpdateChecker" /XML "$InstallRecoveryTaskXmlPath" /F'
      Pop $InstallOperationCode
      ${If} $InstallOperationCode == "error"
        StrCpy $ApoRollbackStatus "The exact previous updater task could not be recreated. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${ElseIf} $InstallOperationCode != 0
        StrCpy $ApoRollbackStatus "The exact previous updater task could not be recreated (exit code: $InstallOperationCode). Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
    ${ElseIf} $1 == 0
      ${If} $2 != 0
        StrCpy $ApoRollbackStatus "The absent updater-task XML state is malformed. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
      nsExec::ExecToLog '"$SYSDIR\schtasks.exe" /Query /TN "EqualizerAPOUpdateChecker" /FO LIST'
      Pop $InstallOperationCode
      ${If} $InstallOperationCode == "error"
        StrCpy $ApoRollbackStatus "The updater task could not be queried during rollback. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${ElseIf} $InstallOperationCode == 0
        nsExec::ExecToLog '"$SYSDIR\schtasks.exe" /Delete /TN "EqualizerAPOUpdateChecker" /F'
        Pop $InstallOperationCode
        ${If} $InstallOperationCode == "error"
          StrCpy $ApoRollbackStatus "The newly created updater task could not be deleted. Recovery files remain at $InstallRollbackDirectory."
          DetailPrint "$ApoRollbackStatus"
          Return
        ${ElseIf} $InstallOperationCode != 0
          StrCpy $ApoRollbackStatus "The newly created updater task could not be deleted (exit code: $InstallOperationCode). Recovery files remain at $InstallRollbackDirectory."
          DetailPrint "$ApoRollbackStatus"
          Return
        ${EndIf}
      ${ElseIf} $InstallOperationCode == 1
        System::Call 'kernel32::GetFileAttributesW(w "$WINDIR\System32\Tasks\EqualizerAPOUpdateChecker") i .r3 ?e'
        Pop $4
        ${If} $3 != ${INVALID_FILE_ATTRIBUTES}
          StrCpy $ApoRollbackStatus "The updater task query returned exit 1 but its backing file still exists. Recovery files remain at $InstallRollbackDirectory."
          DetailPrint "$ApoRollbackStatus"
          Return
        ${ElseIf} $4 != ${ERROR_FILE_NOT_FOUND}
        ${AndIf} $4 != ${ERROR_PATH_NOT_FOUND}
          StrCpy $ApoRollbackStatus "The updater task query returned exit 1 with Win32 error $4. Recovery files remain at $InstallRollbackDirectory."
          DetailPrint "$ApoRollbackStatus"
          Return
        ${EndIf}
      ${ElseIf} $InstallOperationCode != 1
        StrCpy $ApoRollbackStatus "The updater task query failed during rollback (exit code: $InstallOperationCode). Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
    ${Else}
      StrCpy $ApoRollbackStatus "Previous updater-task state is malformed. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "UpdaterOperationStarted" 0
    ${If} ${Errors}
      StrCpy $ApoRollbackStatus "The updater task was restored, but its rollback phase could not be persisted. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    ClearErrors
    ReadRegDWORD $3 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "UpdaterOperationStarted"
    ${If} ${Errors}
    ${OrIf} $3 != 0
      StrCpy $ApoRollbackStatus "The updater rollback phase did not verify. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
  ${ElseIf} $0 != 0
    StrCpy $ApoRollbackStatus "Updater recovery state is malformed. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "DeviceSelectorOperationStarted"
  ${If} ${Errors}
    StrCpy $ApoRollbackStatus "Device-selector recovery state is missing. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${ElseIf} $0 == 1
    ReadRegDWORD $1 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "DeviceSelectorMode"
    ${If} ${Errors}
      StrCpy $ApoRollbackStatus "Device-selector recovery mode is missing. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    ; Mode 2 is accepted only to finish a journal written by an older installer.
    ; New interactive selection is post-commit and never enters this transaction.
    ${If} $1 == 2
    ${AndIf} $PreviousApoPresent == 0
      ClearErrors
      StrCpy $InstallOperationCode "process did not start"
      ExecWait '"$INSTDIR\DeviceSelector.exe" /u /s' $InstallOperationCode
      ${If} ${Errors}
        StrCpy $ApoRollbackStatus "Fresh endpoint registration cleanup could not be started. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${ElseIf} $InstallOperationCode != 0
        StrCpy $ApoRollbackStatus "Fresh endpoint registration cleanup failed (exit code: $InstallOperationCode). Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
    ${ElseIf} $1 != 1
    ${AndIf} $1 != 2
      StrCpy $ApoRollbackStatus "Device-selector recovery mode is malformed. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "DeviceSelectorOperationStarted" 0
    ${If} ${Errors}
      StrCpy $ApoRollbackStatus "Device-selector rollback completed, but its phase could not be persisted. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    ClearErrors
    ReadRegDWORD $2 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "DeviceSelectorOperationStarted"
    ${If} ${Errors}
    ${OrIf} $2 != 0
      StrCpy $ApoRollbackStatus "Device-selector rollback phase did not verify. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
  ${ElseIf} $0 != 0
    StrCpy $ApoRollbackStatus "Device-selector recovery state is malformed. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}

  !if ${ENABLE_ASIO_PROXY} == 1
    ${If} $NewAsioRegAttempted == "1"
      ; Make the half-installed class undiscoverable before replacing its DLL.
      ; The exact previous registration is restored only after the previous file
      ; has been copied back below.
      Call HideAsioProxyRegistrationForRollback
      ${If} $InstallRecoveryFailed == "1"
        StrCpy $ApoRollbackStatus "Automatic rollback could not hide the partial Hibiki EQAPO driver registration. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
    ${EndIf}
  !endif

  ; Only a regsvr32 process which actually started can have left partial COM
  ; entries. Pre-registration extraction/verification failures must never
  ; unregister the previously installed APO.
  ${If} $NewApoRegistrationAttempted == "1"
    ${If} ${FileExists} "$INSTDIR\EqualizerAPO.dll"
      ClearErrors
      StrCpy $ApoRollbackUnregistrationCode "process did not start"
      ExecWait '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\EqualizerAPO.dll"' $ApoRollbackUnregistrationCode
      ${If} ${Errors}
        StrCpy $ApoRollbackStatus "Automatic rollback could not start regsvr32 to remove partial registration. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${ElseIf} $ApoRollbackUnregistrationCode != 0
        StrCpy $ApoRollbackStatus "Automatic rollback could not remove partial registration (regsvr32 exit code: $ApoRollbackUnregistrationCode). Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
    ${EndIf}
    StrCpy $NewApoRegistrationAttempted "0"
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewApoRegistrationAttempted" 0
    ${If} ${Errors}
      StrCpy $ApoRollbackStatus "The new APO was unregistered, but its recovery phase could not be persisted. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
  ${EndIf}

  StrCpy $InstallRecoveryFailed "0"
  Call RemoveInstalledProductFiles
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "Automatic rollback could not remove all new application files. User configuration was preserved and recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}

  Call ValidateInstallRecoveryComponents
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "The recovery tree changed before application files could be restored. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  nsExec::ExecToLog '"$SYSDIR\robocopy.exe" "$InstallRollbackFiles" "$INSTDIR" /E /COPY:DATS /DCOPY:DAT /R:1 /W:1 /XJ /NFL /NDL /NJH /NJS /NP'
  Pop $InstallRollbackCopyCode
  ${If} $InstallRollbackCopyCode == "error"
    StrCpy $ApoRollbackStatus "Automatic rollback could not restore all previous application files. User configuration was preserved and recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${ElseIf} $InstallRollbackCopyCode >= 8
    StrCpy $ApoRollbackStatus "Automatic rollback could not restore all previous application files (robocopy exit code: $InstallRollbackCopyCode). User configuration was preserved and recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}

  ${If} $PreviousApoPresent == "1"
    ${IfNot} ${FileExists} "$INSTDIR\EqualizerAPO.dll"
      StrCpy $ApoRollbackStatus "Previous application files were copied back, but EqualizerAPO.dll is missing. User configuration was preserved and recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}

    Call BeginProtectedAudioOverride
    ${If} $ProtectedAudioOverrideActive != "1"
      StrCpy $ApoRollbackStatus "The previous application files were restored, but the protected-audio recovery journal could not be created. User configuration was preserved and recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    ClearErrors
    StrCpy $ApoRollbackRegistrationCode "process did not start"
    ExecWait '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\EqualizerAPO.dll"' $ApoRollbackRegistrationCode
    ${If} ${Errors}
      Call RestoreProtectedAudioSetting
      StrCpy $ApoRollbackStatus "The previous application files were restored, but regsvr32 could not be started to re-register the previous APO. User configuration was preserved and recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    Call RestoreProtectedAudioSetting
    ${If} $ProtectedAudioOverrideActive == "1"
      StrCpy $ApoRollbackStatus "The previous application files and APO registration were restored, but the protected-audio setting could not be restored. User configuration was preserved and recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${ElseIf} $ApoRollbackRegistrationCode != 0
      StrCpy $ApoRollbackStatus "The previous application files were restored, but APO re-registration failed (regsvr32 exit code: $ApoRollbackRegistrationCode). User configuration was preserved and recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
    StrCpy $ApoRollbackStatus "The previous application files were restored and the previous APO was re-registered. User configuration was preserved."

    ; Make the restored APO active before declaring rollback complete.
    ${If} ${FileExists} "$INSTDIR\DeviceSelector.exe"
      ClearErrors
      StrCpy $InstallOperationCode "process did not start"
      ExecWait '"$INSTDIR\DeviceSelector.exe" /r /s' $InstallOperationCode
      ${If} ${Errors}
        StrCpy $ApoRollbackStatus "The previous APO was restored, but the audio service restart could not be started. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${ElseIf} $InstallOperationCode != 0
        StrCpy $ApoRollbackStatus "The previous APO was restored, but the audio service restart failed (exit code: $InstallOperationCode). Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
    ${EndIf}
  ${Else}
    StrCpy $ApoRollbackStatus "The new application files were removed. User configuration was preserved."
    ; Keep the now-empty root until the durable journal is cleared. Recovery on
    ; the next run must still be able to resolve and verify its physical target.
  ${EndIf}

  !if ${ENABLE_ASIO_PROXY} == 1
    ${If} $NewAsioRegAttempted == "1"
      Call RestoreAsioProxyRegistration
      ${If} $InstallRecoveryFailed == "1"
        StrCpy $ApoRollbackStatus "Application files were restored, but the previous Hibiki EQAPO driver registration could not be restored. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
    ${EndIf}
  !endif

  ; Restore only the ACL of the user-data root which was excluded from the app
  ; snapshot. /L never follows a replacement reparse point and no descendant is
  ; enumerated or modified.
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "ConfigAclSaved"
  ${If} ${Errors}
    StrCpy $ApoRollbackStatus "Application files were restored, but the config ACL recovery state is missing. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${ElseIf} $0 == 1
    nsExec::ExecToLog '"$SYSDIR\icacls.exe" "$INSTDIR" /restore "$InstallRecoveryAclPath" /L /Q'
    Pop $InstallOperationCode
    ${If} $InstallOperationCode == "error"
      StrCpy $ApoRollbackStatus "Application files were restored, but config ACL restoration could not start. Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${ElseIf} $InstallOperationCode != 0
      StrCpy $ApoRollbackStatus "Application files were restored, but config ACL restoration failed (exit code: $InstallOperationCode). Recovery files remain at $InstallRollbackDirectory."
      DetailPrint "$ApoRollbackStatus"
      Return
    ${EndIf}
  ${ElseIf} $0 == 0
    ${If} ${FileExists} "$INSTDIR\config"
      nsExec::ExecToLog '"$SYSDIR\icacls.exe" "$INSTDIR\config" /reset /L /Q'
      Pop $InstallOperationCode
      ${If} $InstallOperationCode == "error"
        StrCpy $ApoRollbackStatus "Application files were restored, but the new config ACL could not be reset. Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${ElseIf} $InstallOperationCode != 0
        StrCpy $ApoRollbackStatus "Application files were restored, but the new config ACL reset failed (exit code: $InstallOperationCode). Recovery files remain at $InstallRollbackDirectory."
        DetailPrint "$ApoRollbackStatus"
        Return
      ${EndIf}
    ${EndIf}
  ${Else}
    StrCpy $ApoRollbackStatus "Application files were restored, but the config ACL recovery state is malformed. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}

  Call RestoreInstallMetadata
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "Application files were restored, but previous installer registry metadata could not be restored. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  Call RestoreInstallShortcuts
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "Application files and registry metadata were restored, but previous shortcuts could not be restored. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}

  DetailPrint "$ApoRollbackStatus"
  ; Persist the rollback decision before cleanup. A crash from this point onward
  ; resumes manifest/snapshot cleanup and never replays system mutations.
  ClearErrors
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase" "rollback-cleanup"
  ${If} ${Errors}
    StrCpy $ApoRollbackStatus "Rollback completed, but its cleanup phase could not be persisted. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  ClearErrors
  ReadRegStr $InstallRecoveryPhase HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
  ${OrIf} $InstallRecoveryPhase != "rollback-cleanup"
    StrCpy $ApoRollbackStatus "Rollback completed, but its cleanup phase did not verify. Recovery files remain at $InstallRollbackDirectory."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  StrCpy $InstallRollbackState "0"
  Call PrepareRenameManifestForCleanup
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "Rollback completed; rename-manifest migration is deferred to the next installer run. Recovery files and journal were retained."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  Call DiscardRenamedProductFiles
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "Rollback completed; renamed-file cleanup is deferred to the next installer run. Recovery files and journal were retained."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  Call DiscardInstallRecoverySnapshot
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "Rollback completed; snapshot cleanup is deferred to the next installer run. Recovery files and journal were retained."
    DetailPrint "$ApoRollbackStatus"
    Return
  ${EndIf}
  Call ClearInstallRecoveryJournal
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $ApoRollbackStatus "Rollback completed, but the durable recovery journal could not be cleared. A later installer run will retry cleanup."
    DetailPrint "$ApoRollbackStatus"
  ${EndIf}
FunctionEnd

Function CommitInstallTransaction
  ; The committed phase is the atomic decision point. It is written only after
  ; every fallible transactional operation, including silent service restart and
  ; updater work. Interactive endpoint selection is a post-commit user action.
  StrCpy $InstallRecoveryFailed "0"
  ClearErrors
  WriteRegStr HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase" "committed"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  ReadRegStr $InstallRecoveryPhase HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${ElseIf} $InstallRecoveryPhase != "committed"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  Call CloseRenameManifest
  StrCpy $NewApoRegistrationAttempted "0"
  !if ${ENABLE_ASIO_PROXY} == 1
    StrCpy $NewAsioRegAttempted "0"
  !endif
  StrCpy $InstallRollbackState "0"
  ; Cleanup is restartable and journal-last. A cleanup failure after committed
  ; cannot turn a successful install into rollback; the next run resumes it.
  Call CleanupCompletedInstallTransaction
  ${If} $InstallRecoveryFailed == "1"
    DetailPrint "Installation committed; transaction cleanup was deferred to the next installer run."
    StrCpy $InstallRecoveryFailed "0"
  ${EndIf}
  DetailPrint "Installation transaction committed."
FunctionEnd

Function .onInstFailed
  ; Also covers extraction/verification failures before the registration check.
  Call CloseConfigRootIdentityHandle
  Call RollbackInstallTransaction
FunctionEnd

Function VerifyRequiredAssets
  !insertmacro RequireInstalledAsset "$INSTDIR\EqualizerAPO.dll"
  !if ${ENABLE_ASIO_PROXY} == 1
    !insertmacro RequireInstalledAsset "$INSTDIR\${ASIO_PROXY_DLL}"
  !endif
  !insertmacro RequireInstalledAsset "$INSTDIR\EqApoOutProcHost.exe"
  !insertmacro RequireInstalledAsset "$INSTDIR\DeviceSelector.exe"
  !insertmacro RequireInstalledAsset "$INSTDIR\Benchmark.exe"
  !insertmacro RequireInstalledAsset "$INSTDIR\VoicemeeterClient.exe"
  !insertmacro RequireInstalledAsset "$INSTDIR\UpdateChecker.exe"
  !insertmacro RequireInstalledAsset "$INSTDIR\Editor.exe"
  !insertmacro RequireInstalledAsset "$INSTDIR\NOTICE.md"
  !insertmacro RequireInstalledAsset "$INSTDIR\LICENSE.txt"
  !insertmacro RequireInstalledAsset "$INSTDIR\libfftw3.dll"
  !insertmacro RequireInstalledAsset "$INSTDIR\sndfile.dll"
  !insertmacro RequireInstalledAsset "$INSTDIR\Qt6Core.dll"
  !insertmacro RequireInstalledAsset "$INSTDIR\Qt6Gui.dll"
  !insertmacro RequireInstalledAsset "$INSTDIR\Qt6Network.dll"
  !insertmacro RequireInstalledAsset "$INSTDIR\Qt6Widgets.dll"
  !insertmacro RequireInstalledAsset "$INSTDIR\qt\platforms\qwindows.dll"
  !insertmacro RequireInstalledAsset "$INSTDIR\qt.conf"
  ${If} $ConfigRootCreated == "1"
    !insertmacro RequireInstalledAsset "$INSTDIR\config\config.txt"
  ${EndIf}
  Return

  missingRequiredAsset:
  DetailPrint "Required installer asset is missing: $MissingAsset"
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK $(AssetValidationError)
  ${EndIf}
  Call RollbackInstallTransaction
  Abort
FunctionEnd

Function WriteInstallDiagnostics
  StrCpy $DiagnosticIssues ""
  StrCpy $InstallLogPath "$INSTDIR\install-diagnostics.log"
  FileOpen $9 "$InstallLogPath" w
  ${If} $9 == ""
    Return
  ${EndIf}

  FileWrite $9 "Hibiki EQAPO installer diagnostics$\r$\n"
  FileWrite $9 "Version: ${VERSION}$\r$\n"
  FileWrite $9 "Target architecture: ${TARGET_ARCH}$\r$\n"
  FileWrite $9 "Install path: $INSTDIR$\r$\n"
  FileWrite $9 "$\r$\n"

  !insertmacro LogLoadLibrary "EqualizerAPO.dll"
  !insertmacro LogFileExists "EqApoOutProcHost.exe"
  !insertmacro LogFileExists "Editor.exe"
  !insertmacro LogFileExists "DeviceSelector.exe"
  !insertmacro LogFileExists "Benchmark.exe"
  !insertmacro LogFileExists "VoicemeeterClient.exe"
  !insertmacro LogFileExists "UpdateChecker.exe"
  !insertmacro LogLoadLibrary "libfftw3.dll"
  !insertmacro LogLoadLibrary "fftw3.dll"
  !insertmacro LogLoadLibrary "sndfile.dll"
  !insertmacro LogLoadLibrary "FLAC.dll"
  !insertmacro LogLoadLibrary "libmp3lame.dll"
  !insertmacro LogLoadLibrary "mpg123.dll"
  !insertmacro LogLoadLibrary "ogg.dll"
  !insertmacro LogLoadLibrary "opus.dll"
  !insertmacro LogLoadLibrary "vorbis.dll"
  !insertmacro LogLoadLibrary "vorbisenc.dll"
  !insertmacro LogLoadLibrary "vorbisfile.dll"
  !insertmacro LogLoadLibrary "msvcp140.dll"
  !insertmacro LogLoadLibrary "msvcp140_1.dll"
  !insertmacro LogLoadLibrary "vcruntime140.dll"
  !insertmacro LogLoadLibrary "vcruntime140_1.dll"
  !insertmacro LogLoadLibrary "Qt6Core.dll"
  !insertmacro LogLoadLibrary "Qt6Gui.dll"
  !insertmacro LogLoadLibrary "Qt6Network.dll"
  !insertmacro LogLoadLibrary "Qt6Svg.dll"
  !insertmacro LogLoadLibrary "Qt6Widgets.dll"
  !insertmacro LogLoadLibrary "d3dcompiler_47.dll"

  FileClose $9
FunctionEnd

Function InstallBundledRuntimeDlls
  SetOutPath "$INSTDIR"
  File "${LIBPATH}\libfftw3.dll"
  File "${LIBPATH}\fftw3.dll"
  File "${LIBPATH}\sndfile.dll"
  File "${LIBPATH}\FLAC.dll"
  File "${LIBPATH}\libmp3lame.dll"
  File "${LIBPATH}\mpg123.dll"
  File "${LIBPATH}\ogg.dll"
  File "${LIBPATH}\opus.dll"
  File "${LIBPATH}\vorbis.dll"
  File "${LIBPATH}\vorbisenc.dll"
  File "${LIBPATH}\vorbisfile.dll"
  File "${LIBPATH}\msvcp140.dll"
  File "${LIBPATH}\msvcp140_1.dll"
  File "${LIBPATH}\vcruntime140.dll"
  File "${LIBPATH}\vcruntime140_1.dll"
  File "${LIBPATH}\d3dcompiler_47.dll"
  !if /FileExists "${LIBPATH}\dxcompiler.dll"
    File "${LIBPATH}\dxcompiler.dll"
  !endif
  !if /FileExists "${LIBPATH}\dxil.dll"
    File "${LIBPATH}\dxil.dll"
  !endif
  File "${LIBPATH}\icuuc.dll"
  File "${LIBPATH}\Qt6Core.dll"
  File "${LIBPATH}\Qt6Gui.dll"
  File "${LIBPATH}\Qt6Network.dll"
  File "${LIBPATH}\Qt6Svg.dll"
  File "${LIBPATH}\Qt6Widgets.dll"
FunctionEnd

Function WriteX64LoadDiagnostics
  Push $OUTDIR
  SetOutPath "$SYSDIR"
  !insertmacro RunEmbeddedX64LoadCheck
  Pop $OUTDIR
  SetOutPath $OUTDIR

  ${If} $2 != "powershell not found"
    FileOpen $9 "$InstallLogPath" a
    ${If} $9 != ""
      FileWrite $9 "64-bit LoadLibrary diagnostic exit code: $2$\r$\n"
      FileClose $9
    ${EndIf}
  ${Else}
    FileOpen $9 "$InstallLogPath" a
    ${If} $9 != ""
      FileWrite $9 "$\r$\n64-bit PowerShell was not found. Skipping LoadLibrary diagnostic.$\r$\n"
      FileClose $9
    ${EndIf}
  ${EndIf}
FunctionEnd

;--------------------------------
;Installer Sections
LangString SecCheckForUpdates ${LANG_ENGLISH} "Check for updates automatically"
LangString SecCheckForUpdates ${LANG_SPANISH} "Buscar actualizaciones automaticamente"
LangString SecCheckForUpdates ${LANG_TRADCHINESE} "自動檢查更新"
LangString SecCheckForUpdates ${LANG_SIMPCHINESE} "自动检查更新"
LangString SecCheckForUpdates ${LANG_GERMAN} "Automatisch auf Updates prüfen"

Section /o $(SecCheckForUpdates) SecCheckForUpdates
SectionEnd

Section "-Install"
  ; Keep every elevated child process on a trusted CWD. Validate the nearest
  ; existing ancestor before creating a new leaf, then validate the complete
  ; path before SetOutPath, restore-point creation, extraction or execution.
  SetOutPath "$SYSDIR"
  StrCpy $InstallRootAllowMissing "1"
  Call ValidateInstallRootAcl
  ${If} $InstallOperationCode == "error"
    StrCpy $InstallFailureReason "starting install-root trust validation"
    Goto installRootTrustFailed
  ${ElseIf} $InstallOperationCode != 0
    StrCpy $InstallFailureReason "validating the install-root ancestor (exit code: $InstallOperationCode)"
    Goto installRootTrustFailed
  ${EndIf}
  ClearErrors
  CreateDirectory "$INSTDIR"
  ${If} ${Errors}
    StrCpy $InstallFailureReason "creating the installation directory"
    Goto installRootTrustFailed
  ${EndIf}
  StrCpy $InstallRootAllowMissing "0"
  Call ValidateInstallRootAcl
  ${If} $InstallOperationCode == "error"
    StrCpy $InstallFailureReason "starting final install-root trust validation"
    Goto installRootTrustFailed
  ${ElseIf} $InstallOperationCode != 0
    StrCpy $InstallFailureReason "validating the complete installation path (exit code: $InstallOperationCode)"
    Goto installRootTrustFailed
  ${EndIf}

  SetOutPath "$INSTDIR"
  Call CreateRestorePoint
  Goto installRootTrusted

  installRootTrustFailed:
  DetailPrint "Setup stopped before extraction while $InstallFailureReason."
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "Setup cannot safely use this installation directory while $InstallFailureReason. Choose a local, system-protected NTFS/ReFS folder. No product files were extracted or executed."
  ${EndIf}
  SetErrorLevel 1
  Abort

  installRootTrusted:
  ; Read both shortcut locations before the durable journal is created so they
  ; can be restored after an interrupted or failed upgrade.
  !insertmacro MUI_STARTMENU_GETFOLDER Application $OldStartMenuFolder

  ; Migrate only known historical default prefixes while preserving custom
  ; folder names and every unrelated shortcut inside an old folder.
  StrCpy $0 "$OldStartMenuFolder" 13
  ${If} $0 == "Hibiki EQAPO "
    StrCpy $StartMenuFolder "${PRODUCT_LABEL} ${VERSION}"
  ${Else}
    StrCpy $0 "$OldStartMenuFolder" 14
    ${If} $0 == "Equalizer APO "
      StrCpy $StartMenuFolder "${PRODUCT_LABEL} ${VERSION}"
    ${Else}
      StrCpy $0 "$OldStartMenuFolder" 38
      ${If} $0 == "Loudness Correction for Equalizer APO "
        StrCpy $StartMenuFolder "${PRODUCT_LABEL} ${VERSION}"
      ${ElseIf} $OldStartMenuFolder == "Hibiki EQAPO"
        StrCpy $StartMenuFolder "${PRODUCT_LABEL} ${VERSION}"
      ${ElseIf} $OldStartMenuFolder == "Equalizer APO"
        StrCpy $StartMenuFolder "${PRODUCT_LABEL} ${VERSION}"
      ${ElseIf} $OldStartMenuFolder == "Loudness Correction for Equalizer APO"
        StrCpy $StartMenuFolder "${PRODUCT_LABEL} ${VERSION}"
      ${ElseIf} $StartMenuFolder == ""
        StrCpy $StartMenuFolder "${PRODUCT_LABEL} ${VERSION}"
      ${EndIf}
    ${EndIf}
  ${EndIf}

  ; Snapshot every installed application file and relevant metadata before
  ; deleting or replacing anything.
  Call PrepareInstallTransaction

  Delete "$INSTDIR\Configurator.exe"
  Delete "$INSTDIR\Qt5Core.dll"
  Delete "$INSTDIR\Qt5Gui.dll"
  Delete "$INSTDIR\Qt5Widgets.dll"
  Delete "$INSTDIR\qt\imageformats\qgif.dll"
  Delete "$INSTDIR\qt\imageformats\qjpeg.dll"
  Delete "$INSTDIR\qt\styles\qwindowsvistastyle.dll"

  ;Rename before delete as these files may be in use
  !insertmacro RenameAndDelete "$INSTDIR\EqualizerAPO.dll"
  ${If} $PreviousApoPresent == "1"
    ${If} ${FileExists} "$INSTDIR\EqualizerAPO.dll"
      ; The active DLL could not be moved, so do not attempt to overwrite it.
      Call RollbackInstallTransaction
      DetailPrint "The existing EqualizerAPO.dll could not be moved aside."
      ${IfNot} ${Silent}
        MessageBox MB_ICONSTOP|MB_OK "The existing EqualizerAPO.dll is still in use and could not be replaced safely. Installation will stop."
      ${EndIf}
      Abort
    ${EndIf}
  ${EndIf}
  !if ${ENABLE_ASIO_PROXY} == 1
    !insertmacro RenameAndDelete "$INSTDIR\${ASIO_PROXY_DLL}"
  !endif
  !insertmacro RenameAndDelete "$INSTDIR\EqApoOutProcHost.exe"
  !insertmacro RenameAndDelete "$INSTDIR\EqApoJuceVST3Host.dll"
  !insertmacro RenameAndDelete "$INSTDIR\EqApoOutProcJuceHost.exe"
  !insertmacro RenameAndDelete "$INSTDIR\libfftw3-3.dll"
  !insertmacro RenameAndDelete "$INSTDIR\libfftw3.dll"
  !insertmacro RenameAndDelete "$INSTDIR\fftw3.dll"
  !insertmacro RenameAndDelete "$INSTDIR\libsndfile-1.dll"
  !insertmacro RenameAndDelete "$INSTDIR\sndfile.dll"
  !insertmacro RenameAndDelete "$INSTDIR\samplerate.dll"
  !insertmacro RenameAndDelete "$INSTDIR\msvcp100.dll"
  !insertmacro RenameAndDelete "$INSTDIR\msvcr100.dll"
  !insertmacro RenameAndDelete "$INSTDIR\msvcp120.dll"
  !insertmacro RenameAndDelete "$INSTDIR\msvcr120.dll"
  !insertmacro RenameAndDelete "$INSTDIR\msvcp140.dll"
  !insertmacro RenameAndDelete "$INSTDIR\msvcp140_1.dll"
  !insertmacro RenameAndDelete "$INSTDIR\msvcp140_2.dll"
  !insertmacro RenameAndDelete "$INSTDIR\icudt.dll"
  !insertmacro RenameAndDelete "$INSTDIR\icuin.dll"
  !insertmacro RenameAndDelete "$INSTDIR\icuuc.dll"
  !insertmacro RenameAndDelete "$INSTDIR\icudt78.dll"
  !insertmacro RenameAndDelete "$INSTDIR\icuin78.dll"
  !insertmacro RenameAndDelete "$INSTDIR\icuuc78.dll"
  !insertmacro RenameAndDelete "$INSTDIR\VoicemeeterClient.exe"
  !insertmacro RenameAndDelete "$INSTDIR\vcruntime140.dll"
  !insertmacro RenameAndDelete "$INSTDIR\vcruntime140_1.dll"
  !insertmacro RenameAndDelete "$INSTDIR\FLAC.dll"
  !insertmacro RenameAndDelete "$INSTDIR\libmp3lame.dll"
  !insertmacro RenameAndDelete "$INSTDIR\mpg123.dll"
  !insertmacro RenameAndDelete "$INSTDIR\ogg.dll"
  !insertmacro RenameAndDelete "$INSTDIR\opus.dll"
  !insertmacro RenameAndDelete "$INSTDIR\vorbis.dll"
  !insertmacro RenameAndDelete "$INSTDIR\vorbisenc.dll"
  !insertmacro RenameAndDelete "$INSTDIR\vorbisfile.dll"
  !insertmacro RenameAndDelete "$INSTDIR\d3dcompiler_47.dll"
  !insertmacro RenameAndDelete "$INSTDIR\dxcompiler.dll"
  !insertmacro RenameAndDelete "$INSTDIR\dxil.dll"
  !insertmacro RenameAndDelete "$INSTDIR\Qt6Core.dll"
  !insertmacro RenameAndDelete "$INSTDIR\Qt6Gui.dll"
  !insertmacro RenameAndDelete "$INSTDIR\Qt6Network.dll"
  !insertmacro RenameAndDelete "$INSTDIR\Qt6Svg.dll"
  !insertmacro RenameAndDelete "$INSTDIR\Qt6Widgets.dll"

  File "${BINPATH}\EqualizerAPO.dll"
  !if ${ENABLE_ASIO_PROXY} == 1
    File "${LIBPATH}\${ASIO_PROXY_DLL}"
  !endif
  File "${BINPATH}\EqApoOutProcHost.exe"
  File "${BINPATH}\DeviceSelector.exe"
  File "${BINPATH}\Benchmark.exe"
  File "${BINPATH}\VoicemeeterClient.exe"
  File "${BINPATH}\UpdateChecker.exe"
  File /oname=NOTICE.md "..\NOTICE.md"
  File /oname=LICENSE.txt "..\LICENSE"
  File "${BINPATH_EDITOR}\Editor.exe"

  File "${LIBPATH}\libfftw3.dll"
  File "${LIBPATH}\fftw3.dll"
  File "${LIBPATH}\sndfile.dll"
  File "${LIBPATH}\FLAC.dll"
  File "${LIBPATH}\libmp3lame.dll"
  File "${LIBPATH}\mpg123.dll"
  File "${LIBPATH}\ogg.dll"
  File "${LIBPATH}\opus.dll"
  File "${LIBPATH}\vorbis.dll"
  File "${LIBPATH}\vorbisenc.dll"
  File "${LIBPATH}\vorbisfile.dll"
  File "${LIBPATH}\msvcp140.dll"
  File "${LIBPATH}\msvcp140_1.dll"
  File "${LIBPATH}\vcruntime140.dll"
  File "${LIBPATH}\vcruntime140_1.dll"
  File "${LIBPATH}\d3dcompiler_47.dll"
  !if /FileExists "${LIBPATH}\dxcompiler.dll"
    File "${LIBPATH}\dxcompiler.dll"
  !endif
  !if /FileExists "${LIBPATH}\dxil.dll"
    File "${LIBPATH}\dxil.dll"
  !endif
  File "${LIBPATH}\icuuc.dll"
  File "${LIBPATH}\Qt6Core.dll"
  File "${LIBPATH}\Qt6Gui.dll"
  File "${LIBPATH}\Qt6Network.dll"
  File "${LIBPATH}\Qt6Svg.dll"
  File "${LIBPATH}\Qt6Widgets.dll"

  CreateDirectory "$INSTDIR\qt"
  CreateDirectory "$INSTDIR\qt\generic"
  CreateDirectory "$INSTDIR\qt\iconengines"
  CreateDirectory "$INSTDIR\qt\imageformats"
  CreateDirectory "$INSTDIR\qt\networkinformation"
  CreateDirectory "$INSTDIR\qt\platforms"
  CreateDirectory "$INSTDIR\qt\styles"
  CreateDirectory "$INSTDIR\qt\tls"

  File /oname=qt\generic\qtuiotouchplugin.dll "${LIBPATH}\qt\generic\qtuiotouchplugin.dll"
  File /oname=qt\iconengines\qsvgicon.dll "${LIBPATH}\qt\iconengines\qsvgicon.dll"
  File /nonfatal /oname=qt\imageformats\qgif.dll "${LIBPATH}\qt\imageformats\qgif.dll"
  File /oname=qt\imageformats\qico.dll "${LIBPATH}\qt\imageformats\qico.dll"
  File /nonfatal /oname=qt\imageformats\qjpeg.dll "${LIBPATH}\qt\imageformats\qjpeg.dll"
  File /oname=qt\imageformats\qsvg.dll "${LIBPATH}\qt\imageformats\qsvg.dll"
  File /oname=qt\networkinformation\qnetworklistmanager.dll "${LIBPATH}\qt\networkinformation\qnetworklistmanager.dll"
  File /oname=qt\platforms\qwindows.dll "${LIBPATH}\qt\platforms\qwindows.dll"
  File /oname=qt\styles\qmodernwindowsstyle.dll "${LIBPATH}\qt\styles\qmodernwindowsstyle.dll"
  File /oname=qt\tls\qcertonlybackend.dll "${LIBPATH}\qt\tls\qcertonlybackend.dll"
  File /oname=qt\tls\qschannelbackend.dll "${LIBPATH}\qt\tls\qschannelbackend.dll"

  File "Configuration tutorial (online).url"
  File "Configuration reference (online).url"
  File "qt.conf"

  ; Never write through an existing user-writable config tree. A fresh root
  ; inherits the protected install-root ACL, so defaults can be created before
  ; Users receive access. Existing roots are only validated and receive the
  ; root-only ACL update below; applications create any missing user data later.
  StrCpy $ConfigRootCreated "0"
  System::Call 'kernel32::GetFileAttributesW(w "$INSTDIR\config") i .r0 ?e'
  Pop $1
  ${If} $0 == ${INVALID_FILE_ATTRIBUTES}
    ${If} $1 != ${ERROR_FILE_NOT_FOUND}
    ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
      StrCpy $InstallFailureReason "inspecting the config directory (Win32 error: $1)"
      Goto installTransactionFailed
    ${EndIf}
    ClearErrors
    CreateDirectory "$INSTDIR\config"
    ${If} ${Errors}
      StrCpy $InstallFailureReason "creating the config directory"
      Goto installTransactionFailed
    ${EndIf}
    StrCpy $ConfigRootCreated "1"
  ${EndIf}
  Call ValidateAndHoldConfigRoot
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallFailureReason "validating the config directory"
    Goto installTransactionFailed
  ${EndIf}
  ${If} $ConfigRootCreated == "1"
    CreateDirectory "$INSTDIR\config\HeadphoneCalibrations"
    CreateDirectory "$INSTDIR\config\IRs"
    SetOverwrite off
    File /oname=config\config.txt "config\config.txt"
    File /oname=config\example.txt "config\example.txt"
    File /oname=config\demo.txt "config\demo.txt"
    File /oname=config\multichannel.txt "config\multichannel.txt"
    File /oname=config\iir_lowpass.txt "config\iir_lowpass.txt"
    File /oname=config\selective_delay.txt "config\selective_delay.txt"
    SetOverwrite on
  ${EndIf}
  ; /L binds the operation to the root entry and omitting /T and /C prevents
  ; explicit traversal of existing user descendants, including junctions.
  nsExec::ExecToLog '"$SYSDIR\icacls.exe" "$INSTDIR\config" /grant *S-1-5-32-545:(OI)(CI)F /L /Q'
  Pop $InstallOperationCode
  ${If} $InstallOperationCode == "error"
    StrCpy $InstallFailureReason "starting the config ACL update"
    Goto installTransactionFailed
  ${ElseIf} $InstallOperationCode != 0
    StrCpy $InstallFailureReason "updating the config ACL (exit code: $InstallOperationCode)"
    Goto installTransactionFailed
  ${EndIf}
  Call CloseConfigRootIdentityHandle
  CreateDirectory "$INSTDIR\VSTPlugins"

  ; Do not make persistent system changes when the installer payload is incomplete.
  Call VerifyRequiredAssets
  Call WriteInstallDiagnostics

  Call BeginProtectedAudioOverride
  ${If} $ProtectedAudioOverrideActive != "1"
    StrCpy $FailedRegistrationCode "protected-audio recovery journal could not be created"
    Goto apoRegistrationFailed
  ${EndIf}
  ; Persist the possibility of partial COM registration before starting
  ; regsvr32. A power loss while the child process is running must still cause
  ; the next installer to unregister the new DLL before restoring the old one.
  StrCpy $NewApoRegistrationAttempted "1"
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewApoRegistrationAttempted" 1
  ${If} ${Errors}
    StrCpy $FailedRegistrationCode "registration recovery phase could not be persisted"
    Call RestoreProtectedAudioSetting
    Goto apoRegistrationFailed
  ${EndIf}
  ; RegDLL does not work for 64-bit DLLs. Treat process-creation failure as a
  ; hard failure; ExecWait's result variable is undefined in that case.
  ClearErrors
  StrCpy $1 "process did not start"
  ExecWait '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\EqualizerAPO.dll"' $1
  ${If} ${Errors}
    StrCpy $FailedRegistrationCode "process did not start"
    Call RestoreProtectedAudioSetting
    Goto apoRegistrationFailed
  ${EndIf}
  Call RestoreProtectedAudioSetting
  ${If} $ProtectedAudioOverrideActive == "1"
    StrCpy $FailedRegistrationCode "protected-audio setting could not be restored"
    Goto apoRegistrationFailed
  ${EndIf}
  ${If} $1 != 0
    StrCpy $FailedRegistrationCode "$1"
    ${If} $DiagnosticIssues != ""
      ${IfNot} ${Silent}
        MessageBox MB_ICONQUESTION|MB_YESNO "Hibiki EQAPO could not be registered.$\r$\n$\r$\nThe installer found these missing or incompatible bundled files:$\r$\n$\r$\n$DiagnosticIssues$\r$\nSetup can reinstall the bundled DLLs into the compatibility installation folder and retry registration.$\r$\n$\r$\nThis will not replace Windows system DLLs.$\r$\n$\r$\nDo you want to reinstall the bundled DLLs and retry?" IDNO apoRegistrationFailed
        Call InstallBundledRuntimeDlls
        Call WriteInstallDiagnostics
        Call BeginProtectedAudioOverride
        ${If} $ProtectedAudioOverrideActive != "1"
          StrCpy $FailedRegistrationCode "protected-audio recovery journal could not be recreated for registration retry"
          Goto apoRegistrationFailed
        ${EndIf}
        ClearErrors
        StrCpy $1 "process did not start"
        ExecWait '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\EqualizerAPO.dll"' $1
        ${If} ${Errors}
          StrCpy $FailedRegistrationCode "retry process did not start"
          Call RestoreProtectedAudioSetting
          Goto apoRegistrationFailed
        ${EndIf}
        Call RestoreProtectedAudioSetting
        ${If} $ProtectedAudioOverrideActive == "1"
          StrCpy $FailedRegistrationCode "protected-audio setting could not be restored after registration retry"
          Goto apoRegistrationFailed
        ${EndIf}
        ${If} $1 == 0
          Goto apoRegistrationSucceeded
        ${EndIf}
        StrCpy $FailedRegistrationCode "$1 (retry)"
      ${EndIf}
    ${EndIf}
    Goto apoRegistrationFailed
  ${EndIf}
  Goto apoRegistrationSucceeded

  apoRegistrationFailed:
  FileOpen $9 "$InstallLogPath" a
  ${If} $9 != ""
    FileWrite $9 "$\r$\nregsvr32 result: $FailedRegistrationCode$\r$\n"
    FileClose $9
  ${EndIf}
  Call WriteX64LoadDiagnostics
  Call RollbackInstallTransaction
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "Hibiki EQAPO could not be registered. Installation will stop before modifying audio devices.$\r$\n$\r$\nThis usually means a required runtime DLL is missing or incompatible.$\r$\n$\r$\nregsvr32 result: $FailedRegistrationCode$\r$\nDiagnostics log: $InstallLogPath$\r$\n$\r$\n$ApoRollbackStatus"
  ${EndIf}
  Abort

  apoRegistrationSucceeded:
  StrCpy $OLDINSTDIR ""
  ClearErrors
  ReadRegStr $OLDINSTDIR HKLM ${REGPATH} "InstallPath"
  !insertmacro WriteRequiredRegStr HKLM "${REGPATH}" "InstallPath" "$INSTDIR" "writing the installation path"

  ; Write ConfigPath if non-existing or if InstallPath has changed.
  StrCpy $0 ""
  ClearErrors
  ReadRegStr $0 HKLM ${REGPATH} "ConfigPath"
  ${If} $0 == ""
  ${OrIf} $INSTDIR != $OLDINSTDIR
    !insertmacro WriteRequiredRegStr HKLM "${REGPATH}" "ConfigPath" "$INSTDIR\config" "writing the configuration path"
  ${EndIf}

  StrCpy $0 ""
  ClearErrors
  ReadRegStr $0 HKLM ${REGPATH} "EnableTrace"
  ${If} $0 == ""
    !insertmacro WriteRequiredRegStr HKLM "${REGPATH}" "EnableTrace" "false" "writing the trace setting"
  ${EndIf}

  !insertmacro WriteRequiredRegDWORD HKLM "${REGPATH}" "BrandMigrationVersion" ${BRAND_MIGRATION_VERSION} "writing the Hibiki EQAPO brand migration marker"
  !if ${ENABLE_ASIO_PROXY} == 1
    Call ApplyAsioProxyRegistration
    ${If} $InstallRecoveryFailed == "1"
      Goto installTransactionFailed
    ${EndIf}
  !endif

  ClearErrors
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ${If} ${Errors}
    StrCpy $InstallFailureReason "writing the uninstaller"
    Goto installTransactionFailed
  ${EndIf}

  ; Replace only the product shortcuts after registration succeeds. Unrelated
  ; shortcuts in the same custom folder remain untouched.
  !insertmacro DeleteProductShortcuts $OldStartMenuFolder
  ${If} $InstallOperationCode != 0
    StrCpy $InstallFailureReason "removing previous product shortcuts"
    Goto installTransactionFailed
  ${EndIf}
  !insertmacro MUI_STARTMENU_WRITE_BEGIN Application
  ClearErrors
  CreateDirectory "$SMPROGRAMS\$StartMenuFolder"
  ${If} ${Errors}
    StrCpy $InstallFailureReason "creating the Start Menu folder"
    Goto installTransactionFailed
  ${EndIf}
  !insertmacro CreateRequiredShortcut "$SMPROGRAMS\$StartMenuFolder\Hibiki EQAPO Configuration Editor.lnk" "$INSTDIR\Editor.exe"
  !insertmacro CreateRequiredShortcut "$SMPROGRAMS\$StartMenuFolder\Configuration tutorial (online).lnk" "$INSTDIR\Configuration tutorial (online).url"
  !insertmacro CreateRequiredShortcut "$SMPROGRAMS\$StartMenuFolder\Configuration reference (online).lnk" "$INSTDIR\Configuration reference (online).url"
  !insertmacro CreateRequiredShortcut "$SMPROGRAMS\$StartMenuFolder\Hibiki EQAPO Device Selector.lnk" "$INSTDIR\DeviceSelector.exe"
  !insertmacro CreateRequiredShortcut "$SMPROGRAMS\$StartMenuFolder\Benchmark.lnk" "$INSTDIR\Benchmark.exe"
  !insertmacro CreateRequiredShortcut "$SMPROGRAMS\$StartMenuFolder\Check for updates.lnk" "$INSTDIR\UpdateChecker.exe"
  !insertmacro CreateRequiredShortcut "$SMPROGRAMS\$StartMenuFolder\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  !insertmacro MUI_STARTMENU_WRITE_END
  !insertmacro WriteRequiredRegStr HKLM "${REGPATH}" "Start Menu Folder" "$StartMenuFolder" "writing the Start Menu folder"

  !insertmacro WriteRequiredRegStr HKLM "${UNINST_REGPATH}" "DisplayName" "${PRODUCT_FULL_LABEL}" "writing the uninstall display name"
  !insertmacro WriteRequiredRegStr HKLM "${UNINST_REGPATH}" "DisplayVersion" "${VERSION}" "writing the uninstall display version"
  !insertmacro WriteRequiredRegStr HKLM "${UNINST_REGPATH}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\"" "writing the uninstall command"
  !insertmacro WriteRequiredRegDWORD HKLM "${UNINST_REGPATH}" "NoModify" 1 "writing the uninstall NoModify flag"
  !insertmacro WriteRequiredRegDWORD HKLM "${UNINST_REGPATH}" "NoRepair" 1 "writing the uninstall NoRepair flag"

  ; Silent installation performs only the reversible service restart inside the
  ; transaction. Interactive endpoint selection is intentionally post-commit.
  ${If} ${Silent}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "DeviceSelectorMode" 1
    ${If} ${Errors}
      StrCpy $InstallFailureReason "persisting the silent Device Selector recovery mode"
      Goto installTransactionFailed
    ${EndIf}
    ClearErrors
    WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "DeviceSelectorOperationStarted" 1
    ${If} ${Errors}
      StrCpy $InstallFailureReason "persisting the silent Device Selector recovery phase"
      Goto installTransactionFailed
    ${EndIf}
    ClearErrors
    StrCpy $DeviceSelectorResult "process did not start"
    ExecWait '"$INSTDIR\DeviceSelector.exe" /r /s' $DeviceSelectorResult
    ${If} ${Errors}
      SetRebootFlag true
      StrCpy $InstallFailureReason "starting the silent audio-service restart"
      Goto installTransactionFailed
    ${ElseIf} $DeviceSelectorResult != 0
      SetRebootFlag true
      StrCpy $InstallFailureReason "restarting the audio service (exit code: $DeviceSelectorResult)"
      Goto installTransactionFailed
    ${EndIf}
  ${EndIf}

  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "UpdaterOperationStarted" 1
  ${If} ${Errors}
    StrCpy $InstallFailureReason "persisting the updater recovery phase"
    Goto installTransactionFailed
  ${EndIf}
  ClearErrors
  StrCpy $InstallOperationCode "process did not start"
  ${If} ${SectionIsSelected} ${SecCheckForUpdates}
    ${If} ${Silent}
      ExecWait '"$INSTDIR\UpdateChecker.exe" -i -s' $InstallOperationCode
    ${Else}
      ExecWait '"$INSTDIR\UpdateChecker.exe" -i' $InstallOperationCode
    ${EndIf}
  ${Else}
    ${If} ${Silent}
      ExecWait '"$INSTDIR\UpdateChecker.exe" -u -s' $InstallOperationCode
    ${Else}
      ExecWait '"$INSTDIR\UpdateChecker.exe" -u' $InstallOperationCode
    ${EndIf}
  ${EndIf}
  ${If} ${Errors}
    StrCpy $InstallFailureReason "starting Update Checker task configuration"
    Goto installTransactionFailed
  ${ElseIf} $InstallOperationCode != 0
    StrCpy $InstallFailureReason "configuring the Update Checker task (exit code: $InstallOperationCode)"
    Goto installTransactionFailed
  ${EndIf}

  ; The durable commit ends the rollback transaction. Endpoint selection below
  ; is a separate user action and can never cause transaction rollback.
  Call CommitInstallTransaction
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallFailureReason "writing the final installation commit marker"
    Goto installTransactionFailed
  ${EndIf}
  ${IfNot} ${Silent}
    ClearErrors
    StrCpy $DeviceSelectorResult "process did not start"
    ExecWait '"$INSTDIR\DeviceSelector.exe" /i' $DeviceSelectorResult
    ${If} ${Errors}
      SetRebootFlag true
      MessageBox MB_ICONEXCLAMATION|MB_OK "Installation completed, but Device Selector could not be started. Run Hibiki EQAPO Device Selector from the Start Menu to choose audio endpoints. No committed files were rolled back."
    ${ElseIf} $DeviceSelectorResult != 0
      SetRebootFlag true
      MessageBox MB_ICONEXCLAMATION|MB_OK "Installation completed, but Device Selector exited with code $DeviceSelectorResult. Run it again from the Start Menu if endpoint changes were not applied. No committed files were rolled back."
    ${EndIf}
  ${EndIf}
  Goto installTransactionComplete

  installTransactionFailed:
  Call CloseConfigRootIdentityHandle
  Call RollbackInstallTransaction
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "Installation could not complete while $InstallFailureReason.$\r$\n$\r$\nThe installer attempted to restore the previous application files, registry metadata, shortcuts, update task, audio registration and protected-audio setting.$\r$\n$\r$\n$ApoRollbackStatus"
  ${EndIf}
  Abort

  installTransactionComplete:
SectionEnd

;--------------------------------
;Uninstaller Sections

Function un.BlockIfInstallRecoveryJournalExists
  ; An application-tree transaction is owned by Setup.exe. The uninstaller must
  ; never guess whether a partial snapshot, registration change or rename
  ; manifest can be discarded. Even a structurally valid pending journal is a
  ; hard stop until Setup.exe has replayed recovery and removed the key.
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $InstallFailureReason ""
  !if ${LIBPATH} != "lib32"
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${INSTALLER_APP_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  !else
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${INSTALLER_APP_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE}, *p .r0) i .r1'
  !endif
  ${If} $1 == ${ERROR_FILE_NOT_FOUND}
    Return
  ${ElseIf} $1 != 0
    StrCpy $InstallFailureReason "the installer recovery journal could not be inspected (registry error: $1)"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  ${If} $1 != 0
    StrCpy $InstallFailureReason "the installer recovery journal could not be closed safely (registry error: $1)"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Pending"
  ${If} ${Errors}
    Goto malformedInstallRecoveryJournal
  ${EndIf}
  ClearErrors
  ReadRegStr $InstallRecoveryPhase HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
  ${OrIf} $InstallRecoveryPhase == ""
    Goto malformedInstallRecoveryJournal
  ${EndIf}
  ClearErrors
  ReadRegDWORD $1 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "JournalVersion"
  ${If} ${Errors}
  ${OrIf} $1 != ${INSTALL_RECOVERY_JOURNAL_VERSION}
  ${OrIf} $0 != 1
    Goto malformedInstallRecoveryJournal
  ${EndIf}

  ${If} $InstallRecoveryPhase == "initializing"
  ${OrIf} $InstallRecoveryPhase == "preparing"
  ${OrIf} $InstallRecoveryPhase == "prepared"
  ${OrIf} $InstallRecoveryPhase == "active"
  ${OrIf} $InstallRecoveryPhase == "committed"
  ${OrIf} $InstallRecoveryPhase == "rollback-cleanup"
    StrCpy $InstallFailureReason "a pending Setup recovery transaction (phase: $InstallRecoveryPhase)"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  malformedInstallRecoveryJournal:
  StrCpy $InstallFailureReason "an unknown, malformed or partial Setup recovery journal"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function un.LoadUninstallRecoveryTargetForInit
  ; If product metadata was already removed before an interruption, only a
  ; complete, versioned uninstall journal may supply the path used to bind this
  ; exact Uninstall.exe. Full step/phase validation is repeated before actions.
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $UninstallRecoveryInstallPath ""
  !if ${LIBPATH} != "lib32"
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  !else
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE}, *p .r0) i .r1'
  !endif
  ${If} $1 != 0
    Goto invalidUninstallRecoveryTarget
  ${EndIf}
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  ${If} $1 != 0
    Goto invalidUninstallRecoveryTarget
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${UNINSTALLER_RECOVERY_REGPATH} "JournalVersion"
  ${If} ${Errors}
  ${OrIf} $0 != ${UNINSTALLER_RECOVERY_JOURNAL_VERSION}
    Goto invalidUninstallRecoveryTarget
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Pending"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    Goto invalidUninstallRecoveryTarget
  ${EndIf}
  ClearErrors
  ReadRegStr $UninstallRecoveryPhase HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
    Goto invalidUninstallRecoveryTarget
  ${EndIf}
  ${If} $UninstallRecoveryPhase != "critical"
  ${AndIf} $UninstallRecoveryPhase != "payload"
    Goto invalidUninstallRecoveryTarget
  ${EndIf}
  ClearErrors
  ReadRegStr $UninstallRecoveryInstallPath HKLM ${UNINSTALLER_RECOVERY_REGPATH} "InstallPath"
  ${If} ${Errors}
  ${OrIf} $UninstallRecoveryInstallPath == ""
    Goto invalidUninstallRecoveryTarget
  ${EndIf}
  Return

  invalidUninstallRecoveryTarget:
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function un.onInit
  !if ${LIBPATH} != "lib32"
    SetRegView 64
  !endif
  StrCpy $RemoveUserConfigurationRequested "0"

  ; This must be the first state gate. Do not read or execute any installed
  ; payload while Setup owns a pending or unverifiable recovery transaction.
  StrCpy $InstallRecoveryFailed "0"
  Call un.BlockIfInstallRecoveryJournalExists
  ${If} $InstallRecoveryFailed == "1"
    Goto uninstallInstallRecoveryBlocked
  ${EndIf}

  ; $INSTDIR in an uninstaller initially comes from the executable's directory.
  ; Bind it to the protected machine-wide registration before executing or
  ; loading any installed sidecar; a copied Uninstall.exe must be inert.
  ClearErrors
  ReadRegStr $0 HKLM ${REGPATH} "InstallPath"
  ${If} ${Errors}
    Goto loadUninstallRecoveryTarget
  ${ElseIf} $0 == ""
    Goto loadUninstallRecoveryTarget
  ${EndIf}
  Goto normalizeUninstallRegisteredTarget

  loadUninstallRecoveryTarget:
  Call un.LoadUninstallRecoveryTargetForInit
  ${If} $InstallRecoveryFailed == "1"
    Goto uninstallTrustValidationFailed
  ${EndIf}
  StrCpy $0 "$UninstallRecoveryInstallPath"

  normalizeUninstallRegisteredTarget:
  ClearErrors
  GetFullPathName $1 "$0"
  ${If} ${Errors}
  ${OrIf} $1 == ""
    Goto uninstallTrustValidationFailed
  ${EndIf}
  normalizeRegisteredUninstallRootTail:
  StrCpy $3 "$1" 1 -1
  ${If} $3 == "\"
    ${GetRoot} "$1" $4
    ${If} $1 != $4
    ${AndIf} $1 != "$4\"
      StrCpy $1 "$1" -1
      Goto normalizeRegisteredUninstallRootTail
    ${EndIf}
  ${EndIf}

  ClearErrors
  GetFullPathName $2 "$EXEDIR"
  ${If} ${Errors}
  ${OrIf} $2 == ""
    Goto uninstallTrustValidationFailed
  ${EndIf}
  normalizeExecutableUninstallRootTail:
  StrCpy $3 "$2" 1 -1
  ${If} $3 == "\"
    ${GetRoot} "$2" $4
    ${If} $2 != $4
    ${AndIf} $2 != "$4\"
      StrCpy $2 "$2" -1
      Goto normalizeExecutableUninstallRootTail
    ${EndIf}
  ${EndIf}

  ${If} $1 != $2
    Goto uninstallTrustValidationFailed
  ${EndIf}
  StrCpy $INSTDIR "$1"
  StrCpy $InstallRootAllowMissing "0"
  Call un.ValidateInstallRootAcl
  ${If} $InstallOperationCode == "error"
    Goto uninstallTrustValidationFailed
  ${ElseIf} $InstallOperationCode != 0
    Goto uninstallTrustValidationFailed
  ${EndIf}
  Return

  uninstallInstallRecoveryBlocked:
  DetailPrint "Uninstall refused $InstallFailureReason. The Setup recovery journal was retained."
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "Uninstall cannot safely continue because Setup recovery is pending or cannot be verified. Run the same Setup.exe again first so it can finish recovery, then retry uninstall.$\r$\n$\r$\nThe recovery journal was retained."
  ${EndIf}
  SetErrorLevel 1
  Abort

  uninstallTrustValidationFailed:
  DetailPrint "Uninstall refused an unregistered, missing, reparse-point, or weak-ACL installation directory."
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "Uninstall cannot safely trust this installation directory. Run the registered Uninstall.exe from a system-protected installation folder, or repair the installation first. No installed application was started and no product state was changed."
  ${EndIf}
  SetErrorLevel 1
  Abort
FunctionEnd

Function un.ValidateInstallRootAcl
  !insertmacro ValidateInstallRootAclBody
FunctionEnd

Function un.FlushUninstallTransaction
  StrCpy $InstallRecoveryFailed "0"
  !if ${LIBPATH} != "lib32"
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  !else
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE}, *p .r0) i .r1'
  !endif
  ${If} $1 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call 'advapi32::RegFlushKey(p r0) i .r1'
  System::Call 'advapi32::RegCloseKey(p r0) i .r2'
  ${If} $1 != 0
  ${OrIf} $2 != 0
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function un.PrepareUninstallTransaction
  ; This peer key survives DeleteRegKey ${REGPATH}. It records completed
  ; non-file side effects before payload deletion can remove the helpers needed
  ; to repeat them. A missing helper is accepted only when this protected,
  ; versioned journal proves that exact operation already completed.
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $UninstallRecoveryPhase ""
  StrCpy $UninstallRecoveryInstallPath ""
  StrCpy $UninstallUpdateCheckerDone "0"
  StrCpy $UninstallDeviceSelectorDone "0"
  StrCpy $UninstallApoUnregistered "0"
  StrCpy $UninstallAsioProxyUnregistered "0"

  !if ${LIBPATH} != "lib32"
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  !else
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE}, *p .r0) i .r1'
  !endif
  ${If} $1 == ${ERROR_FILE_NOT_FOUND}
    Goto createUninstallTransaction
  ${ElseIf} $1 != 0
    StrCpy $InstallFailureReason "opening the uninstall recovery journal (registry error: $1)"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  ${If} $1 != 0
    StrCpy $InstallFailureReason "closing the uninstall recovery journal (registry error: $1)"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegDWORD $0 HKLM ${UNINSTALLER_RECOVERY_REGPATH} "JournalVersion"
  ${If} ${Errors}
  ${OrIf} $0 != ${UNINSTALLER_RECOVERY_JOURNAL_VERSION}
    Goto malformedUninstallTransaction
  ${EndIf}
  ClearErrors
  ReadRegStr $UninstallRecoveryInstallPath HKLM ${UNINSTALLER_RECOVERY_REGPATH} "InstallPath"
  ${If} ${Errors}
  ${OrIf} $UninstallRecoveryInstallPath == ""
  ${OrIf} $UninstallRecoveryInstallPath != "$INSTDIR"
    Goto malformedUninstallTransaction
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Pending"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    Goto malformedUninstallTransaction
  ${EndIf}
  ClearErrors
  ReadRegStr $UninstallRecoveryPhase HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
  ${OrIf} $UninstallRecoveryPhase == ""
    Goto malformedUninstallTransaction
  ${EndIf}
  ClearErrors
  ReadRegDWORD $UninstallUpdateCheckerDone HKLM ${UNINSTALLER_RECOVERY_REGPATH} "UpdateCheckerDone"
  ${If} ${Errors}
    Goto malformedUninstallTransaction
  ${EndIf}
  ClearErrors
  ReadRegDWORD $UninstallDeviceSelectorDone HKLM ${UNINSTALLER_RECOVERY_REGPATH} "DeviceSelectorDone"
  ${If} ${Errors}
    Goto malformedUninstallTransaction
  ${EndIf}
  ClearErrors
  ReadRegDWORD $UninstallApoUnregistered HKLM ${UNINSTALLER_RECOVERY_REGPATH} "ApoUnregistered"
  ${If} ${Errors}
    Goto malformedUninstallTransaction
  ${EndIf}
  ClearErrors
  ReadRegDWORD $UninstallAsioProxyUnregistered HKLM ${UNINSTALLER_RECOVERY_REGPATH} "AsioProxyUnregistered"
  ${If} ${Errors}
    Goto malformedUninstallTransaction
  ${EndIf}
  ${If} $UninstallUpdateCheckerDone != 0
  ${AndIf} $UninstallUpdateCheckerDone != 1
    Goto malformedUninstallTransaction
  ${EndIf}
  ${If} $UninstallDeviceSelectorDone != 0
  ${AndIf} $UninstallDeviceSelectorDone != 1
    Goto malformedUninstallTransaction
  ${EndIf}
  ${If} $UninstallApoUnregistered != 0
  ${AndIf} $UninstallApoUnregistered != 1
    Goto malformedUninstallTransaction
  ${EndIf}
  ${If} $UninstallAsioProxyUnregistered != 0
  ${AndIf} $UninstallAsioProxyUnregistered != 1
    Goto malformedUninstallTransaction
  ${EndIf}

  ${If} $UninstallRecoveryPhase == "critical"
    ${If} $UninstallUpdateCheckerDone == 0
    ${AndIfNot} ${FileExists} "$INSTDIR\UpdateChecker.exe"
      Goto missingUninstallUpdateChecker
    ${EndIf}
    ${If} $UninstallDeviceSelectorDone == 0
    ${AndIfNot} ${FileExists} "$INSTDIR\DeviceSelector.exe"
      Goto missingUninstallDeviceSelector
    ${EndIf}
    ${If} $UninstallApoUnregistered == 0
    ${AndIfNot} ${FileExists} "$INSTDIR\EqualizerAPO.dll"
      Goto missingUninstallApo
    ${EndIf}
    !if ${ENABLE_ASIO_PROXY} == 1
      ${If} $UninstallAsioProxyUnregistered == 0
        Call un.CheckAsioProxyDllUnlocked
        ${If} $InstallOperationCode != 0
          Goto lockedUninstallAsioProxy
        ${EndIf}
      ${EndIf}
    !endif
    Return
  ${ElseIf} $UninstallRecoveryPhase == "payload"
    ${If} $UninstallUpdateCheckerDone != 1
    ${OrIf} $UninstallDeviceSelectorDone != 1
    ${OrIf} $UninstallApoUnregistered != 1
    ${OrIf} $UninstallAsioProxyUnregistered != 1
      Goto malformedUninstallTransaction
    ${EndIf}
    Return
  ${EndIf}
  Goto malformedUninstallTransaction

  createUninstallTransaction:
  ; Validate all critical helpers before the first journal write. Therefore an
  ; initially damaged installation can never manufacture "done" state merely
  ; because a helper was already absent.
  ${IfNot} ${FileExists} "$INSTDIR\UpdateChecker.exe"
    Goto missingUninstallUpdateChecker
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\DeviceSelector.exe"
    Goto missingUninstallDeviceSelector
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\EqualizerAPO.dll"
    Goto missingUninstallApo
  ${EndIf}
  !if ${ENABLE_ASIO_PROXY} == 1
    Call un.CheckAsioProxyDllUnlocked
    ${If} $InstallOperationCode != 0
      Goto lockedUninstallAsioProxy
    ${EndIf}
  !endif

  ClearErrors
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "JournalVersion" ${UNINSTALLER_RECOVERY_JOURNAL_VERSION}
  ${If} ${Errors}
    Goto writeUninstallTransactionFailed
  ${EndIf}
  ClearErrors
  WriteRegStr HKLM ${UNINSTALLER_RECOVERY_REGPATH} "InstallPath" "$INSTDIR"
  ${If} ${Errors}
    Goto writeUninstallTransactionFailed
  ${EndIf}
  ClearErrors
  WriteRegStr HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Phase" "critical"
  ${If} ${Errors}
    Goto writeUninstallTransactionFailed
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "UpdateCheckerDone" 0
  ${If} ${Errors}
    Goto writeUninstallTransactionFailed
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "DeviceSelectorDone" 0
  ${If} ${Errors}
    Goto writeUninstallTransactionFailed
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "ApoUnregistered" 0
  ${If} ${Errors}
    Goto writeUninstallTransactionFailed
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "AsioProxyUnregistered" 0
  ${If} ${Errors}
    Goto writeUninstallTransactionFailed
  ${EndIf}
  ; Pending is last: a crash before it is an explicitly malformed partial
  ; journal, never evidence that a critical operation completed.
  ClearErrors
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Pending" 1
  ${If} ${Errors}
    Goto writeUninstallTransactionFailed
  ${EndIf}
  Call un.FlushUninstallTransaction
  ${If} $InstallRecoveryFailed == "1"
    StrCpy $InstallFailureReason "durably creating the uninstall recovery journal"
    Return
  ${EndIf}
  StrCpy $UninstallRecoveryInstallPath "$INSTDIR"
  StrCpy $UninstallRecoveryPhase "critical"
  Return

  missingUninstallUpdateChecker:
  StrCpy $InstallFailureReason "validating the required UpdateChecker.exe before uninstall"
  StrCpy $InstallRecoveryFailed "1"
  Return
  missingUninstallDeviceSelector:
  StrCpy $InstallFailureReason "validating the required DeviceSelector.exe before uninstall"
  StrCpy $InstallRecoveryFailed "1"
  Return
  missingUninstallApo:
  StrCpy $InstallFailureReason "validating the required EqualizerAPO.dll before uninstall"
  StrCpy $InstallRecoveryFailed "1"
  Return
  lockedUninstallAsioProxy:
  StrCpy $InstallFailureReason "checking the Hibiki EQAPO driver file; close every DAW and retry (Win32 error: $InstallOperationCode)"
  StrCpy $InstallRecoveryFailed "1"
  Return
  writeUninstallTransactionFailed:
  StrCpy $InstallFailureReason "creating the uninstall recovery journal"
  StrCpy $InstallRecoveryFailed "1"
  Return
  malformedUninstallTransaction:
  StrCpy $InstallFailureReason "validating an unknown, malformed or partial uninstall recovery journal"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function un.PersistUninstallStep
  StrCpy $InstallRecoveryFailed "0"
  ${If} $UninstallRecoveryPhase != "critical"
    Goto persistUninstallStepFailed
  ${EndIf}
  ${If} $UninstallRecoveryStepName != "UpdateCheckerDone"
  ${AndIf} $UninstallRecoveryStepName != "DeviceSelectorDone"
  ${AndIf} $UninstallRecoveryStepName != "ApoUnregistered"
  ${AndIf} $UninstallRecoveryStepName != "AsioProxyUnregistered"
    Goto persistUninstallStepFailed
  ${EndIf}
  ClearErrors
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "$UninstallRecoveryStepName" 1
  ${If} ${Errors}
    Goto persistUninstallStepFailed
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${UNINSTALLER_RECOVERY_REGPATH} "$UninstallRecoveryStepName"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    Goto persistUninstallStepFailed
  ${EndIf}
  Call un.FlushUninstallTransaction
  ${If} $InstallRecoveryFailed == "1"
    Goto persistUninstallStepFailed
  ${EndIf}
  ${If} $UninstallRecoveryStepName == "UpdateCheckerDone"
    StrCpy $UninstallUpdateCheckerDone "1"
  ${ElseIf} $UninstallRecoveryStepName == "DeviceSelectorDone"
    StrCpy $UninstallDeviceSelectorDone "1"
  ${ElseIf} $UninstallRecoveryStepName == "ApoUnregistered"
    StrCpy $UninstallApoUnregistered "1"
  ${ElseIf} $UninstallRecoveryStepName == "AsioProxyUnregistered"
    StrCpy $UninstallAsioProxyUnregistered "1"
  ${EndIf}
  Return

  persistUninstallStepFailed:
  StrCpy $InstallFailureReason "persisting uninstall step $UninstallRecoveryStepName"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function un.MarkUninstallPayloadPhase
  StrCpy $InstallRecoveryFailed "0"
  ${If} $UninstallUpdateCheckerDone != 1
  ${OrIf} $UninstallDeviceSelectorDone != 1
  ${OrIf} $UninstallApoUnregistered != 1
  ${OrIf} $UninstallAsioProxyUnregistered != 1
    StrCpy $InstallFailureReason "validating completed critical uninstall steps"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  WriteRegStr HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Phase" "payload"
  ${If} ${Errors}
    Goto persistUninstallPayloadPhaseFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $UninstallRecoveryPhase HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Phase"
  ${If} ${Errors}
  ${OrIf} $UninstallRecoveryPhase != "payload"
    Goto persistUninstallPayloadPhaseFailed
  ${EndIf}
  Call un.FlushUninstallTransaction
  ${If} $InstallRecoveryFailed == "1"
    Goto persistUninstallPayloadPhaseFailed
  ${EndIf}
  Return

  persistUninstallPayloadPhaseFailed:
  StrCpy $InstallFailureReason "persisting the payload-cleanup uninstall phase"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function un.RestoreCompletedUninstallTransaction
  ; Used only if a final self-removal step fails after journal cleanup. Recreate
  ; the already-completed critical-step proof before exposing a retry entry.
  StrCpy $InstallRecoveryFailed "0"
  ClearErrors
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "JournalVersion" ${UNINSTALLER_RECOVERY_JOURNAL_VERSION}
  WriteRegStr HKLM ${UNINSTALLER_RECOVERY_REGPATH} "InstallPath" "$INSTDIR"
  WriteRegStr HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Phase" "payload"
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "UpdateCheckerDone" 1
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "DeviceSelectorDone" 1
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "ApoUnregistered" 1
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "AsioProxyUnregistered" 1
  WriteRegDWORD HKLM ${UNINSTALLER_RECOVERY_REGPATH} "Pending" 1
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  StrCpy $UninstallRecoveryPhase "payload"
  Call un.FlushUninstallTransaction
FunctionEnd

Function un.ClearUninstallTransaction
  ; Journal-last cleanup. This key is a sibling of ${REGPATH}, so deleting
  ; product metadata cannot erase retry proof before payload cleanup succeeds.
  StrCpy $InstallRecoveryFailed "0"
  ${If} $UninstallRecoveryPhase != "payload"
    Goto clearUninstallTransactionFailed
  ${EndIf}
  ClearErrors
  DeleteRegKey HKLM ${UNINSTALLER_RECOVERY_REGPATH}
  !if ${LIBPATH} != "lib32"
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  !else
    System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${UNINSTALLER_RECOVERY_REGPATH}", i 0, i ${KEY_QUERY_VALUE}, *p .r0) i .r1'
  !endif
  ${If} $1 == ${ERROR_FILE_NOT_FOUND}
    Return
  ${ElseIf} $1 == 0
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  ${EndIf}

  clearUninstallTransactionFailed:
  StrCpy $InstallFailureReason "clearing the completed uninstall recovery journal"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function un.ValidateProductChildDirectory
  ; Validate a caller-supplied product child without following a directory
  ; reparse point. Missing children are safe no-ops; access errors fail closed.
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $InstallRecoveryPathExists "0"
  ClearErrors
  GetFullPathName $0 "$INSTDIR"
  ${If} ${Errors}
  ${OrIf} $0 == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  normalizeUninstallRootTail:
  StrCpy $2 "$0" 1 -1
  ${If} $2 == "\"
    ${GetRoot} "$0" $3
    ${If} $0 != $3
    ${AndIf} $0 != "$3\"
      StrCpy $0 "$0" -1
      Goto normalizeUninstallRootTail
    ${EndIf}
  ${EndIf}

  ClearErrors
  GetFullPathName $1 "$InstallRecoveryPathToCheck"
  ${If} ${Errors}
  ${OrIf} $1 == ""
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  StrCpy $2 "$0\"
  StrLen $3 "$2"
  StrCpy $4 "$1" $3
  ${If} $4 != "$2"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  System::Call 'kernel32::GetFileAttributesW(w "$0") i .r2 ?e'
  Pop $3
  ${If} $2 == ${INVALID_FILE_ATTRIBUTES}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $3 $2 & ${FILE_ATTRIBUTE_DIRECTORY}
  ${If} $3 == 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $3 $2 & ${FILE_ATTRIBUTE_REPARSE_POINT}
  ${If} $3 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  System::Call 'kernel32::GetFileAttributesW(w "$1") i .r2 ?e'
  Pop $3
  ${If} $2 == ${INVALID_FILE_ATTRIBUTES}
    ${If} $3 == ${ERROR_FILE_NOT_FOUND}
    ${OrIf} $3 == ${ERROR_PATH_NOT_FOUND}
      StrCpy $InstallRecoveryPathToCheck "$1"
      Return
    ${EndIf}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $3 $2 & ${FILE_ATTRIBUTE_DIRECTORY}
  ${If} $3 == 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  IntOp $3 $2 & ${FILE_ATTRIBUTE_REPARSE_POINT}
  ${If} $3 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$1"
  StrCpy $InstallRecoveryPathExists "1"
FunctionEnd

Function un.RemoveQtPluginTreeSafely
  ; Validate every product-owned directory which will be traversed by an exact
  ; Delete path. Unknown content and every reparse point are retained.
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\qt"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    Goto unsafeQtTree
  ${EndIf}
  ${If} $InstallRecoveryPathExists == "0"
    Return
  ${EndIf}

  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\qt\generic"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    Goto unsafeQtTree
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\qt\iconengines"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    Goto unsafeQtTree
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\qt\imageformats"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    Goto unsafeQtTree
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\qt\networkinformation"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    Goto unsafeQtTree
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\qt\platforms"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    Goto unsafeQtTree
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\qt\styles"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    Goto unsafeQtTree
  ${EndIf}
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\qt\tls"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    Goto unsafeQtTree
  ${EndIf}

  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\generic\qtuiotouchplugin.dll" "the Qt touch plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\iconengines\qsvgicon.dll" "the Qt SVG icon plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\imageformats\qico.dll" "the Qt ICO plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\imageformats\qsvg.dll" "the Qt SVG image plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\imageformats\qgif.dll" "the legacy Qt GIF plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\imageformats\qjpeg.dll" "the legacy Qt JPEG plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\networkinformation\qnetworklistmanager.dll" "the Qt network plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\platforms\qwindows.dll" "the Qt platform plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\styles\qmodernwindowsstyle.dll" "the Qt modern style plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\styles\qwindowsvistastyle.dll" "the legacy Qt style plug-in"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\tls\qcertonlybackend.dll" "the Qt certificate backend"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt\tls\qschannelbackend.dll" "the Qt TLS backend"
  !insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\qt\generic" "the Qt generic plug-in directory"
  !insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\qt\iconengines" "the Qt icon plug-in directory"
  !insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\qt\imageformats" "the Qt image plug-in directory"
  !insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\qt\networkinformation" "the Qt network plug-in directory"
  !insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\qt\platforms" "the Qt platform plug-in directory"
  !insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\qt\styles" "the Qt style plug-in directory"
  !insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\qt\tls" "the Qt TLS plug-in directory"
  !insertmacro RemoveUninstallPayloadDirectory "$INSTDIR\qt" "the Qt plug-in tree"
  Return

  unsafeQtTree:
  DetailPrint "The Qt plug-in tree contains an unsafe or inaccessible path and was retained: $INSTDIR\qt"
  StrCpy $InstallRecoveryFailed "1"
  StrCpy $InstallFailureReason "validating the Qt plug-in tree"
FunctionEnd

Function un.RemoveRequestedUserConfiguration
  Delete "$INSTDIR\*.reg"
  StrCpy $InstallRecoveryPathToCheck "$INSTDIR\config"
  Call un.ValidateProductChildDirectory
  ${If} $InstallRecoveryFailed == "1"
    DetailPrint "The configuration directory contains an unsafe or inaccessible path and was retained: $INSTDIR\config"
  ${ElseIf} $InstallRecoveryPathExists == "1"
    ; Delete immediate configuration files only. Subdirectories are retained so
    ; the elevated uninstaller never recursively follows a user-created junction.
    Delete "$InstallRecoveryPathToCheck\*.*"
    RMDir /REBOOTOK "$InstallRecoveryPathToCheck"
    ${If} ${FileExists} "$InstallRecoveryPathToCheck"
      DetailPrint "Configuration subdirectories or locked files were retained: $InstallRecoveryPathToCheck"
    ${EndIf}
  ${EndIf}
  DeleteRegKey HKCU ${REGPATH}
FunctionEnd

Function un.StopInstalledProductProcesses
  Push $OUTDIR
  SetOutPath "$SYSDIR"
  !insertmacro RunEmbeddedProcessStopper "0"
  Pop $OUTDIR
  SetOutPath $OUTDIR
FunctionEnd

Function un.CheckInstalledProductProcesses
  Push $OUTDIR
  SetOutPath "$SYSDIR"
  ; The out-of-process host has no document UI and may be stopped immediately.
  ; Interactive product applications are only identified in this pass; exit 2
  ; means explicit consent is required before force termination.
  !insertmacro RunEmbeddedProcessStopper "1"
  Pop $OUTDIR
  SetOutPath $OUTDIR
FunctionEnd

LangString SecRemoveName ${LANG_ENGLISH} "Remove configurations and registry backups"
LangString SecRemoveName ${LANG_SPANISH} "Eliminar configuraciones y copias de seguridad del registro"
LangString SecRemoveName ${LANG_GERMAN} "Konfigurationen und Registrierungsbackups entfernen"
LangString SecRemoveName ${LANG_TRADCHINESE} "移除設定檔與登錄檔備份"
LangString SecRemoveName ${LANG_SIMPCHINESE} "移除配置文件与注册表备份"

Section /o un.$(SecRemoveName)
  ; Section selection is recorded now, but destructive user-data removal is
  ; deferred until every critical unregistration step below has succeeded.
  StrCpy $RemoveUserConfigurationRequested "1"
SectionEnd

Section "-un.Uninstall"
  !if ${LIBPATH} != "lib32"
	SetRegView 64
  !endif

  Call un.PrepareUninstallTransaction
  ${If} $InstallRecoveryFailed == "1"
    Goto uninstallCriticalCleanupFailed
  ${EndIf}

  ;Qt applications only work if working directory is set to application directory
  Push $OUTDIR
  SetOutPath $INSTDIR
  Call un.CheckInstalledProductProcesses
  ${If} $InstallOperationCode == 2
    ${If} ${Silent}
      StrCpy $InstallFailureReason "running interactive Hibiki EQAPO applications; close them before silent uninstall"
      Goto uninstallRestoreOutDirAndFail
    ${EndIf}
    MessageBox MB_YESNO|MB_ICONEXCLAMATION $(UninstallCloseAppsPrompt) IDYES uninstallStopConfirmed
    StrCpy $InstallFailureReason "the user declined to close running Hibiki EQAPO applications"
    Goto uninstallRestoreOutDirAndFail
    uninstallStopConfirmed:
    Call un.StopInstalledProductProcesses
    ${If} $InstallOperationCode != 0
      StrCpy $InstallFailureReason "stopping installed Hibiki EQAPO processes (exit code: $InstallOperationCode)"
      Goto uninstallRestoreOutDirAndFail
    ${EndIf}
  ${ElseIf} $InstallOperationCode != 0
    StrCpy $InstallFailureReason "stopping installed Hibiki EQAPO processes (exit code: $InstallOperationCode)"
    Goto uninstallRestoreOutDirAndFail
  ${EndIf}
  ${If} $UninstallUpdateCheckerDone == "0"
    ClearErrors
    StrCpy $InstallOperationCode "process did not start"
    ${If} ${Silent}
      ExecWait '"$INSTDIR\UpdateChecker.exe" -u -s' $InstallOperationCode
    ${Else}
      ExecWait '"$INSTDIR\UpdateChecker.exe" -u' $InstallOperationCode
    ${EndIf}
    ${If} ${Errors}
      StrCpy $InstallFailureReason "starting Update Checker task removal"
      Goto uninstallRestoreOutDirAndFail
    ${ElseIf} $InstallOperationCode != 0
      StrCpy $InstallFailureReason "removing the Update Checker task (exit code: $InstallOperationCode)"
      Goto uninstallRestoreOutDirAndFail
    ${EndIf}
    StrCpy $UninstallRecoveryStepName "UpdateCheckerDone"
    Call un.PersistUninstallStep
    ${If} $InstallRecoveryFailed == "1"
      Goto uninstallRestoreOutDirAndFail
    ${EndIf}
  ${EndIf}

  ${If} $UninstallDeviceSelectorDone == "0"
    ClearErrors
    StrCpy $InstallOperationCode "process did not start"
    ${If} ${Silent}
      ExecWait '"$INSTDIR\DeviceSelector.exe" /u /s' $InstallOperationCode
    ${Else}
      ExecWait '"$INSTDIR\DeviceSelector.exe" /u' $InstallOperationCode
    ${EndIf}
    ${If} ${Errors}
      StrCpy $InstallFailureReason "starting audio-device cleanup"
      Goto uninstallRestoreOutDirAndFail
    ${ElseIf} $InstallOperationCode != 0
      StrCpy $InstallFailureReason "removing audio-device registration (exit code: $InstallOperationCode)"
      Goto uninstallRestoreOutDirAndFail
    ${EndIf}
    StrCpy $UninstallRecoveryStepName "DeviceSelectorDone"
    Call un.PersistUninstallStep
    ${If} $InstallRecoveryFailed == "1"
      Goto uninstallRestoreOutDirAndFail
    ${EndIf}
  ${EndIf}
  Pop $OUTDIR
  SetOutPath $OUTDIR

  ${If} $UninstallAsioProxyUnregistered == "0"
    !if ${ENABLE_ASIO_PROXY} == 1
      Call un.UnregisterAsioProxy
      ${If} $InstallRecoveryFailed == "1"
        Goto uninstallCriticalCleanupFailed
      ${EndIf}
    !endif
    StrCpy $UninstallRecoveryStepName "AsioProxyUnregistered"
    Call un.PersistUninstallStep
    ${If} $InstallRecoveryFailed == "1"
      Goto uninstallCriticalCleanupFailed
    ${EndIf}
  ${EndIf}

  ${If} $UninstallApoUnregistered == "0"
    ClearErrors
    StrCpy $InstallOperationCode "process did not start"
    ExecWait '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\EqualizerAPO.dll"' $InstallOperationCode
    ${If} ${Errors}
      StrCpy $InstallFailureReason "starting APO unregistration"
      Goto uninstallCriticalCleanupFailed
    ${ElseIf} $InstallOperationCode != 0
      StrCpy $InstallFailureReason "unregistering the APO (exit code: $InstallOperationCode)"
      Goto uninstallCriticalCleanupFailed
    ${EndIf}
    StrCpy $UninstallRecoveryStepName "ApoUnregistered"
    Call un.PersistUninstallStep
    ${If} $InstallRecoveryFailed == "1"
      Goto uninstallCriticalCleanupFailed
    ${EndIf}
  ${EndIf}
  Call un.MarkUninstallPayloadPhase
  ${If} $InstallRecoveryFailed == "1"
    Goto uninstallCriticalCleanupFailed
  ${EndIf}
  Goto uninstallCriticalCleanupSucceeded

  uninstallRestoreOutDirAndFail:
  Pop $OUTDIR
  SetOutPath $OUTDIR
  uninstallCriticalCleanupFailed:
  DetailPrint "Uninstall stopped while $InstallFailureReason. Product files and uninstall metadata were retained."
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "Uninstall could not safely complete while $InstallFailureReason.$\r$\n$\r$\nProduct files and uninstall information were retained. Close running Hibiki EQAPO applications and try again."
  ${EndIf}
  SetErrorLevel 1
  Abort

  uninstallCriticalCleanupSucceeded:
  StrCpy $InstallRecoveryFailed "0"
  !insertmacro MUI_STARTMENU_GETFOLDER Application $StartMenuFolder
  !insertmacro DeleteProductShortcuts $StartMenuFolder
  ${If} $InstallOperationCode != 0
    StrCpy $InstallRecoveryFailed "1"
    StrCpy $InstallFailureReason "removing product shortcuts"
  ${EndIf}

  ; VSTPlugins and config can contain user files. Remove the directory only when
  ; already empty and do not treat unrelated content as product cleanup failure.
  RMDir "$INSTDIR\VSTPlugins"

  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Configuration reference (online).url" "the configuration reference shortcut"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Configuration tutorial (online).url" "the configuration tutorial shortcut"
  Call un.RemoveQtPluginTreeSafely
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Qt6Widgets.dll" "Qt6Widgets.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Qt6Svg.dll" "Qt6Svg.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Qt6Network.dll" "Qt6Network.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Qt6Gui.dll" "Qt6Gui.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Qt6Core.dll" "Qt6Core.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\icuuc.dll" "icuuc.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\dxil.dll" "dxil.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\dxcompiler.dll" "dxcompiler.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\d3dcompiler_47.dll" "d3dcompiler_47.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\vcruntime140_1.dll" "vcruntime140_1.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\vcruntime140.dll" "vcruntime140.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\msvcp140_1.dll" "msvcp140_1.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\msvcp140.dll" "msvcp140.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\sndfile.dll" "sndfile.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\samplerate.dll" "the legacy samplerate.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\libfftw3.dll" "libfftw3.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\fftw3.dll" "fftw3.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Editor.exe" "Editor.exe"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\qt.conf" "qt.conf"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\NOTICE.md" "NOTICE.md"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\LICENSE.txt" "LICENSE.txt"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\install-diagnostics.log" "the installation diagnostics"

  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\FLAC.dll" "FLAC.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\libmp3lame.dll" "libmp3lame.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\mpg123.dll" "mpg123.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\ogg.dll" "ogg.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\opus.dll" "opus.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\vorbis.dll" "vorbis.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\vorbisenc.dll" "vorbisenc.dll"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\vorbisfile.dll" "vorbisfile.dll"

  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\UpdateChecker.exe" "UpdateChecker.exe"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\VoicemeeterClient.exe" "VoicemeeterClient.exe"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\Benchmark.exe" "Benchmark.exe"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\EqApoOutProcHost.exe" "EqApoOutProcHost.exe"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\EqApoJuceVST3Host.dll" "the legacy JUCE VST3 host"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\EqApoOutProcJuceHost.exe" "the legacy JUCE out-of-process host"
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\DeviceSelector.exe" "DeviceSelector.exe"
  !if ${ENABLE_ASIO_PROXY} == 1
    !insertmacro RemoveUninstallPayloadFile "$INSTDIR\${ASIO_PROXY_DLL}" "${ASIO_PROXY_DLL}"
  !endif
  !insertmacro RemoveUninstallPayloadFile "$INSTDIR\EqualizerAPO.dll" "EqualizerAPO.dll"

  ${If} $InstallRecoveryFailed == "1"
    Goto uninstallPayloadCleanupFailed
  ${EndIf}

  ; User configuration is deliberately last: a failed product-file cleanup must
  ; never consume optional user data or the only remaining retry metadata.
  ${If} $RemoveUserConfigurationRequested == "1"
    Call un.RemoveRequestedUserConfiguration
  ${EndIf}

  ClearErrors
  DeleteRegKey HKLM ${UNINST_REGPATH}
  ${If} ${Errors}
    StrCpy $InstallFailureReason "removing Add/Remove Programs metadata"
    Goto uninstallMetadataCleanupFailed
  ${EndIf}
  ClearErrors
  DeleteRegKey HKLM ${REGPATH}
  ${If} ${Errors}
    StrCpy $InstallFailureReason "removing machine-wide product metadata"
    Goto uninstallRestoreRegistrationAndFail
  ${EndIf}
  Goto uninstallFinalize

  uninstallPayloadCleanupFailed:
  DetailPrint "Uninstall retained user data, Uninstall.exe and registry metadata because $InstallFailureReason failed."
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "Uninstall could not finish while $InstallFailureReason.$\r$\n$\r$\nUser configuration, Uninstall.exe and uninstall information were retained so the operation can be retried."
  ${EndIf}
  SetErrorLevel 1
  Abort

  uninstallRestoreRegistrationAndFail:
  ; Product payload is already gone or scheduled. Restore enough registration to
  ; keep this trusted uninstaller discoverable and retryable.
  WriteRegStr HKLM ${REGPATH} "InstallPath" "$INSTDIR"
  WriteRegStr HKLM ${UNINST_REGPATH} "DisplayName" "${PRODUCT_FULL_LABEL}"
  WriteRegStr HKLM ${UNINST_REGPATH} "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM ${UNINST_REGPATH} "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegDWORD HKLM ${UNINST_REGPATH} "NoModify" 1
  WriteRegDWORD HKLM ${UNINST_REGPATH} "NoRepair" 1
  uninstallMetadataCleanupFailed:
  DetailPrint "Uninstall removed product payload but could not finish while $InstallFailureReason."
  ${IfNot} ${Silent}
    MessageBox MB_ICONSTOP|MB_OK "Product payload cleanup completed, but uninstall registration could not finish while $InstallFailureReason. Retry the registered uninstaller after resolving the system error."
  ${EndIf}
  SetErrorLevel 1
  Abort

  uninstallFinalize:
  ; The sibling journal is cleared only after every payload, optional-user-data
  ; and metadata operation succeeded. Keep the trusted uninstaller itself until
  ; that deletion is verified so a journal-cleanup failure remains retryable.
  Call un.ClearUninstallTransaction
  ${If} $InstallRecoveryFailed == "1"
    Goto uninstallRestoreRegistrationAndFail
  ${EndIf}

  ClearErrors
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  ${If} ${Errors}
    ; Journal cleanup happened first, so recreate payload-phase proof before
    ; restoring the registered retry entry. Missing critical helpers remain
    ; legitimate only because all four completion flags are durable again.
    Call un.RestoreCompletedUninstallTransaction
    StrCpy $InstallFailureReason "scheduling Uninstall.exe removal"
    Goto uninstallRestoreRegistrationAndFail
  ${EndIf}

  ; Only remove the root if user configuration and plug-ins did not keep it.
  RMDir /REBOOTOK "$INSTDIR"

SectionEnd

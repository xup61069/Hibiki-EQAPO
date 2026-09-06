; x64 Hibiki EQAPO driver discovery, one-time target selection, and owned
; registration helpers. This file is included only when ENABLE_ASIO_PROXY=1.

!ifndef ASIO_DISCOVERY_HARNESS
LangString AsioPageTitle ${LANG_ENGLISH} "DAW monitoring"
LangString AsioPageTitle ${LANG_SPANISH} "Monitorizacion del DAW"
LangString AsioPageTitle ${LANG_GERMAN} "DAW-Monitoring"
LangString AsioPageTitle ${LANG_TRADCHINESE} "DAW 監聽"
LangString AsioPageTitle ${LANG_SIMPCHINESE} "DAW 监听"
LangString AsioPageSubtitle ${LANG_ENGLISH} "Choose the hardware driver Hibiki EQAPO will wrap."
LangString AsioPageSubtitle ${LANG_SPANISH} "Elija el controlador de hardware que usara Hibiki EQAPO."
LangString AsioPageSubtitle ${LANG_GERMAN} "Wählen Sie den Hardware-Treiber für Hibiki EQAPO."
LangString AsioPageSubtitle ${LANG_TRADCHINESE} "選擇 Hibiki EQAPO 要代理的硬體驅動程式。"
LangString AsioPageSubtitle ${LANG_SIMPCHINESE} "选择 Hibiki EQAPO 要代理的硬件驱动程序。"
LangString AsioPageExplanation ${LANG_ENGLISH} "After setup, select Hibiki EQAPO once in each DAW. Projects then use the same calibrated monitoring path without a plug-in on the master bus."
LangString AsioPageExplanation ${LANG_SPANISH} "Tras instalar, seleccione Hibiki EQAPO una vez en cada DAW. Los proyectos usaran la misma ruta de monitorizacion calibrada sin insertar un plug-in en el bus master."
LangString AsioPageExplanation ${LANG_GERMAN} "Wählen Sie Hibiki EQAPO nach der Installation einmal in jeder DAW. Projekte verwenden danach denselben kalibrierten Monitorpfad ohne Plug-in im Master-Bus."
LangString AsioPageExplanation ${LANG_TRADCHINESE} "安裝後只要在每套 DAW 選一次 Hibiki EQAPO；之後各專案會共用同一條校正監聽路徑，不必在 master bus 掛外掛。"
LangString AsioPageExplanation ${LANG_SIMPCHINESE} "安装后只需在每套 DAW 选择一次 Hibiki EQAPO；之后各项目会共用同一条校准监听路径，不必在 master bus 插入插件。"
LangString AsioPageEnable ${LANG_ENGLISH} "Enable Hibiki EQAPO for DAW monitoring"
LangString AsioPageEnable ${LANG_SPANISH} "Activar Hibiki EQAPO para monitorizacion del DAW"
LangString AsioPageEnable ${LANG_GERMAN} "Hibiki EQAPO für DAW-Monitoring aktivieren"
LangString AsioPageEnable ${LANG_TRADCHINESE} "啟用 Hibiki EQAPO DAW 監聽校正"
LangString AsioPageEnable ${LANG_SIMPCHINESE} "启用 Hibiki EQAPO DAW 监听校准"
LangString AsioPageTarget ${LANG_ENGLISH} "Underlying hardware driver:"
LangString AsioPageTarget ${LANG_SPANISH} "Controlador de hardware subyacente:"
LangString AsioPageTarget ${LANG_GERMAN} "Zugrunde liegender Hardware-Treiber:"
LangString AsioPageTarget ${LANG_TRADCHINESE} "底層硬體驅動程式："
LangString AsioPageTarget ${LANG_SIMPCHINESE} "底层硬件驱动程序："
LangString AsioPageNoDriver ${LANG_ENGLISH} "No eligible 64-bit hardware driver was found. Hibiki EQAPO will not be registered for DAW use."
LangString AsioPageNoDriver ${LANG_SPANISH} "No se encontro un controlador de hardware de 64 bits apto. Hibiki EQAPO no se registrara para el DAW."
LangString AsioPageNoDriver ${LANG_GERMAN} "Kein geeigneter 64-Bit-Hardware-Treiber gefunden. Hibiki EQAPO wird nicht für DAWs registriert."
LangString AsioPageNoDriver ${LANG_TRADCHINESE} "找不到可用的 64 位元硬體驅動程式；本次不會登錄 Hibiki EQAPO 的 DAW 支援。"
LangString AsioPageNoDriver ${LANG_SIMPCHINESE} "找不到可用的 64 位硬件驱动程序；本次不会注册 Hibiki EQAPO 的 DAW 支持。"
LangString AsioPageChooseDriver ${LANG_ENGLISH} "Choose the hardware driver to use, or clear the enable checkbox."
LangString AsioPageChooseDriver ${LANG_SPANISH} "Elija el controlador de hardware o desmarque la casilla de activacion."
LangString AsioPageChooseDriver ${LANG_GERMAN} "Wählen Sie den Hardware-Treiber oder deaktivieren Sie das Kontrollkästchen."
LangString AsioPageChooseDriver ${LANG_TRADCHINESE} "請選擇底層硬體驅動程式，或取消啟用。"
LangString AsioPageChooseDriver ${LANG_SIMPCHINESE} "请选择底层硬件驱动程序，或取消启用。"
!endif

!include "AsioProxyBinaryCheck.nsh"
!include "AsioGuidCheck.nsh"

Function LoadEligibleAsioEntry
  StrCpy $AsioEntryValid "0"
  StrCpy $AsioEntryClsid ""
  StrCpy $AsioEntryName ""
  ClearErrors
  ReadRegStr $AsioEntryClsid HKLM "${ASIO_ENUM_ROOT}\$AsioEntryKey" "CLSID"
  ${If} ${Errors}
  ${OrIf} $AsioEntryClsid == ""
  ${OrIf} $AsioEntryClsid == "${ASIO_PROXY_CLSID}"
    Return
  ${EndIf}
  Push "$AsioEntryClsid"
  Call ValidateAsioGuidText
  Pop $0
  ${If} $0 != "1"
    Return
  ${EndIf}

  ; An enumeration record is useful only when its 64-bit COM server currently
  ; resolves to a real file. The driver repeats stronger PE and file-identity
  ; validation before loading it.
  ClearErrors
  ReadRegStr $1 HKLM "Software\Classes\CLSID\$AsioEntryClsid\InprocServer32" ""
  ${If} ${Errors}
  ${OrIf} $1 == ""
    Return
  ${EndIf}
  ExpandEnvStrings $1 "$1"
  StrCpy $0 "$1" 1
  StrCpy $2 "$1" 1 -1
  ${If} $0 == "$\""
    ${If} $2 != "$\""
      Return
    ${EndIf}
    StrCpy $1 "$1" "" 1
    StrLen $0 "$1"
    IntOp $0 $0 - 1
    StrCpy $1 "$1" $0
  ${ElseIf} $2 == "$\""
    Return
  ${EndIf}
  ; Match the runtime resolver: relative COM server paths depend on the
  ; installer's current directory and therefore are not stable ASIO targets.
  System::Call 'shlwapi::PathIsRelativeW(w r1) i .r0'
  ${If} $0 != 0
    Return
  ${EndIf}
  ${IfNot} ${FileExists} "$1"
    Return
  ${EndIf}
  StrCpy $AsioBinaryPath "$1"
  Call ValidateAmd64AsioDriverBinary
  ${If} $AsioBinaryValid != "1"
    Return
  ${EndIf}

  ClearErrors
  ReadRegStr $AsioEntryName HKLM "${ASIO_ENUM_ROOT}\$AsioEntryKey" "Description"
  ${If} ${Errors}
  ${OrIf} $AsioEntryName == ""
    StrCpy $AsioEntryName "$AsioEntryKey"
  ${EndIf}
  StrCpy $AsioEntryValid "1"
FunctionEnd

Function DiscoverEligibleAsioTargets
  StrCpy $AsioCandidateCount "0"
  StrCpy $AsioSingleCandidateClsid ""
  StrCpy $3 "0"

  asioDiscoverNext:
  ; EnumRegKey can return an empty name at end-of-enumeration without leaving
  ; the NSIS error flag set. The empty sentinel is therefore mandatory.
  StrCpy $AsioEntryKey ""
  ClearErrors
  EnumRegKey $AsioEntryKey HKLM "${ASIO_ENUM_ROOT}" $3
  ${If} ${Errors}
  ${OrIf} $AsioEntryKey == ""
    Goto asioDiscoverDone
  ${EndIf}
  Call LoadEligibleAsioEntry
  ${If} $AsioEntryValid != "1"
    Goto asioDiscoverAdvance
  ${EndIf}
  StrCpy $7 "$AsioEntryClsid"

  ; Some vendors publish aliases that share one CLSID. Count a physical driver
  ; choice once so an alias cannot turn the deterministic one-driver case into
  ; a false ambiguity.
  StrCpy $4 "0"
  asioDiscoverPrior:
  ${If} $4 >= $3
    Goto asioDiscoverUnique
  ${EndIf}
  StrCpy $5 ""
  ClearErrors
  EnumRegKey $5 HKLM "${ASIO_ENUM_ROOT}" $4
  ${If} ${Errors}
  ${OrIf} $5 == ""
    Goto asioDiscoverUnique
  ${EndIf}
  StrCpy $AsioEntryKey "$5"
  Call LoadEligibleAsioEntry
  ${If} $AsioEntryValid == "1"
  ${AndIf} $AsioEntryClsid == $7
    Goto asioDiscoverAdvance
  ${EndIf}
  IntOp $4 $4 + 1
  Goto asioDiscoverPrior

  asioDiscoverUnique:
  IntOp $AsioCandidateCount $AsioCandidateCount + 1
  ${If} $AsioCandidateCount == 1
    StrCpy $AsioSingleCandidateClsid "$7"
  ${Else}
    StrCpy $AsioSingleCandidateClsid ""
  ${EndIf}

  asioDiscoverAdvance:
  IntOp $3 $3 + 1
  Goto asioDiscoverNext

  asioDiscoverDone:
FunctionEnd

Function ValidateAsioTargetSelection
  StrCpy $AsioTargetValid "0"
  StrCpy $AsioSelectedTargetName ""
  ${If} $AsioSelectedTargetClsid == ""
  ${OrIf} $AsioSelectedTargetClsid == "${ASIO_PROXY_CLSID}"
    Return
  ${EndIf}
  StrCpy $3 "0"

  asioValidateTargetNext:
  StrCpy $AsioEntryKey ""
  ClearErrors
  EnumRegKey $AsioEntryKey HKLM "${ASIO_ENUM_ROOT}" $3
  ${If} ${Errors}
  ${OrIf} $AsioEntryKey == ""
    Return
  ${EndIf}
  Call LoadEligibleAsioEntry
  ${If} $AsioEntryValid == "1"
  ${AndIf} $AsioEntryClsid == $AsioSelectedTargetClsid
    ; Keep the canonical spelling read from the vendor entry.
    StrCpy $AsioSelectedTargetClsid "$AsioEntryClsid"
    StrCpy $AsioSelectedTargetName "$AsioEntryName"
    StrCpy $AsioTargetValid "1"
    Return
  ${EndIf}
  IntOp $3 $3 + 1
  Goto asioValidateTargetNext
FunctionEnd

Function ResolveInitialAsioSelection
  StrCpy $AsioProxyRequested "0"
  StrCpy $AsioSelectedTargetClsid ""
  StrCpy $AsioSelectedTargetName ""
  StrCpy $AsioCommandLineTargetClsid ""
  StrCpy $AsioNoProxyOption "0"
  Call DiscoverEligibleAsioTargets

  ${GetParameters} $0
  StrCpy $1 ""
  ClearErrors
  ${GetOptions} "$0" "/NOASIOPROXY" $1
  ${IfNot} ${Errors}
    StrCpy $AsioNoProxyOption "1"
  ${EndIf}
  ClearErrors
  ${GetOptions} "$0" "/ASIOCLSID=" $AsioCommandLineTargetClsid
  ${If} ${Errors}
    StrCpy $AsioCommandLineTargetClsid ""
  ${EndIf}

  ${If} $AsioNoProxyOption == "1"
  ${AndIf} $AsioCommandLineTargetClsid != ""
    StrCpy $InstallFailureReason "/NOASIOPROXY and /ASIOCLSID cannot be used together"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ${If} $AsioNoProxyOption == "1"
    Return
  ${EndIf}
  ${If} $AsioCommandLineTargetClsid != ""
    StrCpy $AsioSelectedTargetClsid "$AsioCommandLineTargetClsid"
    Call ValidateAsioTargetSelection
    ${If} $AsioTargetValid != "1"
      StrCpy $InstallFailureReason "validating the explicit /ASIOCLSID target"
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
    StrCpy $AsioProxyRequested "1"
    Return
  ${EndIf}

  ; Preserve an existing valid machine default. Per-user HKCU overrides are
  ; deliberately not read by elevated setup because over-the-shoulder UAC may
  ; run under a different account; the driver reads HKCU first at runtime.
  ClearErrors
  ReadRegStr $AsioSelectedTargetClsid HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"
  ${IfNot} ${Errors}
  ${AndIf} $AsioSelectedTargetClsid != ""
    Call ValidateAsioTargetSelection
    ${If} $AsioTargetValid == "1"
      StrCpy $AsioProxyRequested "1"
      Return
    ${EndIf}
  ${EndIf}
  StrCpy $AsioSelectedTargetClsid ""
  StrCpy $AsioSelectedTargetName ""

  ${If} $AsioCandidateCount == 1
    StrCpy $AsioSelectedTargetClsid "$AsioSingleCandidateClsid"
    Call ValidateAsioTargetSelection
    ${If} $AsioTargetValid == "1"
      StrCpy $AsioProxyRequested "1"
    ${EndIf}
  ${ElseIf} $AsioCandidateCount > 1
    ; Interactive setup will require an explicit choice. Silent setup is
    ; deterministic and leaves the proxy unregistered rather than guessing.
    ${IfNot} ${Silent}
      StrCpy $AsioProxyRequested "1"
    ${EndIf}
  ${EndIf}
FunctionEnd

!ifndef ASIO_DISCOVERY_HARNESS
Function PopulateAsioTargetCombo
  SendMessage $AsioTargetCombo ${CB_RESETCONTENT} 0 0
  SendMessage $AsioTargetCombo ${CB_SETCURSEL} -1 0
  StrCpy $3 "0"
  StrCpy $6 "0"

  asioPopulateNext:
  StrCpy $AsioEntryKey ""
  ClearErrors
  EnumRegKey $AsioEntryKey HKLM "${ASIO_ENUM_ROOT}" $3
  ${If} ${Errors}
  ${OrIf} $AsioEntryKey == ""
    Goto asioPopulateDone
  ${EndIf}
  Call LoadEligibleAsioEntry
  ${If} $AsioEntryValid != "1"
    Goto asioPopulateAdvance
  ${EndIf}
  StrCpy $7 "$AsioEntryClsid"
  StrCpy $8 "$AsioEntryName"
  StrCpy $4 "0"
  asioPopulatePrior:
  ${If} $4 >= $3
    Goto asioPopulateUnique
  ${EndIf}
  StrCpy $5 ""
  ClearErrors
  EnumRegKey $5 HKLM "${ASIO_ENUM_ROOT}" $4
  ${If} ${Errors}
  ${OrIf} $5 == ""
    Goto asioPopulateUnique
  ${EndIf}
  StrCpy $AsioEntryKey "$5"
  Call LoadEligibleAsioEntry
  ${If} $AsioEntryValid == "1"
  ${AndIf} $AsioEntryClsid == $7
    Goto asioPopulateAdvance
  ${EndIf}
  IntOp $4 $4 + 1
  Goto asioPopulatePrior

  asioPopulateUnique:
  StrCpy $9 "$8 — $7"
  SendMessage $AsioTargetCombo ${CB_ADDSTRING} 0 "STR:$9"
  ${If} $7 == $AsioSelectedTargetClsid
    SendMessage $AsioTargetCombo ${CB_SETCURSEL} $6 0
  ${EndIf}
  IntOp $6 $6 + 1

  asioPopulateAdvance:
  IntOp $3 $3 + 1
  Goto asioPopulateNext

  asioPopulateDone:
FunctionEnd

Function AsioProxyEnableChanged
  ${NSD_GetState} $AsioProxyEnableCheckbox $0
  ${If} $0 == ${BST_CHECKED}
  ${AndIf} $AsioCandidateCount > 0
    EnableWindow $AsioTargetCombo 1
  ${Else}
    EnableWindow $AsioTargetCombo 0
  ${EndIf}
FunctionEnd

Function AsioProxyPageCreate
  !insertmacro MUI_HEADER_TEXT "$(AsioPageTitle)" "$(AsioPageSubtitle)"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 31u "$(AsioPageExplanation)"
  Pop $0
  ${NSD_CreateCheckbox} 0 39u 100% 12u "$(AsioPageEnable)"
  Pop $AsioProxyEnableCheckbox
  ${NSD_CreateLabel} 0 59u 100% 12u "$(AsioPageTarget)"
  Pop $0
  ${NSD_CreateDropList} 0 74u 100% 120u ""
  Pop $AsioTargetCombo
  ${NSD_CreateLabel} 0 101u 100% 32u ""
  Pop $AsioTargetStatusLabel

  Call DiscoverEligibleAsioTargets
  Call PopulateAsioTargetCombo
  ${If} $AsioCandidateCount == 0
    StrCpy $AsioProxyRequested "0"
    StrCpy $AsioSelectedTargetClsid ""
    ${NSD_Uncheck} $AsioProxyEnableCheckbox
    EnableWindow $AsioProxyEnableCheckbox 0
    EnableWindow $AsioTargetCombo 0
    ${NSD_SetText} $AsioTargetStatusLabel "$(AsioPageNoDriver)"
  ${ElseIf} $AsioProxyRequested == "1"
    ${NSD_Check} $AsioProxyEnableCheckbox
  ${Else}
    ${NSD_Uncheck} $AsioProxyEnableCheckbox
  ${EndIf}
  ${NSD_OnClick} $AsioProxyEnableCheckbox AsioProxyEnableChanged
  Call AsioProxyEnableChanged
  nsDialogs::Show
FunctionEnd

Function AsioProxyPageLeave
  ${NSD_GetState} $AsioProxyEnableCheckbox $0
  ${If} $0 != ${BST_CHECKED}
    StrCpy $AsioProxyRequested "0"
    StrCpy $AsioSelectedTargetClsid ""
    StrCpy $AsioSelectedTargetName ""
    Return
  ${EndIf}

  ${NSD_GetText} $AsioTargetCombo $1
  StrLen $2 "$1"
  ${If} $2 < 38
    MessageBox MB_ICONEXCLAMATION|MB_OK "$(AsioPageChooseDriver)"
    Abort
  ${EndIf}
  StrCpy $AsioSelectedTargetClsid "$1" 38 -38
  Call ValidateAsioTargetSelection
  ${If} $AsioTargetValid != "1"
    MessageBox MB_ICONEXCLAMATION|MB_OK "$(AsioPageChooseDriver)"
    Abort
  ${EndIf}
  StrCpy $AsioProxyRequested "1"
FunctionEnd

Function CheckAsioProxyDllUnlocked
  StrCpy $InstallOperationCode "0"
  ${IfNot} ${FileExists} "$INSTDIR\${ASIO_PROXY_DLL}"
    Return
  ${EndIf}
  System::Call 'kernel32::CreateFileW(w "$INSTDIR\${ASIO_PROXY_DLL}", i ${DELETE_ACCESS}, i 0, p 0, i ${OPEN_EXISTING}, i ${FILE_FLAG_OPEN_REPARSE_POINT}, p 0) p .r0 ?e'
  Pop $1
  ${If} $0 == ${INVALID_HANDLE_VALUE}
    StrCpy $InstallOperationCode "$1"
    Return
  ${EndIf}
  System::Call 'kernel32::CloseHandle(p r0) i .r1'
  ${If} $1 == 0
    StrCpy $InstallOperationCode "handle close failed"
  ${EndIf}
FunctionEnd

Function ValidateAsioSnapshotRegistryTypes
  StrCpy $InstallRecoveryFailed "0"

  ; Read only the registry metadata here. ReadRegStr accepts REG_EXPAND_SZ,
  ; which would otherwise be journaled and restored as REG_SZ. Refuse such an
  ; install before the journal or any public registration value is changed.
  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_ENUM_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "CLSID", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto asioSnapshotEnumCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto asioSnapshotEnumCloseAndFail
    ${EndIf}
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "Description", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto asioSnapshotEnumCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto asioSnapshotEnumCloseAndFail
    ${EndIf}
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto asioSnapshotTypeFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto asioSnapshotTypeFailed
  ${EndIf}

  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_COM_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto asioSnapshotComCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto asioSnapshotComCloseAndFail
    ${EndIf}
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto asioSnapshotTypeFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto asioSnapshotTypeFailed
  ${EndIf}

  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_COM_REGPATH}\InprocServer32", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto asioSnapshotInprocCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto asioSnapshotInprocCloseAndFail
    ${EndIf}
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "ThreadingModel", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto asioSnapshotInprocCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto asioSnapshotInprocCloseAndFail
    ${EndIf}
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto asioSnapshotTypeFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto asioSnapshotTypeFailed
  ${EndIf}

  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_TARGET_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "TargetCLSID", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto asioSnapshotTargetCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto asioSnapshotTargetCloseAndFail
    ${EndIf}
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto asioSnapshotTypeFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto asioSnapshotTypeFailed
  ${EndIf}
  Return

  asioSnapshotEnumCloseAndFail:
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  Goto asioSnapshotTypeFailed
  asioSnapshotComCloseAndFail:
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  Goto asioSnapshotTypeFailed
  asioSnapshotInprocCloseAndFail:
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  Goto asioSnapshotTypeFailed
  asioSnapshotTargetCloseAndFail:
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  asioSnapshotTypeFailed:
  StrCpy $InstallFailureReason "validating the registry value types before journaling the Hibiki EQAPO driver registration"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function ValidateExistingAsioProxyRegistration
  StrCpy $InstallRecoveryFailed "0"
  StrCpy $2 "0"
  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_ENUM_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 "1"
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto asioExistingCollision
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto asioExistingCollision
  ${EndIf}
  ${If} $2 == "1"
    ClearErrors
    ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
    ${If} ${Errors}
    ${OrIf} $0 != "${ASIO_PROXY_CLSID}"
      Goto asioExistingCollision
    ${EndIf}
    ClearErrors
    ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description"
    ${If} ${Errors}
    ${OrIf} $0 != "${ASIO_PROXY_LABEL}"
      Goto asioExistingCollision
    ${EndIf}
  ${EndIf}

  StrCpy $3 "0"
  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_COM_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $3 "1"
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto asioExistingCollision
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto asioExistingCollision
  ${EndIf}
  ${If} $3 == "1"
    ClearErrors
    ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}" ""
    ${If} ${Errors}
    ${OrIf} $0 != "${ASIO_PROXY_LABEL}"
      Goto asioExistingCollision
    ${EndIf}
    ClearErrors
    ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" ""
    ${If} ${Errors}
      Goto asioExistingCollision
    ${EndIf}
    ExpandEnvStrings $0 "$0"
    StrCpy $1 "$0" 1
    StrCpy $4 "$0" 1 -1
    ${If} $1 == "$\""
      ${If} $4 != "$\""
        Goto asioExistingCollision
      ${EndIf}
      StrCpy $0 "$0" "" 1
      StrLen $1 "$0"
      IntOp $1 $1 - 1
      StrCpy $0 "$0" $1
    ${ElseIf} $4 == "$\""
      Goto asioExistingCollision
    ${EndIf}
    ClearErrors
    GetFullPathName $1 "$INSTDIR\${ASIO_PROXY_DLL}"
    ${If} ${Errors}
    ${OrIf} $1 == ""
    ${OrIf} $0 != $1
      Goto asioExistingCollision
    ${EndIf}
    ClearErrors
    ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel"
    ${If} ${Errors}
    ${OrIf} $0 != "Apartment"
      Goto asioExistingCollision
    ${EndIf}
  ${EndIf}
  ; A complete owned registration has both keys. A single orphaned public key
  ; is not adopted without a durable transaction proving how it was created.
  ${If} $2 != $3
    Goto asioExistingCollision
  ${EndIf}
  Call ValidateAsioSnapshotRegistryTypes
  Return

  asioExistingCollision:
  StrCpy $InstallFailureReason "an existing ASIO/COM registration at the Hibiki EQAPO identity is not owned by this installation"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function RemoveOwnedAsioProxyRegistration
  Call ValidateExistingAsioProxyRegistration
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ; CLSID is the host's activation pointer, so remove it first.
  DeleteRegValue HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
  DeleteRegValue HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_ENUM_REGPATH}"
  DeleteRegValue HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel"
  DeleteRegValue HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" ""
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32"
  DeleteRegValue HKLM "${ASIO_PROXY_COM_REGPATH}" ""
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_COM_REGPATH}"
  DeleteRegValue HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_TARGET_REGPATH}"

  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
  ${IfNot} ${Errors}
    StrCpy $InstallFailureReason "removing the Hibiki EQAPO driver enumeration"
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" ""
  ${IfNot} ${Errors}
    StrCpy $InstallFailureReason "removing the Hibiki EQAPO COM registration"
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"
  ${IfNot} ${Errors}
    StrCpy $InstallFailureReason "removing the Hibiki EQAPO hardware-driver target"
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function ApplyAsioProxyRegistration
  StrCpy $InstallRecoveryFailed "0"
  Call ValidateExistingAsioProxyRegistration
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  ; This durable bit precedes the first external registry mutation. Recovery can
  ; therefore restore the exact values journaled by SaveInstallMetadataJournal.
  StrCpy $NewAsioRegAttempted "1"
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewAsioRegistrationAttempted" 1
  ${If} ${Errors}
    Goto asioApplyFailed
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewAsioRegistrationAttempted"
  ${If} ${Errors}
  ${OrIf} $0 != 1
    Goto asioApplyFailed
  ${EndIf}
  Call FlushRenameCleanupJournal
  ${If} $InstallRecoveryFailed == "1"
    Goto asioApplyFailed
  ${EndIf}

  ${If} $AsioProxyRequested != "1"
    Call RemoveOwnedAsioProxyRegistration
    Return
  ${EndIf}
  Call ValidateAsioTargetSelection
  ${If} $AsioTargetValid != "1"
    StrCpy $InstallFailureReason "validating the selected hardware driver immediately before registration"
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}

  ; Publish the enumeration key last so a host can never discover a COM server
  ; before its target and InprocServer32 values are complete.
  ClearErrors
  WriteRegStr HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID" "$AsioSelectedTargetClsid"
  ${If} ${Errors}
    Goto asioApplyFailed
  ${EndIf}
  WriteRegStr HKLM "${ASIO_PROXY_COM_REGPATH}" "" "${ASIO_PROXY_LABEL}"
  ${If} ${Errors}
    Goto asioApplyFailed
  ${EndIf}
  WriteRegStr HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "" "$INSTDIR\${ASIO_PROXY_DLL}"
  ${If} ${Errors}
    Goto asioApplyFailed
  ${EndIf}
  WriteRegStr HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel" "Apartment"
  ${If} ${Errors}
    Goto asioApplyFailed
  ${EndIf}
  WriteRegStr HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description" "${ASIO_PROXY_LABEL}"
  ${If} ${Errors}
    Goto asioApplyFailed
  ${EndIf}
  ; CLSID is the discoverability/activation pointer and must be last.
  WriteRegStr HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID" "${ASIO_PROXY_CLSID}"
  ${If} ${Errors}
    Goto asioApplyFailed
  ${EndIf}

  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"
  ${If} ${Errors}
  ${OrIf} $0 != $AsioSelectedTargetClsid
    Goto asioApplyFailed
  ${EndIf}
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" ""
  ${If} ${Errors}
  ${OrIf} $0 != "$INSTDIR\${ASIO_PROXY_DLL}"
    Goto asioApplyFailed
  ${EndIf}
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel"
  ${If} ${Errors}
  ${OrIf} $0 != "Apartment"
    Goto asioApplyFailed
  ${EndIf}
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}" ""
  ${If} ${Errors}
  ${OrIf} $0 != "${ASIO_PROXY_LABEL}"
    Goto asioApplyFailed
  ${EndIf}
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
  ${If} ${Errors}
  ${OrIf} $0 != "${ASIO_PROXY_CLSID}"
    Goto asioApplyFailed
  ${EndIf}
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description"
  ${If} ${Errors}
  ${OrIf} $0 != "${ASIO_PROXY_LABEL}"
    Goto asioApplyFailed
  ${EndIf}
  Return

  asioApplyFailed:
  StrCpy $InstallFailureReason "writing or verifying the Hibiki EQAPO driver registration"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function HideAsioProxyRegistrationForRollback
  StrCpy $InstallRecoveryFailed "0"
  ; Missing is valid after a crash during an earlier rollback attempt. A value
  ; owned by somebody else at the fixed public key is never removed.
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
  ${IfNot} ${Errors}
    ${If} $0 != "${ASIO_PROXY_CLSID}"
      StrCpy $InstallFailureReason "validating ownership before hiding the Hibiki EQAPO driver registration"
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
    ClearErrors
    DeleteRegValue HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
    ${If} ${Errors}
      StrCpy $InstallFailureReason "hiding the Hibiki EQAPO driver registration for rollback"
      StrCpy $InstallRecoveryFailed "1"
      Return
    ${EndIf}
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
  ${IfNot} ${Errors}
    StrCpy $InstallFailureReason "verifying that the Hibiki EQAPO driver is hidden for rollback"
    StrCpy $InstallRecoveryFailed "1"
  ${EndIf}
FunctionEnd

Function RestoreAsioProxyRegistration
  StrCpy $InstallRecoveryFailed "0"
  Call HideAsioProxyRegistrationForRollback
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ; Restore non-discoverable state first. InprocServer32 and the enumeration
  ; CLSID are each activation pointers, so they are restored last in their key.
  !insertmacro RestorePreviousString HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID" "PreviousAsioTargetClsid"
  !insertmacro RestorePreviousString HKLM "${ASIO_PROXY_COM_REGPATH}" "" "PreviousAsioComDescription"
  !insertmacro RestorePreviousString HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel" "PreviousAsioThreadingModel"
  !insertmacro RestorePreviousString HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "" "PreviousAsioInprocServer"
  !insertmacro RestorePreviousString HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description" "PreviousAsioEnumDescription"
  !insertmacro RestorePreviousString HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID" "PreviousAsioEnumClsid"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_ENUM_REGPATH}"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_COM_REGPATH}"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_TARGET_REGPATH}"
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}

  StrCpy $NewAsioRegAttempted "0"
  ClearErrors
  WriteRegDWORD HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewAsioRegistrationAttempted" 0
  ${If} ${Errors}
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  ClearErrors
  ReadRegDWORD $0 HKLM ${INSTALLER_APP_RECOVERY_REGPATH} "NewAsioRegistrationAttempted"
  ${If} ${Errors}
  ${OrIf} $0 != 0
    StrCpy $InstallRecoveryFailed "1"
    Return
  ${EndIf}
  Call FlushRenameCleanupJournal
FunctionEnd

Function un.CheckAsioProxyDllUnlocked
  StrCpy $InstallOperationCode "0"
  ${IfNot} ${FileExists} "$INSTDIR\${ASIO_PROXY_DLL}"
    Return
  ${EndIf}
  System::Call 'kernel32::CreateFileW(w "$INSTDIR\${ASIO_PROXY_DLL}", i ${DELETE_ACCESS}, i 0, p 0, i ${OPEN_EXISTING}, i ${FILE_FLAG_OPEN_REPARSE_POINT}, p 0) p .r0 ?e'
  Pop $1
  ${If} $0 == ${INVALID_HANDLE_VALUE}
    StrCpy $InstallOperationCode "$1"
    Return
  ${EndIf}
  System::Call 'kernel32::CloseHandle(p r0) i .r1'
  ${If} $1 == 0
    StrCpy $InstallOperationCode "handle close failed"
  ${EndIf}
FunctionEnd

Function un.ValidateAsioRegistryTypesForRemoval
  StrCpy $InstallRecoveryFailed "0"

  ; Uninstall may resume after a partial removal, so missing keys and values are
  ; valid. Any known value which remains must have the REG_SZ type we own.
  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_ENUM_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "CLSID", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto unAsioTypesEnumCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto unAsioTypesEnumCloseAndFail
    ${EndIf}
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "Description", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto unAsioTypesEnumCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto unAsioTypesEnumCloseAndFail
    ${EndIf}
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto unAsioTypesFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto unAsioTypesFailed
  ${EndIf}

  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_COM_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto unAsioTypesComCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto unAsioTypesComCloseAndFail
    ${EndIf}
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto unAsioTypesFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto unAsioTypesFailed
  ${EndIf}

  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_COM_REGPATH}\InprocServer32", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto unAsioTypesInprocCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto unAsioTypesInprocCloseAndFail
    ${EndIf}
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "ThreadingModel", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto unAsioTypesInprocCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto unAsioTypesInprocCloseAndFail
    ${EndIf}
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto unAsioTypesFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto unAsioTypesFailed
  ${EndIf}

  System::Call 'advapi32::RegOpenKeyExW(p ${WIN32_HKEY_LOCAL_MACHINE}, w "${ASIO_PROXY_TARGET_REGPATH}", i 0, i ${KEY_QUERY_VALUE_64}, *p .r0) i .r1'
  ${If} $1 == 0
    StrCpy $2 0
    StrCpy $3 0
    System::Call 'advapi32::RegQueryValueExW(p r0, w "TargetCLSID", p 0, *i .r2, p 0, *i .r3) i .r1'
    ${If} $1 == 0
      ${If} $2 != ${REG_SZ}
        Goto unAsioTypesTargetCloseAndFail
      ${EndIf}
    ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
      Goto unAsioTypesTargetCloseAndFail
    ${EndIf}
    System::Call 'advapi32::RegCloseKey(p r0) i .r1'
    ${If} $1 != 0
      Goto unAsioTypesFailed
    ${EndIf}
  ${ElseIf} $1 != ${ERROR_FILE_NOT_FOUND}
  ${AndIf} $1 != ${ERROR_PATH_NOT_FOUND}
    Goto unAsioTypesFailed
  ${EndIf}
  Return

  unAsioTypesEnumCloseAndFail:
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  Goto unAsioTypesFailed
  unAsioTypesComCloseAndFail:
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  Goto unAsioTypesFailed
  unAsioTypesInprocCloseAndFail:
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  Goto unAsioTypesFailed
  unAsioTypesTargetCloseAndFail:
  System::Call 'advapi32::RegCloseKey(p r0) i .r1'
  unAsioTypesFailed:
  StrCpy $InstallFailureReason "validating the Hibiki EQAPO driver registry value types before removal"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd

Function un.UnregisterAsioProxy
  StrCpy $InstallRecoveryFailed "0"
  Call un.ValidateAsioRegistryTypesForRemoval
  ${If} $InstallRecoveryFailed == "1"
    Return
  ${EndIf}
  ; Missing values are accepted so a crash halfway through this function is
  ; idempotently retryable. Every value which remains must still be ours; extra
  ; foreign values and non-empty keys are deliberately retained.
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
  ${IfNot} ${Errors}
  ${AndIf} $0 != "${ASIO_PROXY_CLSID}"
    Goto unAsioOwnershipFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description"
  ${IfNot} ${Errors}
  ${AndIf} $0 != "${ASIO_PROXY_LABEL}"
    Goto unAsioOwnershipFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}" ""
  ${IfNot} ${Errors}
  ${AndIf} $0 != "${ASIO_PROXY_LABEL}"
    Goto unAsioOwnershipFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" ""
  ${IfNot} ${Errors}
    ExpandEnvStrings $0 "$0"
    StrCpy $2 "$0" 1
    StrCpy $3 "$0" 1 -1
    ${If} $2 == "$\""
      ${If} $3 != "$\""
        Goto unAsioOwnershipFailed
      ${EndIf}
      StrCpy $0 "$0" "" 1
      StrLen $2 "$0"
      IntOp $2 $2 - 1
      StrCpy $0 "$0" $2
    ${ElseIf} $3 == "$\""
      Goto unAsioOwnershipFailed
    ${EndIf}
    ClearErrors
    GetFullPathName $1 "$INSTDIR\${ASIO_PROXY_DLL}"
    ${If} ${Errors}
    ${OrIf} $1 == ""
    ${OrIf} $0 != $1
      Goto unAsioOwnershipFailed
    ${EndIf}
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel"
  ${IfNot} ${Errors}
  ${AndIf} $0 != "Apartment"
    Goto unAsioOwnershipFailed
  ${EndIf}

  ; Unpublish before removing the COM activation pointer and descriptive values.
  DeleteRegValue HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
  DeleteRegValue HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_ENUM_REGPATH}"
  DeleteRegValue HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" ""
  DeleteRegValue HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32"
  DeleteRegValue HKLM "${ASIO_PROXY_COM_REGPATH}" ""
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_COM_REGPATH}"
  DeleteRegValue HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"
  DeleteRegKey /ifempty HKLM "${ASIO_PROXY_TARGET_REGPATH}"
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "CLSID"
  ${IfNot} ${Errors}
    Goto unAsioRemovalFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_ENUM_REGPATH}" "Description"
  ${IfNot} ${Errors}
    Goto unAsioRemovalFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}" ""
  ${IfNot} ${Errors}
    Goto unAsioRemovalFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" ""
  ${IfNot} ${Errors}
    Goto unAsioRemovalFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_COM_REGPATH}\InprocServer32" "ThreadingModel"
  ${IfNot} ${Errors}
    Goto unAsioRemovalFailed
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKLM "${ASIO_PROXY_TARGET_REGPATH}" "TargetCLSID"
  ${IfNot} ${Errors}
    Goto unAsioRemovalFailed
  ${EndIf}
  Return

  unAsioOwnershipFailed:
  StrCpy $InstallFailureReason "validating ownership of the Hibiki EQAPO driver registration"
  StrCpy $InstallRecoveryFailed "1"
  Return
  unAsioRemovalFailed:
  StrCpy $InstallFailureReason "removing the Hibiki EQAPO driver registration"
  StrCpy $InstallRecoveryFailed "1"
FunctionEnd
!endif

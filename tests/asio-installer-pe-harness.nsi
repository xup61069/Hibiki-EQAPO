Unicode true
SilentInstall silent
RequestExecutionLevel user

!ifndef HARNESS_OUTPUT
  !error "HARNESS_OUTPUT must point to the harness executable"
!endif

OutFile "${HARNESS_OUTPUT}"

!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define IMAGE_FILE_MACHINE_AMD64 34404 ; 0x8664
!define IMAGE_FILE_DLL 8192 ; 0x2000

Var AsioBinaryPath
Var AsioBinaryValid
Var AsioBinaryFailure
Var HarnessResultPath
Var HarnessParameters

!include "..\Setup\AsioProxyBinaryCheck.nsh"

Section
  ${GetParameters} $0
  StrCpy $HarnessParameters "$0"
  ClearErrors
  ${GetOptions} "$0" "/INPUT=" $AsioBinaryPath
  ${If} ${Errors}
  ${OrIf} $AsioBinaryPath == ""
    SetErrorLevel 2
    Return
  ${EndIf}
  StrCpy $0 "scratch-0"
  StrCpy $1 "scratch-1"
  StrCpy $2 "scratch-2"
  StrCpy $3 "scratch-3"
  StrCpy $4 "scratch-4"
  StrCpy $5 "scratch-5"
  StrCpy $6 "scratch-6"
  StrCpy $7 "scratch-7"
  StrCpy $8 "scratch-8"
  StrCpy $9 "scratch-9"
  Call ValidateAmd64AsioDriverBinary
  ${If} $0 != "scratch-0"
  ${OrIf} $1 != "scratch-1"
  ${OrIf} $2 != "scratch-2"
  ${OrIf} $3 != "scratch-3"
  ${OrIf} $4 != "scratch-4"
  ${OrIf} $5 != "scratch-5"
  ${OrIf} $6 != "scratch-6"
  ${OrIf} $7 != "scratch-7"
  ${OrIf} $8 != "scratch-8"
  ${OrIf} $9 != "scratch-9"
    SetErrorLevel 3
    Return
  ${EndIf}
  ClearErrors
  ${GetOptions} "$HarnessParameters" "/RESULT=" $HarnessResultPath
  ${IfNot} ${Errors}
  ${AndIf} $HarnessResultPath != ""
    ClearErrors
    FileOpen $1 "$HarnessResultPath" w
    ${IfNot} ${Errors}
      FileWrite $1 "$AsioBinaryFailure"
      FileClose $1
    ${EndIf}
  ${EndIf}
  ${If} $AsioBinaryValid == "1"
    SetErrorLevel 0
  ${Else}
    SetErrorLevel 1
  ${EndIf}
SectionEnd

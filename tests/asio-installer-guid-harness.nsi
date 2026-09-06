Unicode true
SilentInstall silent
RequestExecutionLevel user

!ifndef HARNESS_OUTPUT
  !error "HARNESS_OUTPUT must point to the harness executable"
!endif

OutFile "${HARNESS_OUTPUT}"

!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "..\Setup\AsioGuidCheck.nsh"

Section
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} "$0" "/CLSID=" $1
  ${If} ${Errors}
    SetErrorLevel 2
    Return
  ${EndIf}
  Push "$1"
  Call ValidateAsioGuidText
  Pop $0
  ${If} $0 == "1"
    SetErrorLevel 0
  ${Else}
    SetErrorLevel 1
  ${EndIf}
SectionEnd

; Validate one braced CLSID string without touching the registry.
; Input: one string on the NSIS stack. Output: "1" for a Windows-parseable
; GUID, otherwise "0". All general scratch registers are restored.
Function ValidateAsioGuidText
  Exch $0
  Push $1
  Push $2
  StrCpy $2 "0"

  StrLen $1 "$0"
  ${If} $1 != 38
    Goto asioGuidDone
  ${EndIf}
  StrCpy $1 "$0" 1
  ${If} $1 != "{"
    Goto asioGuidDone
  ${EndIf}
  StrCpy $1 "$0" 1 -1
  ${If} $1 != "}"
    Goto asioGuidDone
  ${EndIf}

  ; CLSIDFromString rejects non-hex digits and malformed separators that a
  ; length/braces-only check would otherwise publish as an ASIO target.
  System::Call 'ole32::CLSIDFromString(w "$0", g .r1) i.r2'
  ${If} $2 == 0
    StrCpy $2 "1"
  ${Else}
    StrCpy $2 "0"
  ${EndIf}

  asioGuidDone:
  StrCpy $0 "$2"
  Pop $2
  Pop $1
  Exch $0
FunctionEnd

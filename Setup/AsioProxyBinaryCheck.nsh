; Shared by the production installer and the non-elevated runtime harness.
; Callers define IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_DLL, AsioBinaryPath and
; AsioBinaryValid before including this file.
Function ValidateAmd64AsioDriverBinary
  ; Parse only the DOS/COFF headers. Loading an untrusted vendor DLL in the
  ; elevated installer merely to learn its architecture would execute code in
  ; DllMain. The runtime driver repeats this validation before loading it.
  Push $0
  Push $1
  Push $2
  Push $3
  Push $4
  Push $5
  Push $6
  Push $7
  Push $8
  Push $9
  StrCpy $AsioBinaryValid "0"
  StrCpy $AsioBinaryFailure "open"
  ClearErrors
  FileOpen $0 "$AsioBinaryPath" r
  ${If} ${Errors}
    Goto asioBinaryDone
  ${EndIf}

  StrCpy $AsioBinaryFailure "file-size"
  ClearErrors
  FileSeek $0 0 END $1
  ${If} ${Errors}
    Goto asioBinaryInvalid
  ${ElseIf} $1 < 88
    Goto asioBinaryInvalid
  ${EndIf}
  StrCpy $AsioBinaryFailure "dos-signature"
  ClearErrors
  FileSeek $0 0 SET $2
  FileReadByte $0 $2
  FileReadByte $0 $3
  ${If} ${Errors}
  ${OrIf} $2 != 77
  ${OrIf} $3 != 90
    Goto asioBinaryInvalid
  ${EndIf}

  StrCpy $AsioBinaryFailure "pe-offset"
  ClearErrors
  FileSeek $0 60 SET $2
  FileReadByte $0 $2
  FileReadByte $0 $3
  FileReadByte $0 $4
  FileReadByte $0 $5
  ${If} ${Errors}
    Goto asioBinaryInvalid
  ${EndIf}
  IntOp $6 $3 << 8
  IntOp $7 $4 << 16
  IntOp $8 $5 << 24
  IntOp $9 $2 + $6
  IntOp $9 $9 + $7
  IntOp $9 $9 + $8
  ${If} $9 < 64
    Goto asioBinaryInvalid
  ${EndIf}
  IntOp $6 $1 - 24
  ${If} $9 > $6
    Goto asioBinaryInvalid
  ${EndIf}

  ; PE signature, AMD64 machine, and at least one section.
  StrCpy $AsioBinaryFailure "pe-signature"
  ClearErrors
  FileSeek $0 $9 SET $8
  FileReadByte $0 $2
  FileReadByte $0 $3
  FileReadByte $0 $4
  FileReadByte $0 $5
  FileReadByte $0 $6
  FileReadByte $0 $7
  ${If} ${Errors}
  ${OrIf} $2 != 80
  ${OrIf} $3 != 69
  ${OrIf} $4 != 0
  ${OrIf} $5 != 0
    Goto asioBinaryInvalid
  ${EndIf}
  StrCpy $AsioBinaryFailure "machine"
  IntOp $8 $7 << 8
  IntOp $8 $8 + $6
  ${If} $8 != ${IMAGE_FILE_MACHINE_AMD64}
    Goto asioBinaryInvalid
  ${EndIf}
  StrCpy $AsioBinaryFailure "section-count"
  FileReadByte $0 $2
  FileReadByte $0 $3
  ${If} ${Errors}
    Goto asioBinaryInvalid
  ${EndIf}
  IntOp $8 $3 << 8
  IntOp $8 $8 + $2
  ${If} $8 == 0
    Goto asioBinaryInvalid
  ${EndIf}

  ; Require a bounded PE32+ optional header and the IMAGE_FILE_DLL bit.
  StrCpy $AsioBinaryFailure "optional-header"
  IntOp $7 $9 + 20
  ClearErrors
  FileSeek $0 $7 SET $8
  FileReadByte $0 $2
  FileReadByte $0 $3
  FileReadByte $0 $4
  FileReadByte $0 $5
  ${If} ${Errors}
    Goto asioBinaryInvalid
  ${EndIf}
  IntOp $6 $3 << 8
  IntOp $6 $6 + $2
  ; Runtime reads the complete IMAGE_OPTIONAL_HEADER64, so installer discovery
  ; must require the same PE32+ header size rather than accepting only its magic.
  ${If} $6 < 240
    Goto asioBinaryInvalid
  ${EndIf}
  IntOp $8 $5 << 8
  IntOp $8 $8 + $4
  IntOp $8 $8 & ${IMAGE_FILE_DLL}
  ${If} $8 == 0
    StrCpy $AsioBinaryFailure "dll-characteristic"
    Goto asioBinaryInvalid
  ${EndIf}
  IntOp $7 $9 + 24
  IntOp $8 $7 + $6
  ${If} $8 > $1
    StrCpy $AsioBinaryFailure "optional-header-bounds"
    Goto asioBinaryInvalid
  ${EndIf}
  ; The section table immediately follows the optional header. Keep the
  ; section count and prove the complete NumberOfSections * 40-byte table fits
  ; without relying on overflowing addition. Re-read the count because every
  ; NSIS scratch register is also needed while validating the optional header.
  StrCpy $AsioBinaryFailure "section-table-bounds"
  IntOp $5 $9 + 6
  ClearErrors
  FileSeek $0 $5 SET $4
  FileReadByte $0 $2
  FileReadByte $0 $3
  ${If} ${Errors}
    Goto asioBinaryInvalid
  ${EndIf}
  IntOp $5 $3 << 8
  IntOp $5 $5 + $2
  IntOp $4 $1 - $8
  IntOp $4 $4 / 40
  ${If} $5 > $4
    Goto asioBinaryInvalid
  ${EndIf}
  StrCpy $AsioBinaryFailure "pe32-plus"
  ClearErrors
  FileSeek $0 $7 SET $8
  FileReadByte $0 $2
  FileReadByte $0 $3
  ${If} ${Errors}
  ${OrIf} $2 != 11
  ${OrIf} $3 != 2
    Goto asioBinaryInvalid
  ${EndIf}

  ; SizeOfHeaders must cover the NT headers and the complete section table,
  ; not merely name a non-empty prefix somewhere inside the file.
  IntOp $4 $5 * 40
  IntOp $4 $4 + $7
  IntOp $4 $4 + $6

  ; Match the runtime PE parser: SizeOfHeaders is a DWORD at offset 60 in an
  ; IMAGE_OPTIONAL_HEADER64 and must cover all parsed headers that exist.
  StrCpy $AsioBinaryFailure "size-of-headers"
  IntOp $8 $7 + 60
  ClearErrors
  FileSeek $0 $8 SET $6
  FileReadByte $0 $2
  FileReadByte $0 $3
  FileReadByte $0 $8
  FileReadByte $0 $9
  ${If} ${Errors}
    Goto asioBinaryInvalid
  ${EndIf}
  IntOp $5 $3 << 8
  IntOp $6 $8 << 16
  IntOp $7 $9 << 24
  IntOp $6 $6 + $2
  IntOp $6 $6 + $5
  IntOp $6 $6 + $7
  ${If} $6 < $4
  ${OrIf} $6 > $1
    Goto asioBinaryInvalid
  ${EndIf}

  StrCpy $AsioBinaryValid "1"
  StrCpy $AsioBinaryFailure ""
  ClearErrors
  FileClose $0
  Goto asioBinaryDone

  asioBinaryInvalid:
  ClearErrors
  FileClose $0
  ClearErrors
  asioBinaryDone:
  Pop $9
  Pop $8
  Pop $7
  Pop $6
  Pop $5
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd

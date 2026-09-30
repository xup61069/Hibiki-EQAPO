
!ifndef CONFIGURATION
!define CONFIGURATION "Release"
!endif

!define BINPATH "..\x64\${CONFIGURATION}"
!define BINPATH_EDITOR "..\x64\${CONFIGURATION}"
!define LIBPATH "lib64"
!define VCREDIST_URL "https://aka.ms/vs/17/release/vc_redist.x64.exe"
!define TARGET_ARCH "x64"
; ADR-0007's real driver/DAW gate is still open. Official builds exclude the
; experimental proxy; development builds must opt in explicitly.
!ifndef ENABLE_ASIO_PROXY
!define ENABLE_ASIO_PROXY 0
!endif

!include "Setup.nsi"

OutFile "Hibiki-EQAPO-x64-${VERSION}.exe"

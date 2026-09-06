
!ifndef CONFIGURATION
!define CONFIGURATION "Release"
!endif

!define BINPATH "..\x64\${CONFIGURATION}"
!define BINPATH_EDITOR "..\x64\${CONFIGURATION}"
!define LIBPATH "lib64"
!define VCREDIST_URL "https://aka.ms/vs/17/release/vc_redist.x64.exe"
!define TARGET_ARCH "x64"
!define ENABLE_ASIO_PROXY 1

!include "Setup.nsi"

OutFile "Hibiki-EQAPO-x64-${VERSION}.exe"

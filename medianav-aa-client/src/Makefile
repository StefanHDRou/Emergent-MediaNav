# =============================================================================
# MediaNav Android Auto Client - NMAKE Makefile
# Target: Windows CE 6.0 / MIPS (Alchemy Au1320)
# Toolchain: Visual Studio 2005 + Platform Builder CE 6.0 MIPS SDK
# =============================================================================
#
# USAGE:
#   nmake /f Makefile CFG=DEBUG      - Debug build with logging
#   nmake /f Makefile CFG=RELEASE    - Release build, max optimization
#   nmake /f Makefile clean          - Remove build artifacts
#
# PREREQUISITES:
#   - VS2005 with Platform Builder CE 6.0 installed
#   - MediaNav MIPS SDK installed (or generic MIPS WinCE 6.0 SDK)
#   - Run from VS2005 Command Prompt with SDK paths set
#
# NOTE (C++ sources):
#   - main.cpp and usb/usb_handler.cpp are C++98/03 translation units.
#     clmips.exe auto-selects the C++ front-end based on the .cpp
#     extension (no /TP switch needed).
#   - Legacy C USB files live under usb/legacy/ and are NOT built.
# =============================================================================

# --- Configuration -----------------------------------------------------------
!IF "$(CFG)" == ""
CFG=DEBUG
!ENDIF

# --- Toolchain ---------------------------------------------------------------
CC      = clmips.exe
LINK    = link.exe
RC      = rc.exe

# --- Target ------------------------------------------------------------------
TARGET_NAME = mn1aa
TARGET_EXE  = $(TARGET_NAME).exe

# --- Source Files ------------------------------------------------------------
# main.cpp is the C++ entry point. Everything else still C.
MAIN_SRC_CPP = main.cpp

# usb_handler.cpp is the user-mode USB host stack (HCD1: / EHCI direct).
# Legacy usb_host.c and aoa_handshake.c are excluded from the build on
# purpose - they live under usb/legacy/ for reference only.
USB_SRC_CPP = usb\usb_handler.cpp

PROTOCOL_SRC = \
    protocol\aap_framing.c \
    protocol\aap_service.c \
    protocol\pb_lite.c \
    protocol\custom_protocol.c

VIDEO_SRC = \
    video\mjpeg_decoder.c \
    video\jpeg_idct_mips.c \
    video\jpeg_huffman.c \
    video\color_convert.c

DISPLAY_SRC = \
    display\display_ddraw.c \
    display\display_gdi.c

INPUT_SRC = \
    input\touch_input.c

TLS_SRC = \
    tls\tls_minimal.c

UTIL_SRC = \
    util\ring_buffer.c \
    util\debug_log.c

ALL_CPP_SRC = $(MAIN_SRC_CPP) $(USB_SRC_CPP)
ALL_C_SRC   = $(PROTOCOL_SRC) $(VIDEO_SRC) $(DISPLAY_SRC) \
              $(INPUT_SRC) $(TLS_SRC) $(UTIL_SRC)

# --- Object Files (auto-generated from sources) -----------------------------
OBJDIR = obj\$(CFG)

OBJECTS = \
    $(OBJDIR)\main.obj \
    $(OBJDIR)\usb_handler.obj \
    $(OBJDIR)\aap_framing.obj \
    $(OBJDIR)\aap_service.obj \
    $(OBJDIR)\pb_lite.obj \
    $(OBJDIR)\custom_protocol.obj \
    $(OBJDIR)\mjpeg_decoder.obj \
    $(OBJDIR)\jpeg_idct_mips.obj \
    $(OBJDIR)\jpeg_huffman.obj \
    $(OBJDIR)\color_convert.obj \
    $(OBJDIR)\display_ddraw.obj \
    $(OBJDIR)\display_gdi.obj \
    $(OBJDIR)\touch_input.obj \
    $(OBJDIR)\tls_minimal.obj \
    $(OBJDIR)\ring_buffer.obj \
    $(OBJDIR)\debug_log.obj

# --- Compiler Flags ----------------------------------------------------------
# MIPS WinCE 6.0 flags
#   /MC   = Target WinCE (sets subsystem, entry point, etc.)
#   /GS-  = Disable buffer security checks (saves cycles)
#   /Gy   = Function-level linking (smaller binary)
#   /W3   = Warning level 3
#   /EHs- /EHc-  = Disable C++ exceptions (cheaper, no unwind tables)

COMMON_CFLAGS = \
    /nologo \
    /W3 \
    /GS- \
    /Gy \
    /DUNICODE /D_UNICODE \
    /DUNDER_CE=0x600 \
    /D_WIN32_WCE=0x600 \
    /DWINCE \
    /DMIPS \
    /D_MIPS_ \
    /I. \
    /Iusb /Iprotocol /Ivideo /Idisplay /Iinput /Itls /Iutil

# C++-specific flags (exceptions/RTTI disabled to match the minimal CE runtime)
CPP_EXTRA = /EHs- /EHc- /GR-

!IF "$(CFG)" == "DEBUG"
# Debug: No optimization, full debug info, logging enabled
CFLAGS   = $(COMMON_CFLAGS) /Od /Zi /DDEBUG /D_DEBUG /DMN1_LOG_LEVEL=3
CPPFLAGS = $(CFLAGS) $(CPP_EXTRA)
LDFLAGS  = /DEBUG /NOLOGO
!ELSE
# Release: Maximum optimization for speed
CFLAGS   = $(COMMON_CFLAGS) /Ox /Ot /Ob2 /DNDEBUG /DMN1_LOG_LEVEL=0
CPPFLAGS = $(CFLAGS) $(CPP_EXTRA)
LDFLAGS  = /RELEASE /NOLOGO /OPT:REF /OPT:ICF
!ENDIF

# --- Linker Flags ------------------------------------------------------------
# toolhelp.lib required by usb_handler.cpp (CreateToolhelp32Snapshot
# for MgrUSB.exe enumeration/termination).
LIBS = \
    coredll.lib \
    corelibc.lib \
    toolhelp.lib \
    ole32.lib \
    oleaut32.lib \
    uuid.lib \
    commctrl.lib \
    aygshell.lib \
    ddraw.lib

LFLAGS = $(LDFLAGS) \
    /SUBSYSTEM:WINDOWSCE \
    /MACHINE:MIPS \
    /ENTRY:WinMainCRTStartup \
    /NODEFAULTLIB:oldnames.lib \
    $(LIBS)

# =============================================================================
# BUILD RULES
# =============================================================================

all: dirs $(TARGET_EXE)

dirs:
    @if not exist $(OBJDIR) mkdir $(OBJDIR)

$(TARGET_EXE): $(OBJECTS)
    $(LINK) $(LFLAGS) /OUT:$@ $(OBJECTS)
    @echo.
    @echo ===== BUILD COMPLETE: $(TARGET_EXE) [$(CFG)] =====
    @echo.

# --- Explicit compile rules (NMAKE inference rules don't chain subdirs well) -

$(OBJDIR)\main.obj: main.cpp
    $(CC) $(CPPFLAGS) /c /Fo$@ main.cpp

$(OBJDIR)\usb_handler.obj: usb\usb_handler.cpp
    $(CC) $(CPPFLAGS) /c /Fo$@ usb\usb_handler.cpp

# --- C source inference rules (one per subdirectory) ------------------------

{protocol\}.c{$(OBJDIR)}.obj:
    $(CC) $(CFLAGS) /c /Fo$@ $<

{video\}.c{$(OBJDIR)}.obj:
    $(CC) $(CFLAGS) /c /Fo$@ $<

{display\}.c{$(OBJDIR)}.obj:
    $(CC) $(CFLAGS) /c /Fo$@ $<

{input\}.c{$(OBJDIR)}.obj:
    $(CC) $(CFLAGS) /c /Fo$@ $<

{tls\}.c{$(OBJDIR)}.obj:
    $(CC) $(CFLAGS) /c /Fo$@ $<

{util\}.c{$(OBJDIR)}.obj:
    $(CC) $(CFLAGS) /c /Fo$@ $<

# --- Clean -------------------------------------------------------------------
clean:
    @if exist obj rmdir /s /q obj
    @if exist $(TARGET_EXE) del $(TARGET_EXE)
    @echo Cleaned.

# --- Size report (post-build) ------------------------------------------------
size: $(TARGET_EXE)
    @echo --- Binary Size ---
    @dir $(TARGET_EXE) | findstr $(TARGET_EXE)

.PHONY: all dirs clean size

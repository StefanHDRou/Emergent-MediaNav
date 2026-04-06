# 01 - Toolchain Setup Guide

## Setting Up the MIPS WinCE 6.0 Toolchain in 2025/2026

This is the hardest part. Microsoft ended all support for Windows CE in 2018,
and finding the tools today requires archaeology-level effort.

---

## Prerequisites

| Item | Notes |
|------|-------|
| Windows XP SP2 or Vista VM | Required host for VS2005. Modern Windows has compatibility issues. |
| 8+ GB RAM for VM | VS2005 + Platform Builder is heavy |
| 20+ GB disk for VM | SDK generation creates large artifacts |
| VirtualBox or VMware | For running the XP/Vista VM on your modern machine |

---

## Step 1: Obtain Visual Studio 2005 with SP1

VS2005 is the ONLY officially supported IDE for WinCE 6.0 MIPS development.

1. **Download**: VS2005 Professional or Team Edition. Check:
   - MSDN subscriber downloads (if you still have access)
   - Archive.org mirrors of MSDN DVDs
   - Volume license ISO images
   
2. **Install VS2005** on your XP/Vista VM
3. **Install VS2005 SP1** (critical for CE 6.0 compatibility)
4. If using Vista: Install "VS2005 SP1 Update for Vista"

**Verification**: Open VS2005, go to Help -> About. Confirm version 8.0.50727.762 or later.

---

## Step 2: Install Windows Embedded CE 6.0 Platform Builder

Platform Builder is the key tool. It installs as a VS2005 plugin and includes:
- The MIPS cross-compiler (`clmips.exe`)
- WinCE 6.0 system headers and libraries
- BSP (Board Support Package) framework
- SDK generation wizard

1. **Download** from Microsoft:
   - [Windows Embedded CE 6.0 Platform Builder](https://www.microsoft.com/en-us/download/details.aspx?id=4097)
   - If the link is dead, search for "Windows Embedded CE 6.0 Evaluation Edition"
   
2. **Install Platform Builder** (runs as MSI)
3. **Install CE 6.0 Service Pack 1** (mandatory)
4. **Install QFE updates** (optional but recommended for stability)

After installation, VS2005 will have new project types under "Platform Builder".

---

## Step 3: Create a MIPS SDK

An SDK is needed to compile standalone applications (not kernel/drivers).

### Option A: Generate SDK from Platform Builder

1. In VS2005, create a new **OS Design** project:
   - Template: "PB - Windows Embedded CE 6.0"
   - BSP: Select any MIPS BSP (or "Device Emulator MIPS" if available)
   
2. In the Catalog:
   - Include: MIPS support, USB Host Support, DirectDraw, GDI, Shell
   - Exclude: Everything else (minimize build time)
   
3. Build the OS Design (this takes 30-60 minutes)

4. **Generate SDK**:
   - Project Menu -> "Create SDK..."
   - Fill in: Name="MediaNav_MIPS", Manufacturer="Custom"
   - Language: C/C++
   - Click "Build" -> generates an MSI installer

5. **Install the SDK** MSI on the same machine (or a dev machine with VS2005)

### Option B: Use an Existing SDK (Recommended)

Several vendors published MIPS CE 6.0 SDKs that may still work:

- **Toradex CE 6.0 SDK**: Check [developer.toradex.com](https://developer.toradex.com)
  - Edit the MSI with Orca tool to change VS namespace if needed for VS2008
  - Key: `VS_NAMESPACE` from `MS.VSIPCC.v80` to `v90` for VS2008

- **Generic MIPS CE SDK**: Search for "Windows CE 6.0 Standard SDK MIPS"

### Option C: Use VS2008 (Workaround)

If you only have VS2008:
1. Install the CE 6.0 SDK MSI
2. If it refuses to install, use [Orca](https://learn.microsoft.com/en-us/windows/win32/msi/orca-exe) to edit the MSI:
   - Open the MSI Property table
   - Change `VS_NAMESPACE` from `MS.VSIPCC.v80` to `MS.VSIPCC.v90`
3. Install the modified MSI

---

## Step 4: Verify the Toolchain

Open a **VS2005 Command Prompt** (or VS2008 if using the workaround).

```batch
REM Check if the MIPS compiler is available
where clmips.exe

REM If found, test a simple compile:
echo int main() { return 0; } > test.c
clmips.exe /nologo /c test.c
link.exe /nologo /SUBSYSTEM:WINDOWSCE /MACHINE:MIPS /OUT:test.exe test.obj coredll.lib
```

If `clmips.exe` is not found, the SDK paths are not set. Run:
```batch
REM Set SDK paths manually (adjust paths to your installation)
set INCLUDE=C:\WINCE600\public\common\sdk\inc;C:\WINCE600\public\common\oak\inc
set LIB=C:\WINCE600\public\common\sdk\lib\MIPS
set PATH=C:\WINCE600\sdk\bin\i386\MIPS;%PATH%
```

---

## Step 5: Alternative - Open Source Toolchain (EXPERIMENTAL)

The **CeGCC** project attempted to create an open-source GCC for WinCE,
but MIPS support was never well-maintained (focus was ARM).

If you want to try:
```bash
git clone https://github.com/nicta/cegcc-build.git
cd cegcc-build
# Edit scripts to target MIPS instead of ARM
# This is UNSUPPORTED and will require significant effort
```

**Recommendation**: Use VS2005 + Platform Builder. It's the only proven path for MIPS.

---

## Step 6: Deploy to MediaNav

After building `mn1aa.exe`:

1. Format a USB stick as **FAT32** (max 32GB)
2. Copy `mn1aa.exe` to the root of the USB stick
3. On the MediaNav, enter **Test Mode**:
   - Create a folder `logfiles_bavn` on the USB
   - Insert USB with engine running
   - Wait for the file copy popup
   - Go to System Version screen
   - Tap the specific code sequence (e.g., 8005 Enter, delete, 0362 Enter)
4. Use **Total Commander** (from Super Mod) to navigate to the USB
5. Run `mn1aa.exe`

### Alternative: Super Mod Installation

Install the **Super Mod v2** for MN1 (requires firmware >= 4.0.3):
1. Download Super Mod for MN1
2. Extract to USB root
3. Insert USB, accept the update
4. Total Commander will be available in the mod menu
5. Use it to run any `.exe` from the Storage Card or USB

---

## Common Issues

| Issue | Solution |
|-------|----------|
| `clmips.exe` not found | SDK paths not set. Check VS environment setup. |
| Link error: coredll.lib not found | LIB path doesn't include MIPS libs. Set manually. |
| App crashes on MediaNav | Verify MIPS binary (not ARM). Use `dumpbin /headers mn1aa.exe` |
| USB not working | MediaNav USB is in Device mode. Need OTG cable or register hack. |
| No display output | Try GDI backend instead of DirectDraw (set `MN1_DISPLAY_BACKEND=MN1_DISPLAY_GDI` in config.h) |

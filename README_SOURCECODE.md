# Jornada CE Meshtastic Client

---

## Build Instructions (eMbedded Visual C++ 3.0)

---

## Required Development Environment

This project must be built using:

- **Microsoft eMbedded Visual C++ 3.0**
- **Windows CE HPC2000 SDK**
- **ARM toolchain** (installed with eVC++)

> Modern versions of Visual Studio are not supported.

---

## Installing the Environment

1. Install **Microsoft eMbedded Visual C++ 3.0**.
2. During installation, ensure **ARM support** is selected.
3. Install the **Windows CE HPC2000 SDK**.
4. Start eVC++ and verify that the following platform appears in the platform dropdown:

```
HPC 2000
```

If **HPC 2000** does not appear, the SDK installation is incomplete.

---

## Opening the Project

1. Start **eMbedded Visual C++ 3.0**.
2. Open the workspace file:

```
MESHTASTIC.VCW
```

---

## Required Project Configuration

After opening the workspace, verify the toolbar dropdowns match exactly:

| Setting        | Value                              |
|---------------|------------------------------------|
| Project       | MESHTASTIC                         |
| Platform      | HPC 2000                           |
| Configuration | Win32 (WCE ARM) Debug / Release    |
| Device        | HPC 2000 (Default Device)          |

### Important

- The platform must be **HPC 2000**.
- The CPU target must be **ARM (WCE ARM)**.
- Do **not** select Pocket PC SDKs.
- Do **not** select the x86 emulator for final builds.

The HP Jornada 720/728 uses **ARMV4 architecture (StrongARM SA-1100)**, which requires the WCE ARM configuration.

---

## Building the Project

1. Select **Build → Rebuild All**.
2. Ensure there are no build errors.

The output executable will be generated in:

```
ARMDbg\   (Debug build)
ARMRel\   (Release build)
```

The resulting binary is:

```
MESHTASTIC.exe
```

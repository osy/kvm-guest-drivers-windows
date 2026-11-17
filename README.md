# WDDM driver for viogpu

Windows guest drivers for the VirtIO GPU (`PCI\VEN_1AF4&DEV_1050`), derived
from [virtio-win/kvm-guest-drivers-windows](https://github.com/virtio-win/kvm-guest-drivers-windows).
The repository has two kernel-mode drivers for the same device. Install one
or the other:

- **`viogpudo`** is a WDDM *display-only* driver. It handles modesetting and
  scanout, with no 3D acceleration. It builds with nothing but Visual Studio
  and the WDK.
- **`viogpu3d`** is a full WDDM driver with 3D acceleration. Its INF registers
  the **Neptune** user-mode driver (`neptune_umd.dll`, Mesa's Direct3D over
  virtio) as the Direct3D UMD, and the Venus Vulkan ICD (`virtio_icd.json` →
  `vulkan_virtio.dll`) as the Vulkan driver. Both user-mode pieces are built
  from a Mesa fork and packaged with the kernel driver.

Both packages also ship `vgpusrv.exe` (`viogpusc`) and `viogpuap.exe`
(`viogpuap`). These are a user-mode service and agent that apply resolution
changes inside the interactive session.

| Path | Contents |
| --- | --- |
| `viogpudo\`, `viogpu3d\` | the two kernel-mode drivers |
| `common\`, `shared\` | code shared by the drivers and the user-mode helpers |
| `viogpuap\`, `viogpusc\` | resolution agent and service |
| `VirtIO\` | VirtioLib, the virtio transport library the drivers link |
| `build\` | MSBuild props/targets, version resources, and the test certificate |

## Prerequisites

- **Visual Studio 2022** with the *Desktop development with C++* workload,
  including the build tools for each architecture you target (**x86**,
  **x64**, **ARM64**).
- The **Windows Driver Kit (WDK)** for that Visual Studio. Alternatively, use an
  **EWDK** command prompt, which provides both.

A complete `viogpu3d` package additionally needs the Mesa user-mode drivers.
See [Building viogpu3d](#building-viogpu3d).

## Building

`build.bat` at the repository root builds `viogpu.sln`. If `msbuild.exe` is on
`PATH` (an EWDK or Developer Command Prompt), it uses that MSBuild. Otherwise it
locates the newest Visual Studio installation with `vswhere`, so a plain
`cmd` prompt works.

```
build.bat [driver...] [arch...] [os...] [Debug|Release] [/Build|/Rebuild|/Clean]

  driver   viogpu3d, viogpudo or all            (default: all)
  arch     x86, x64 or ARM64                    (default: host architecture)
  os       Win10 or Win11                       (default: Win10 Win11)
```

For example, `build.bat viogpudo x64 Win11` or
`build.bat viogpu3d ARM64 Win10 Debug /Rebuild`. Building one driver also
builds what it depends on: VirtioLib and the two user-mode helpers. Each
configuration writes a full log, `build_<driver>_<os>_<arch>_<flavor>.log`,
next to `build.bat`.

The installable package goes to `Install\<os>\<arch>\`. A Debug build goes to
`Install_Debug\<os>\<arch>\` instead. `<arch>` is `x86`, `amd64` or `ARM64`.
Windows 11 has no x86 version, so x86 is built for Win10 only, and
`build.bat` skips any Win11 x86 combination.

On an ARM64 build host, `build.bat` turns off the WDK's InfVerif and
ApiValidator steps. The WDK runs x86-only binaries for both, and on ARM64 they
fail after a successful compile and link, which stops the package from being
assembled.

## Building viogpudo

```
build.bat viogpudo
```

No other setup is needed. The package contains `viogpudo.sys`,
`viogpudo.inf`, `viogpudo.cat`, `vgpusrv.exe` and `viogpuap.exe`.

## Building viogpu3d

A `viogpu3d` package has two halves: the kernel driver and helpers from this
repository, and the user-mode drivers from the
[Mesa fork](https://github.com/utmapp/mesa). This section describes this
repository's half, and how it packages the other.

### The kernel driver alone

```
build.bat viogpu3d
```

Without the user-mode drivers this compiles and links `viogpu3d.sys`,
`vgpusrv.exe` and `viogpuap.exe`, warns that `VIOGPU_UMD_DIR` is not set, and
stops there. No package is assembled, because the INF would list files that
are not there. This is also what a build from Visual Studio does when the
variable is not set, so the driver can be compiled and debugged without the
Mesa half.

### With the user-mode drivers

Set `VIOGPU_UMD_DIR`, as an environment variable or an MSBuild property, to a
flat directory that holds the user-mode files under their final names:

| File | |
| --- | --- |
| `neptune_umd.dll` | required: the Direct3D user-mode driver |
| `vulkan_virtio.dll` | required: the Venus Vulkan ICD |
| `virtio_icd.json` | required: the ICD manifest, with `"library_path": ".\\vulkan_virtio.dll"` |
| `neptune_umd_arm64.dll`, `neptune_umd_ec.dll` | optional pair: the two views behind an ARM64X `neptune_umd.dll` |
| `vulkan_virtio_arm64.dll`, `vulkan_virtio_ec.dll` | optional pair: the two views behind an ARM64X `vulkan_virtio.dll` |
| `neptune_umd_x86.dll` | optional: the 32-bit Direct3D driver for WOW64 applications |

```
set VIOGPU_UMD_DIR=C:\path\to\umd
build.bat viogpu3d ARM64
```

The build packages the required files, and every optional set that is present,
next to `viogpu3d.sys` in `Install\<os>\<arch>\`, and generates an INF to
match: an optional set adds its `SourceDisksFiles` and `CopyFiles` entries, and
the 32-bit driver also adds the `UserModeDriverNameWow` registry value. The INF
is regenerated on every build, so it always describes the directory it was
built against. A required file that is missing, half of a pair, or a file that
does not apply to the target (ARM64X files outside an ARM64 build, the WOW64
driver in a 32-bit build) fails the build.

What the optional files are for:

- **ARM64X.** Windows on ARM runs native ARM64 processes and, under emulation,
  x64 ones, and no single DLL serves both. Each user-mode driver is therefore
  built for arm64 and for arm64ec, and the pair ships behind an ARM64X front
  DLL that keeps the plain name. The INF needs no other change: the adapter
  still registers `neptune_umd.dll` and `virtio_icd.json`. Without the pair,
  only native ARM64 applications can use the device.
- **WOW64.** 32-bit applications run as WOW64 processes on both x64 and ARM64
  Windows and can only load a 32-bit DLL; ARM64X has no 32-bit view. The
  32-bit driver has its own file name because dirid 13 (the DriverStore) has no
  WoW64 file-system redirection, so both drivers land in one directory, and it
  forbids renaming in `CopyFiles`. Without it, 32-bit applications fall back to
  WARP or fail to create a device.

The INF registers no Direct3D 9 UMD. The D3D9 slot of `UserModeDriverName`
is the token `<>`. dxgkrnl reports that token as "no driver", so the D3D9
runtime serves Direct3D 9 through `D3D9On12` on top of the D3D12 UMD. Any
other value in that slot either fails device creation or hides the adapter
entirely. That includes a UMD without `OpenAdapter`, `d3d9on12.dll` itself,
and an empty string.

### The driver version

Windows requires the Direct3D UMD's file version to equal the INF `DriverVer`
(`Device.Graphics.AdapterBase.DriverVersion`). The kernel driver stamps
`$(_NT_TARGET_MAJ).$(_RHEL_RELEASE_VERSION_).$(_BUILD_MAJOR_VERSION_).$(_BUILD_MINOR_VERSION_)`,
set in `build\Driver.RHEL.props`. That file is the one place the version is
defined: build the UMD with the same number.

### Revision stamps

A package names the commits it was built from. The INF header lists them:

```
; Built from:
;   viogpu-wddm     9ee33e8d9b
;   mesa            3eac9d327e
```

and the product version string of `viogpu3d.sys` carries the driver's own, as
in `100.6.101.58000 (git-9ee33e8d9b)`. A trailing `+` marks a build from a
tree with uncommitted changes. The values come from two optional environment
variables or MSBuild properties:

| Variable | |
| --- | --- |
| `VIOGPU_KMD_REVISION` | this repository; when unset, the build asks `git` |
| `VIOGPU_UMD_REVISION` | the Mesa fork the files in `VIOGPU_UMD_DIR` came from |

Whatever is neither set nor found is stamped `unknown`; nothing fails for lack
of a stamp.

## Signing

The build does not produce an installable signature. The WDK test-signs the
`.sys` with a certificate it generates for the local user, and leaves the
catalog unsigned. To sign a package for a test machine, sign the `.sys` and
the `.cat` with the repository's test certificate:

```
signtool sign /fd SHA256 /f build\VirtIOTestCert.pfx Install\<os>\<arch>\<driver>.sys
signtool sign /fd SHA256 /f build\VirtIOTestCert.pfx Install\<os>\<arch>\<driver>.cat
```

## Installing

On the guest, enable test signing (`bcdedit /set testsigning on`, then
reboot). Copy `build\VirtIOTestCert.cer` to the guest and trust it in **both**
the machine `Root` and `TrustedPublisher` stores:

```
certutil -addstore Root VirtIOTestCert.cer
certutil -addstore TrustedPublisher VirtIOTestCert.cer
```

Then install the whole package directory, for example:

```
pnputil /add-driver viogpu3d.inf /install
```

The kernel driver loads without a reboot. A healthy `viogpu3d` install shows
the *Red Hat VirtIO GPU 3D controller* device as *Started*, and Direct3D device
creation succeeds on the `0x1af4` adapter.

## Troubleshooting

- **The `viogpu3d` build warns that `VIOGPU_UMD_DIR` is not set and produces no
  package**: that is the kernel-only build. Build the user-mode drivers and
  point `VIOGPU_UMD_DIR` at a directory that holds them.
- **The `viogpu3d` build fails with `VIOGPU_UMD_DIR: ... is missing`**: the
  directory lacks a required file or half of an optional pair. See
  [With the user-mode drivers](#with-the-user-mode-drivers).
- **`build.bat` cannot find MSBuild**: install Visual Studio 2022 with the WDK,
  or run `build.bat` from an EWDK or Developer Command Prompt.
- **An x64-emulated app fails to load the UMD on ARM64** (for example,
  `ERROR_BAD_EXE_FORMAT`): the package holds the native-arm64 UMD only. Add
  the ARM64X pairs to `VIOGPU_UMD_DIR`.
- **A 32-bit app falls back to WARP or fails device creation on an x64 or
  ARM64 guest**: the package has no `neptune_umd_x86.dll`, so no
  `UserModeDriverNameWow` was written. Add `neptune_umd_x86.dll` to
  `VIOGPU_UMD_DIR`.
- **`pnputil` reports that the certificate chain is not trusted**: the test
  certificate must be in both the `Root` and `TrustedPublisher` machine stores.

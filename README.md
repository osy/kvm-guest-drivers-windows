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

`viogpu3d` additionally needs the Mesa user-mode driver. See
[Building viogpu3d](#building-viogpu3d) for its prerequisites.

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

The 3D driver is built in two halves, which are then packaged together:

1. Build the **Mesa user-mode driver** and install it to a prefix directory.
2. Point the kernel driver build at that prefix (`MESA_PREFIX`). The
   `viogpu3d` project then packages the UMD DLLs alongside `viogpu3d.sys` into
   an installable driver.

`MESA_PREFIX` is required. The INF always references the user-mode driver
files, so without them `viogpu3d.sys` still compiles, but the build fails when
it creates the catalog (inf2cat reports `neptune_umd.dll` and the other files
as missing).

These steps cover **x86**, **x64** and **ARM64** targets. ARM64 has an extra
step, an ARM64X user-mode driver, so that x64-emulated Direct3D applications
can also use the device. See [ARM64X](#arm64x-arm64-with-x64-emulation-support).

### Additional prerequisites

- **Python** (with `pip`), **CMake**, **Git**, `ninja`, and an MSVC-capable
  **glslang**, for building Mesa. `meson`, `mako`, `packaging` and `pyyaml`
  are installed with `pip`.
- A **static LLVM** built for the *target* architecture. Mesa's
  `virgl`/`llvmpipe`/`swrast` drivers link against it. When cross-compiling
  (for example, the arm64 UMD on an x64 host), you also need a *native*
  `llvm-tblgen`. The Neptune-only ARM64EC view described below is built with
  LLVM disabled and does not need it.
- The **ARM64EC** MSVC build tools, for an ARM64X user-mode driver.

### 1. Build the Mesa user-mode driver

Clone the Mesa fork that carries the Neptune driver:

> **https://github.com/osy/virtio-win-mesa**

Configure it with Meson for a statically linked Windows build that enables
Neptune and the Venus (virtio) Vulkan driver. Replace `<PREFIX>` with a
prefix directory of your choice:

```
meson setup build ^
  --prefix=<PREFIX> ^
  --default-library=static -Dbuildtype=release -Db_ndebug=true -Db_vscrt=mt ^
  -Dc_args="/experimental:c11atomics" ^
  -Dplatforms=windows -Dvideo-codecs= ^
  -Dllvm=enabled ^
  -Dgallium-drivers=virgl,llvmpipe -Dvulkan-drivers=virtio,swrast ^
  -Dneptune=true -Dnpt_wine=false -Dnpt_umd=ddi
ninja -C build install
```

The options that matter:

- `-Dneptune=true` builds the Neptune Direct3D UMD, `neptune_umd.dll`.
- `-Dnpt_wine=false` builds the Windows UMD instead of the Linux/Wine unixlib.
- `-Dnpt_umd=ddi` builds a WDDM-compatible UMD.
- `-Dvulkan-drivers=virtio` builds the Venus Vulkan ICD: `vulkan_virtio.dll`
  and its `virtio_icd.json` manifest.
- `--default-library=static` and `-Db_vscrt=mt` link everything statically, so
  no extra runtime DLLs have to ship.
- `-Dc_args="/experimental:c11atomics"` is required for Mesa under MSVC.

After `ninja install`, the UMD is in `<PREFIX>\bin`: `neptune_umd.dll`,
`vulkan_virtio.dll` and `virtio_icd.json`.

Build for the architecture the guest runs:

- **x86 / x64**: configure and build in a Visual Studio developer prompt for
  that architecture (`x86` or `amd64`). No cross file is needed when the host
  and target architectures match.
- **ARM64**: build natively on Windows on ARM, or cross-compile from x64.
  To cross-compile, add a Meson `--cross-file` that describes the
  `aarch64-pc-windows-msvc` toolchain and points at an arm64 LLVM. The Mesa
  repo has cross-file examples.

Windows requires the UMD DLLs' file version to equal the INF `DriverVer`
(`Device.Graphics.AdapterBase.DriverVersion`). The kernel driver stamps
`$(_NT_TARGET_MAJ).$(_RHEL_RELEASE_VERSION_).$(_BUILD_MAJOR_VERSION_).$(_BUILD_MINOR_VERSION_)`,
set in `build\Driver.RHEL.props`. Pass the same value to the Mesa build as
`NPT_UMD_VERSION`. The build-mesa `build.cmd` forwards it as
`-Dnpt_umd_version`. That stamps `neptune_umd.dll`, and also the ARM64X
forwarder through `make-arm64x.bat`.

### 2. Build the kernel-mode driver

Set `MESA_PREFIX` to the prefix from step 1, the directory whose `bin`
subdirectory holds `vulkan_virtio.dll`, and build:

```
set MESA_PREFIX=<PREFIX>
build.bat viogpu3d <arch>
```

When `%MESA_PREFIX%\bin\vulkan_virtio.dll` exists, the `viogpu3d` project
first regenerates the Vulkan ICD manifest for that prefix
(`viogpu3d\tools\update_icd_jsons.ps1`, written to `viogpu3d\icd\`). It then
packages `neptune_umd.dll`, `vulkan_virtio.dll` and `virtio_icd.json` into the
driver.

`viogpu.sln` maps the Win11 ARM64 and x86 configurations of `viogpu3d` to
its Win10 ones. Those builds therefore produce `Install\Win10\<arch>\`, which
is the package to install on Windows 11 as well.

The package in `Install\<os>\<arch>\` contains `viogpu3d.sys`, `viogpu3d.inf`,
`viogpu3d.cat`, `vgpusrv.exe`, `viogpuap.exe`, and the user-mode driver files.
An ARM64 build also packages the two ARM64X view DLLs (`neptune_umd_arm64.dll`,
`neptune_umd_ec.dll`) when `MESA_ARM64X_PREFIX` is set. Any 64-bit build also
packages `neptune_umd_x86.dll` when `MESA_X86_PREFIX` is set. Both are
described below.

The INF registers no Direct3D 9 UMD. The D3D9 slot of `UserModeDriverName`
is the token `<>`. dxgkrnl reports that token as "no driver", so the D3D9
runtime serves Direct3D 9 through `D3D9On12` on top of the D3D12 UMD. Any
other value in that slot either fails device creation or hides the adapter
entirely. That includes a UMD without `OpenAdapter`, `d3d9on12.dll` itself,
and an empty string.

### ARM64X (ARM64 with x64-emulation support)

On Windows on ARM, native arm64 apps can load a native-arm64
`neptune_umd.dll`, but x64-emulated Direct3D apps **cannot**. Those apps need
an **ARM64EC** view of the UMD. To support both from one driver, the
user-mode driver must be an **ARM64X** image, which carries both a
native-arm64 view and an arm64ec view.

Windows cannot load a single DLL that satisfies both native-arm64 and arm64ec
callers. So the UMD is built twice, and the two builds are stitched together
behind a code-less ARM64X **forwarder**. The result is **three files that ship
side by side**:

- `neptune_umd.dll`, the forwarder that the OS loads
- `neptune_umd_arm64.dll`, the arm64 view
- `neptune_umd_ec.dll`, the arm64ec view

**`make-arm64x.bat`** produces the forwarder. It is in the Mesa fork, next to
the Neptune UMD, at `src/virtio/neptune/triton/tools/make-arm64x.bat`. It
derives the forwarder's exports from `neptune_umd.def`, the same file meson
feeds the real builds, so the forwarder can never drift from the driver's ABI.
It assigns each input DLL to a view by machine type, not by argument order,
and writes the three-file set.

After the normal arm64 UMD build (step 1), produce and package the ARM64X UMD
in three steps:

1. **Build a second, ARM64EC view of the UMD.** Reconfigure Mesa for arm64ec.
   Add `/arm64EC` to the C and C++ arguments and use the dynamic CRT
   (`-Db_vscrt=md`). Disable LLVM and the other drivers, since the emulated
   view only needs Neptune:

   ```
   meson setup build-ec ^
     --prefix=<PREFIX-EC> ^
     --default-library=static -Dbuildtype=release -Db_ndebug=true -Db_vscrt=md ^
     -Dc_args="/experimental:c11atomics /arm64EC" -Dcpp_args="/arm64EC" ^
     -Dllvm=disabled -Dplatforms=windows -Dvideo-codecs= ^
     -Dgallium-drivers= -Dvulkan-drivers= ^
     -Dopengl=false -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dglx=disabled ^
     -Dneptune=true -Dnpt_wine=false -Dnpt_umd=ddi ^
     --cross-file <optional arm64ec cross file>
   ninja -C build-ec
   ```

   This produces an arm64ec `neptune_umd.dll`. The arm64ec build needs the
   ARM64EC MSVC and SDK import libraries on `LIB`: the `arm64ec` variants of
   the VC and Windows SDK `um`/`ucrt` libraries.

2. **Assemble the ARM64X three-file set with `make-arm64x.bat`.** Pass it the
   two `neptune_umd.dll` builds in either order: the native-arm64 one from
   step 1 and the arm64ec one from above. Also pass an output directory:

   ```
   src\virtio\neptune\triton\tools\make-arm64x.bat ^
     <PREFIX>\bin\neptune_umd.dll <build-ec>\...\neptune_umd.dll ^
     <ARM64X-DIR>
   ```

   It classifies each input by machine type (`AA64` is the native view and
   `8664` is the EC view) and builds the forwarder. It writes
   `neptune_umd.dll`, `neptune_umd_arm64.dll` and `neptune_umd_ec.dll` into
   `<ARM64X-DIR>`. It finds Visual Studio with `vswhere` and enters the arm64
   developer environment itself.

3. **Package all three: point `MESA_ARM64X_PREFIX` at `<ARM64X-DIR>`.**

   ```
   set MESA_PREFIX=<PREFIX>
   set MESA_ARM64X_PREFIX=<ARM64X-DIR>
   build.bat viogpu3d ARM64
   ```

   For the **ARM64** target, the build picks up the three files automatically.
   It packages the forwarder as `neptune_umd.dll` and keeps that name as
   `UserModeDriverName`. It adds `neptune_umd_arm64.dll` and
   `neptune_umd_ec.dll` to the package, the catalog, and the INF's file and
   copy lists. No manual INF or catalog edits are needed.

   `MESA_ARM64X_PREFIX` only affects ARM64 builds. x86 and x64 builds always
   package the single native `neptune_umd.dll` from `MESA_PREFIX`. If
   `MESA_ARM64X_PREFIX` is unset, or `%MESA_ARM64X_PREFIX%\neptune_umd_ec.dll`
   is missing, the ARM64 build falls back to the native-arm64
   `neptune_umd.dll`. Then only native-arm64 apps can load the UMD.

#### The Vulkan ICD

The same applies to the Venus Vulkan ICD: an x64-emulated or ARM64EC Vulkan
application cannot load the native-arm64 `vulkan_virtio.dll`. To ship an
ARM64X ICD as well:

1. Also build Venus in the arm64ec configuration, by passing
   `-Dvulkan-drivers=virtio` instead of `-Dvulkan-drivers=` in the ARM64EC step
   above.
2. Run `src\virtio\vulkan\tools\make-arm64x-vk.bat` from the Mesa fork on the
   two `vulkan_virtio.dll` builds, and write into the same `<ARM64X-DIR>`:

   ```
   src\virtio\vulkan\tools\make-arm64x-vk.bat ^
     <PREFIX>\bin\vulkan_virtio.dll <PREFIX-EC>\bin\vulkan_virtio.dll ^
     <ARM64X-DIR>
   ```

   A code-less forwarder does not work for an ICD. The Vulkan loader resolves
   entry points with `GetProcAddress`, and Windows looks up a forwarded export
   on the default DLL search path, which does not include the DriverStore.
   Instead, the front `vulkan_virtio.dll` is a small ARM64X loader stub. It
   loads `vulkan_virtio_arm64.dll` or `vulkan_virtio_ec.dll` from its own
   directory.
3. With `MESA_ARM64X_PREFIX` pointing at that directory, the ARM64 build
   packages the stub as `vulkan_virtio.dll` together with the two payloads.
   It also adds them to the catalog and the INF. If
   `%MESA_ARM64X_PREFIX%\vulkan_virtio_ec.dll` is missing, the build falls back
   to the native-arm64 `vulkan_virtio.dll` from `MESA_PREFIX`.

### WOW64 (32-bit applications on a 64-bit guest)

32-bit x86 Direct3D applications run as WOW64 processes on both x64 and ARM64
Windows, and a WOW64 process can only load a 32-bit PE32 DLL. ARM64X does not
help here. An ARM64X image is PE32+, and its two views are arm64 and
arm64ec/x64, with no 32-bit view. Supporting 32-bit apps therefore needs a
separate **x86** build of the UMD. The adapter's `UserModeDriverNameWow`
registry value points at it.

Build the x86 UMD exactly as in step 1, from an `x86` developer prompt, into
its own prefix. Venus is not currently built for x86, so only
`neptune_umd.dll` is needed. Then point `MESA_X86_PREFIX` at that prefix, the
directory whose `bin` subdirectory holds the x86 `neptune_umd.dll`:

```
set MESA_PREFIX=<PREFIX>
set MESA_X86_PREFIX=<PREFIX-X86>
build.bat viogpu3d x64
```

The build stages `%MESA_X86_PREFIX%\bin\neptune_umd.dll` under the name
**`neptune_umd_x86.dll`** and adds it to the package and the catalog. It also
adds its `SourceDisksFiles` and `CopyFiles` entries and a
`UserModeDriverNameWow` line to the INF. The rename is necessary for two
reasons. The 32-bit and 64-bit UMDs share one DriverStore directory, because
dirid 13 has no WoW64 file-system redirection. And dirid 13 forbids renaming
in `CopyFiles`, so the rename has to happen at build time.

`MESA_X86_PREFIX` is independent of `MESA_ARM64X_PREFIX` and works together
with it. An ARM64 build with both set ships the ARM64X three-file set *and* the
x86 UMD. That covers native-arm64, x64-emulated and x86-emulated
applications. If `MESA_X86_PREFIX` is unset, or
`%MESA_X86_PREFIX%\bin\neptune_umd.dll` is missing, nothing WOW-related is
packaged. It is ignored for `x86` targets, where the same binary would be the
native UMD rather than the WOW64 one.

## Signing

The projects have signing turned off (`SignMode` is `Off`), so the build does
not sign anything. To install on a test machine, sign the `.sys` and the
`.cat` with the repository's test certificate:

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

- **The `viogpu3d` build fails in inf2cat with `neptune_umd.dll ... is missing`**:
  `MESA_PREFIX` is unset, or `%MESA_PREFIX%\bin\vulkan_virtio.dll` is missing.
  Build and install the Mesa UMD (step 1) first, then set `MESA_PREFIX`.
- **`build.bat` cannot find MSBuild**: install Visual Studio 2022 with the WDK,
  or run `build.bat` from an EWDK or Developer Command Prompt.
- **An x64-emulated app fails to load the UMD on ARM64** (for example,
  `ERROR_BAD_EXE_FORMAT`): the UMD is native-arm64 only. Build the ARM64X
  image as described above.
- **A 32-bit app falls back to WARP or fails device creation on an x64 or
  ARM64 guest**: `MESA_X86_PREFIX` was unset, so no `UserModeDriverNameWow`
  was written. Build the x86 UMD and set `MESA_X86_PREFIX`, as described in
  *WOW64* above.
- **`pnputil` reports that the certificate chain is not trusted**: the test
  certificate must be in both the `Root` and `TrustedPublisher` machine stores.
- **The UMD architecture is wrong**: it must match the kernel driver's target.
  Use an `x86` UMD with an `x86` kernel driver and `x64` with `x64`. On ARM64,
  use the ARM64X (or native-arm64) UMD with the `ARM64` kernel driver.

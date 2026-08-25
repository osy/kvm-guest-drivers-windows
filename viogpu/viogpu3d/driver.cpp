/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "driver.h"
#include "helper.h"
#include "baseobj.h"
#include "viogpu_adapter.h"
#include "viogpu_device.h"
#include "viogpu_pnp_fixup.h"
#if !DBG
#include "driver.tmh"
#endif

#pragma code_seg(push)
#pragma code_seg("INIT")

int nDebugLevel;
int virtioDebugLevel;
int bDebugPrint;
int bBreakAlways;

// DxgkDdiUnload takes no arguments, so stash what WPP_CLEANUP needs to
// deregister the trace provider it registered against this driver object.
static DRIVER_OBJECT *g_pDriverObject = NULL;

// Pool tags ('VgPr' / 'VgRp' in the pool tracker; tags display reversed).
#define VIOGPU3D_PROCESS_TAG ((ULONG)'rPgV')
#define VIOGPU3D_REGPATH_TAG ((ULONG)'pRgV')

tDebugPrintFunc VirtioDebugPrintProc;

#ifdef DBG
void InitializeDebugPrints(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    bDebugPrint = 0;
    virtioDebugLevel = 0;
    nDebugLevel = TRACE_LEVEL_NONE;
    bBreakAlways = 0;

#if defined(DBG_VERBOSE)
    bDebugPrint = 1;
    virtioDebugLevel = 0x5;
    bBreakAlways = 1;
    nDebugLevel = TRACE_LEVEL_INFORMATION;
#endif
#if defined(COM_DEBUG)
    VirtioDebugPrintProc = DebugPrintFuncSerial;
#elif defined(PRINT_DEBUG)
    VirtioDebugPrintProc = DebugPrintFuncKdPrint;
#endif
}
#endif

#include <ntddk.h>
#include "viogpu_device.h"

#pragma code_seg(push)
#pragma code_seg("PAGE")

// Read tuning values from <pRegistryPath>\Parameters.  Absent values,
// wrong types, or any failure leave the compiled-in defaults untouched.
static VOID VioGpuReadDriverParameters(_In_ PUNICODE_STRING pRegistryPath)
{
    PAGED_CODE();

    static const WCHAR paramsSuffix[] = L"\\Parameters";
    ULONG debugLevel = 0;
    ULONG defaultValue = 0;

    // Build a NUL-terminated "<service>\Parameters" path for
    // RTL_REGISTRY_ABSOLUTE (pRegistryPath is counted, not terminated).
    SIZE_T cb = pRegistryPath->Length + sizeof(paramsSuffix);
    PWCHAR path = (PWCHAR)ExAllocatePoolZero(PagedPool, cb, VIOGPU3D_REGPATH_TAG);
    if (path == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s failed to allocate registry path\n", __FUNCTION__));
        return;
    }
    RtlCopyMemory(path, pRegistryPath->Buffer, pRegistryPath->Length);
    RtlCopyMemory((PUCHAR)path + pRegistryPath->Length, paramsSuffix, sizeof(paramsSuffix));

    RTL_QUERY_REGISTRY_TABLE query[2];
    RtlZeroMemory(query, sizeof(query));
    // DbgPrint verbosity (TRACE_LEVEL_*).  Absent or 0 keeps the
    // compiled-in level, which is silent unless DBG_VERBOSE was defined --
    // this value is the only way to turn KMD tracing on without a rebuild.
    query[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    query[0].Name = (PWSTR)L"DebugLevel";
    query[0].EntryContext = &debugLevel;
    query[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    query[0].DefaultData = &defaultValue;
    query[0].DefaultLength = sizeof(defaultValue);

    NTSTATUS status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, query, NULL, NULL);
    ExFreePoolWithTag(path, VIOGPU3D_REGPATH_TAG);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("%s RtlQueryRegistryValues failed 0x%X\n", __FUNCTION__, status));
        return;
    }
    if (debugLevel != 0)
    {
        nDebugLevel = (int)debugLevel;
    }
}

extern "C" NTSTATUS DriverEntry(_In_ DRIVER_OBJECT *pDriverObject, _In_ UNICODE_STRING *pRegistryPath)
{
    PAGED_CODE();
    WPP_INIT_TRACING(pDriverObject, pRegistryPath)
    g_pDriverObject = pDriverObject;
    DbgPrint(TRACE_LEVEL_FATAL, ("---> VIOGPU FULL build on on %s %s\n", __DATE__, __TIME__));
    DRIVER_INITIALIZATION_DATA InitialData = {0};

    // 2.2 is the highest version that costs nothing above the 2.0 (GpuMmu)
    // feature set this driver implements: every DDI new in 2.1/2.2 is cap- or
    // callback-conditional, and no dxgkrnl validation above 2.0 is keyed on
    // the version alone.
    // Do NOT raise this to 2.3: from the 2.3 kernel interface dxgkrnl routes
    // every flip through DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3
    // (unimplemented here) and bugchecks 0xD1 on the NULL entry the moment
    // the desktop flips.  The D3D11 runtime's 2.3 UMD DDI (needed for BGRA8
    // typed UAV) is offered independently by the UMD's version ladder and
    // does not require the KMD side to move.
    InitialData.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_2;

    InitialData.DxgkDdiAddDevice = VioGpu3DAddDevice;
    InitialData.DxgkDdiStartDevice = VioGpu3DStartDevice;
    InitialData.DxgkDdiStopDevice = VioGpu3DStopDevice;
    InitialData.DxgkDdiRemoveDevice = VioGpu3DRemoveDevice;

    InitialData.DxgkDdiDispatchIoRequest = VioGpu3DDispatchIoRequest;
    InitialData.DxgkDdiInterruptRoutine = VioGpu3DInterruptRoutine;
    InitialData.DxgkDdiDpcRoutine = VioGpu3DDpcRoutine;

    InitialData.DxgkDdiQueryChildRelations = VioGpu3DQueryChildRelations;
    InitialData.DxgkDdiQueryChildStatus = VioGpu3DQueryChildStatus;
    InitialData.DxgkDdiQueryDeviceDescriptor = VioGpu3DQueryDeviceDescriptor;
    InitialData.DxgkDdiSetPowerState = VioGpu3DSetPowerState;
    InitialData.DxgkDdiResetDevice = VioGpu3DResetDevice;
    InitialData.DxgkDdiUnload = VioGpu3DUnload;

    InitialData.DxgkDdiQueryAdapterInfo = VioGpu3DQueryAdapterInfo;
    InitialData.DxgkDdiEscape = VioGpu3DEscape;
    InitialData.DxgkDdiCreateAllocation = VioGpu3DCreateAllocation;
    InitialData.DxgkDdiOpenAllocation = VioGpu3DOpenAllocation;
    InitialData.DxgkDdiCloseAllocation = VioGpu3DCloseAllocation;
    InitialData.DxgkDdiDescribeAllocation = VioGpu3DDescribeAllocation;
    InitialData.DxgkDdiDestroyAllocation = VioGpu3DDestroyAllocation;
    InitialData.DxgkDdiGetStandardAllocationDriverData = VioGpu3DGetStandardAllocationDriverData;
    InitialData.DxgkDdiBuildPagingBuffer = VioGpu3DBuildPagingBuffer;

    // InitialData.DxgkDdiAcquireSwizzlingRange = VioGpu3DAcquireSwizzlingRange;
    // InitialData.DxgkDdiReleaseSwizzlingRange = VioGpu3DReleaseSwizzlingRange;

    InitialData.DxgkDdiCreateContext = VioGpu3DDdiCreateContext;
    InitialData.DxgkDdiDestroyContext = VioGpu3DDdiDestroyContext;

    InitialData.DxgkDdiPresent = VioGpu3DPresent;
    InitialData.DxgkDdiRender = VioGpu3DRender;
    InitialData.DxgkDdiPatch = VioGpu3DPatch;
    InitialData.DxgkDdiSubmitCommand = VioGpu3DSubmitCommand;

    InitialData.DxgkDdiSetPointerPosition = VioGpu3DSetPointerPosition;
    InitialData.DxgkDdiSetPointerShape = VioGpu3DSetPointerShape;
    InitialData.DxgkDdiIsSupportedVidPn = VioGpu3DIsSupportedVidPn;
    InitialData.DxgkDdiRecommendFunctionalVidPn = VioGpu3DRecommendFunctionalVidPn;
    InitialData.DxgkDdiEnumVidPnCofuncModality = VioGpu3DEnumVidPnCofuncModality;
    InitialData.DxgkDdiSetVidPnSourceVisibility = VioGpu3DSetVidPnSourceVisibility;
    InitialData.DxgkDdiCommitVidPn = VioGpu3DCommitVidPn;
    InitialData.DxgkDdiUpdateActiveVidPnPresentPath = VioGpu3DUpdateActiveVidPnPresentPath;
    InitialData.DxgkDdiSetVidPnSourceAddress = VioGpu3DSetVidPnSourceAddress;
    InitialData.DxgkDdiRecommendMonitorModes = VioGpu3DRecommendMonitorModes;
    InitialData.DxgkDdiQueryVidPnHWCapability = VioGpu3DQueryVidPnHWCapability;
    InitialData.DxgkDdiSystemDisplayEnable = VioGpu3DSystemDisplayEnable;
    InitialData.DxgkDdiSystemDisplayWrite = VioGpu3DSystemDisplayWrite;

    InitialData.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = VioGpu3DStopDeviceAndReleasePostDisplayOwnership;

    InitialData.DxgkDdiCreateDevice = VioGpu3DCreateDevice;
    InitialData.DxgkDdiDestroyDevice = VioGpu3DDestroyDevice;

    InitialData.DxgkDdiPreemptCommand = VioGpu3DDdiPreemptCommand;
    InitialData.DxgkDdiResetFromTimeout = VioGpu3DDdiResetFromTimeout;
    InitialData.DxgkDdiRestartFromTimeout = VioGpu3DDdiRestartFromTimeout;
    InitialData.DxgkDdiCollectDbgInfo = VioGpu3DDdiCollectDbgInfo;
    InitialData.DxgkDdiQueryCurrentFence = VioGpu3DDdiQueryCurrentFence;

    InitialData.DxgkDdiQueryEngineStatus = VioGpu3DDdiQueryEngineStatus;
    InitialData.DxgkDdiResetEngine = VioGpu3DDdiResetEngine;
    InitialData.DxgkDdiCancelCommand = VioGpu3DDdiCancelCommand;

    InitialData.DxgkDdiGetNodeMetadata = VioGpu3DDdiGetNodeMetadata;
    InitialData.DxgkDdiControlInterrupt = VioGpu3DDdiControlInterrupt;
    InitialData.DxgkDdiGetScanLine = VioGpu3DDdiGetScanLine;

    // WDDM2 (GpuMmu) DDIs.
    InitialData.DxgkDdiCreateProcess = VioGpu3DCreateProcess;
    InitialData.DxgkDdiDestroyProcess = VioGpu3DDestroyProcess;
    InitialData.DxgkDdiSubmitCommandVirtual = VioGpu3DSubmitCommandVirtual;
    InitialData.DxgkDdiSetRootPageTable = VioGpu3DSetRootPageTable;
    InitialData.DxgkDdiGetRootPageTableSize = VioGpu3DGetRootPageTableSize;
    // dxgkrnl 26100 refuses AddAdapter for a WDDM2+ driver without
    // these two (ETW 494: "Driver is compiled against
    // DXGKDDI_INTERFACE_VERSION_WDDM2_0_M2_2_1 or greater, but does
    // not fill in the pfnCalibrateGpuClock or pfnSetStablePowerState
    // DDI", then 549 StartAdapter_AddAdapterFailed, 0xC000000D).
    InitialData.DxgkDdiCalibrateGpuClock = VioGpu3DDdiCalibrateGpuClock;
    InitialData.DxgkDdiSetStablePowerState = VioGpu3DDdiSetStablePowerState;

    VioGpuReadDriverParameters(pRegistryPath);

    NTSTATUS Status = DxgkInitialize(pDriverObject, pRegistryPath, &InitialData);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkInitialize failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}
// END: Init Code
#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg("PAGE")

//
// PnP DDIs
//

VOID VioGpu3DUnload(VOID)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--> %s\n", __FUNCTION__));
    WPP_CLEANUP(g_pDriverObject);
    g_pDriverObject = NULL;
}

NTSTATUS
VioGpu3DAddDevice(_In_ DEVICE_OBJECT *pPhysicalDeviceObject, _Outptr_ PVOID *ppDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    if ((pPhysicalDeviceObject == NULL) || (ppDeviceContext == NULL))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("One of pPhysicalDeviceObject (%p), ppDeviceContext (%p) is NULL",
                  pPhysicalDeviceObject,
                  ppDeviceContext));
        return STATUS_INVALID_PARAMETER;
    }
    *ppDeviceContext = NULL;

    VioGpuAdapter *pAdapter = new (NonPagedPoolNx) VioGpuAdapter(pPhysicalDeviceObject);
    if (pAdapter == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pAdapter failed to be allocated"));
        return STATUS_NO_MEMORY;
    }

    *ppDeviceContext = pAdapter->ToHandle();

    VioGpuInstallDisplayFixup(pPhysicalDeviceObject);

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s ppDeviceContext = %p\n", __FUNCTION__, pAdapter));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpu3DRemoveDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, pDeviceContext));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);

    if (pAdapter)
    {
        VioGpuRemoveDisplayFixup(pAdapter->GetPhysicalDevice());
        delete pAdapter;
    }

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpu3DStartDevice(_In_ VOID *pDeviceContext,
                    _In_ DXGK_START_INFO *pDxgkStartInfo,
                    _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                    _Out_ ULONG *pNumberOfViews,
                    _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    return pAdapter->StartDevice(pDxgkStartInfo, pDxgkInterface, pNumberOfViews, pNumberOfChildren);
}

NTSTATUS
VioGpu3DStopDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);
    return pAdapter->StopDevice();
}

NTSTATUS
VioGpu3DDispatchIoRequest(_In_ VOID *pDeviceContext,
                          _In_ ULONG VidPnSourceId,
                          _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VioGpuAdapter (0x%I64x) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->DispatchIoRequest(VidPnSourceId, pVideoRequestPacket);
}

NTSTATUS
VioGpu3DSetPowerState(_In_ VOID *pDeviceContext,
                      _In_ ULONG HardwareUid,
                      _In_ DEVICE_POWER_STATE DevicePowerState,
                      _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        return STATUS_SUCCESS;
    }
    return pAdapter->SetPowerState(HardwareUid, DevicePowerState, ActionType);
}

NTSTATUS
VioGpu3DQueryChildRelations(_In_ VOID *pDeviceContext,
                            _Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                            _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    return pAdapter->QueryChildRelations(pChildRelations, ChildRelationsSize);
}

NTSTATUS
VioGpu3DQueryChildStatus(_In_ VOID *pDeviceContext,
                         _Inout_ DXGK_CHILD_STATUS *pChildStatus,
                         _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    return pAdapter->QueryChildStatus(pChildStatus, NonDestructiveOnly);
}

NTSTATUS
VioGpu3DQueryDeviceDescriptor(_In_ VOID *pDeviceContext,
                              _In_ ULONG ChildUid,
                              _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VIOGPU (%p) is being called when not active!", pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->QueryDeviceDescriptor(ChildUid, pDeviceDescriptor);
}

NTSTATUS
APIENTRY
VioGpu3DQueryAdapterInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    return pAdapter->QueryAdapterInfo(pQueryAdapterInfo);
}

NTSTATUS
APIENTRY
VioGpu3DDdiGetNodeMetadata(_In_ CONST HANDLE hAdapter,
                           UINT NodeOrdinal,
                           _Out_ DXGKARG_GETNODEMETADATA *pGetNodeMetadata)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(hAdapter);

    // WDDMv2 packs the physical adapter index into the high word.
    NodeOrdinal = DXGKNODEMETADATA_GETNODEORDINAL(NodeOrdinal);
    if (NodeOrdinal >= 1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Zero the WHOLE out-struct: dxgkrnl appends FriendlyName to the node
    // name (RtlAppendUnicodeStringToString) and an uninitialized array
    // here is a kernel AV (bugcheck 0x7E during adapter init).
    RtlZeroMemory(pGetNodeMetadata, sizeof(*pGetNodeMetadata));
    pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;
    pGetNodeMetadata->Flags.Value = 0;
    // A GpuMmu adapter whose only node claims no GpuMmu leaves VidMm
    // with an inconsistent node description.
    pGetNodeMetadata->GpuMmuSupported = TRUE;
    pGetNodeMetadata->IoMmuSupported = FALSE;

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DSetPointerPosition(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(pSetPointerPosition);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu (%p) is being called when not active!", pAdapter));
        VioGpuDbgBreak();
        return STATUS_UNSUCCESSFUL;
    }
    // Positioning alone never draws anything (no shape is ever programmed --
    // see the zeroed PointerCaps in QueryAdapterInfo), and the DDI is
    // documented to return STATUS_SUCCESS while making no state changes.
    // DxgkDdiSetPointerShape deliberately keeps returning an error instead:
    // success there would claim a hardware pointer had been drawn.
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DSetPointerShape(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(pSetPointerShape);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
APIENTRY
VioGpu3DEscape(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->Escape(pEscape);
}

NTSTATUS
APIENTRY
VioGpu3DCreateAllocation(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEALLOCATION *pCreateAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return VioGpuAllocation::DxgkCreateAllocation(pAdapter, pCreateAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DDescribeAllocation(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAllocation *pAllocation = VioGpuAllocation::FromHandle(pDescribeAllocation->hAllocation);
    VIOGPU_ASSERT_CHK(pAllocation != NULL);

    return pAllocation->DescribeAllocation(pDescribeAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DOpenAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hDevice != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDxContext = VioGpuDevice::FromHandle(hDevice);
    VIOGPU_ASSERT_CHK(pDxContext != NULL);

    return pDxContext->OpenAllocation(pOpenAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DCloseAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_CLOSEALLOCATION *pCloseAllocation)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hDevice != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    for (ULONG i = 0; i < pCloseAllocation->NumAllocations; i++)
    {
        VioGpuDeviceAllocation *pDeviceAllocation = VioGpuDeviceAllocation::FromHandle(pCloseAllocation->pOpenHandleList[i]);
        if (pDeviceAllocation != NULL)
        {
            pDeviceAllocation->GetAllocation()->Close(pDeviceAllocation);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDestroyAllocation(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_DESTROYALLOCATION *pDestroyAllocation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    VIOGPU_ASSERT_CHK(pDestroyAllocation != NULL);

    for (ULONG i = 0; i < pDestroyAllocation->NumAllocations; i++)
    {
        VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pDestroyAllocation->pAllocationList[i]);
        if (allocation != NULL)
        {
            // Release the DXGK reference. Async paths that took an extra
            // ref (e.g. the vsync DPC) complete the deletion.
            allocation->Release();
        }
    }

    if (pDestroyAllocation->Flags.DestroyResource)
    {
        VioGpuResource *resource = VioGpuResource::FromHandle(pDestroyAllocation->hResource);
        if (resource != NULL)
        {
            delete resource;
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s \n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DGetStandardAllocationDriverData(_In_ CONST HANDLE hAdapter,
                                        _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    return VioGpuAllocation::GetStandardAllocationDriverData(pStandardAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DBuildPagingBuffer(_In_ CONST HANDLE hAdapter, _In_ DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    VIOGPU_ASSERT(pBuildPagingBuffer != NULL);
    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s operation=%d\n", __FUNCTION__, pBuildPagingBuffer->Operation));

    // Paging DMA buffers are recycled with their private-data area, so a
    // stale VioGpuCommand* from the buffer's previous user would reach
    // SubmitCommand and corrupt an unrelated in-flight command's fence (see
    // VioGpuDevice::Present).  Skip the clear for the one slot that already
    // carries a stamp this driver placed: VidMm batches several operations
    // into a single paging buffer, and the private data is handed back
    // unadvanced, so every operation in the batch addresses the same slot.
    if (pBuildPagingBuffer->pDmaBufferPrivateData && pBuildPagingBuffer->DmaBufferPrivateDataSize >= sizeof(void *) &&
        pBuildPagingBuffer->pDmaBufferPrivateData != pAdapter->m_PagingStampPriv)
    {
        *(void **)pBuildPagingBuffer->pDmaBufferPrivateData = NULL;
    }

    switch (pBuildPagingBuffer->Operation)
    {
        case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
            {
                if (pBuildPagingBuffer->MapApertureSegment.hAllocation == NULL)
                {
                    // Routine under WDDM2: VidMm pages its own DMA pool
                    // buffers, which have no driver allocation behind them.
                    DbgPrint(TRACE_LEVEL_VERBOSE,
                             ("<--- %s (map aperture segment) no allocation specified\n", __FUNCTION__));
                    return STATUS_SUCCESS;
                }

                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->MapApertureSegment.hAllocation);
                VIOGPU_ASSERT_CHK(allocation != NULL);
                NTSTATUS Status = allocation->MapApertureSegment(pBuildPagingBuffer);
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s (map aperture segment)\n", __FUNCTION__));
                return Status;
            }
        case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
            {
                if (pBuildPagingBuffer->UnmapApertureSegment.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE,
                             ("<--- %s (unmap aperture segment) no allocation specified\n", __FUNCTION__));
                    return STATUS_SUCCESS;
                }

                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->UnmapApertureSegment.hAllocation);
                VIOGPU_ASSERT_CHK(allocation != NULL);
                NTSTATUS Status = allocation->UnmapApertureSegment(pBuildPagingBuffer);
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s (unmap aperture segment)\n", __FUNCTION__));
                return Status;
            }
        case DXGK_OPERATION_FILL:
            {
                if (pBuildPagingBuffer->Fill.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE,
                             ("<--- %s (fill) no allocation specified\n", __FUNCTION__));
                    return STATUS_SUCCESS;
                }


                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->Fill.hAllocation);
                VIOGPU_ASSERT_CHK(allocation != NULL);
                DbgPrint(TRACE_LEVEL_WARNING, ("<--- %s (fill size=%zu pattern=%x segment=%d addr=%p) res_id=%d isBlob=%d\n",
                                               __FUNCTION__,
                                               pBuildPagingBuffer->Fill.FillSize,
                                               pBuildPagingBuffer->Fill.FillPattern,
                                               pBuildPagingBuffer->Fill.Destination.SegmentId,
                                               pBuildPagingBuffer->Fill.Destination.SegmentAddress.QuadPart,
                                               allocation->GetId(),
                                               allocation->IsBlob()));

                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_DISCARD_CONTENT:
            {
                if (pBuildPagingBuffer->DiscardContent.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE,
                             ("<--- %s (discard) no allocation specified\n", __FUNCTION__));
                    return STATUS_SUCCESS;
                }


                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->DiscardContent.hAllocation);
                VIOGPU_ASSERT_CHK(allocation != NULL);
                DbgPrint(TRACE_LEVEL_WARNING, ("<--- %s (discard segment=%d addr=%p) res_id=%d isBlob=%d\n",
                                               __FUNCTION__,
                                               pBuildPagingBuffer->DiscardContent.SegmentId,
                                               pBuildPagingBuffer->DiscardContent.SegmentAddress.QuadPart,
                                               allocation->GetId(),
                                               allocation->IsBlob()));

                // VidMm is freeing this allocation's segment range for reuse.
                // A mappable blob's HOST mapping ideally dies with it (dead
                // processes never send their UMD-side unmap), but no
                // UNMAP_BLOB is emitted from the paging DMA: QEMU's unmap
                // completion is asynchronous (RCU-deferred region teardown)
                // and under process churn a suspended unmap parks the paging
                // fence past dxgkrnl's TDR budget (a ResetFromTimeout within
                // seconds of DISCARD bursts).  The stale-window concern is
                // better fixed host-side by making RES_UNREF drop any live
                // mapping when the blob is destroyed.

                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_NOTIFY_RESIDENCY:
            {
                if (pBuildPagingBuffer->NotifyResidency.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE,
                             ("<--- %s (residency) no allocation specified\n", __FUNCTION__));
                    return STATUS_SUCCESS;
                }

                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->NotifyResidency.hAllocation);
                VIOGPU_ASSERT_CHK(allocation != NULL);
                DbgPrint(TRACE_LEVEL_WARNING, ("<--- %s (residency segment=%u padding=%u off=%p resident=%d) res_id=%d isBlob=%d\n",
                                               __FUNCTION__,
                                               pBuildPagingBuffer->NotifyResidency.PhysicalAddress.SegmentId,
                                               pBuildPagingBuffer->NotifyResidency.PhysicalAddress.Padding,
                                               pBuildPagingBuffer->NotifyResidency.PhysicalAddress.SegmentOffset,
                                               pBuildPagingBuffer->NotifyResidency.Resident,
                                               allocation->GetId(),
                                               allocation->IsBlob()));

                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_TRANSFER:
            {
                // virtio-gpu has no general guest<->guest aperture
                // transfer; eviction-time TRANSFER would need the
                // ATTACH/DETACH_BACKING + host transfer plumbing that
                // does not exist yet. Returning NOT_SUPPORTED makes
                // the scheduler treat the eviction as fatal, so as
                // a stopgap report success: the destination pages
                // will be served zero-initialised at the next attach
                // and we lose the original content rather than
                // wedging the scheduler.
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("<--- %s (transfer, content lost)\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_READ_PHYSICAL:
        case DXGK_OPERATION_WRITE_PHYSICAL:
            {
                // Read / write of a physical page through the
                // adapter is used by the scheduler for diagnostic
                // access; no virtio-gpu equivalent. Report success
                // so the diagnostic does not abort the scheduler.
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("<--- %s (rw_physical op=%d, no-op)\n",
                          __FUNCTION__, pBuildPagingBuffer->Operation));
                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_SIGNAL_MONITORED_FENCE:
            {
                // dxgkrnl expects the GPU to WRITE MonitoredFenceValue to
                // MonitoredFenceGpuVa; it re-evaluates CPU waiters when the
                // paging packet completes, so the write is what satisfies
                // SetEventOnCompletion for any signal routed through the GPU
                // path (which dxgkrnl takes when the context has work in
                // flight).  Performing it here in the build phase is too
                // early -- dxgkrnl prepares packets ahead of execution, and
                // pollers of GetCompletedValue would observe the value before
                // the GPU work completed.  Resolve the GPU VA and pin a
                // kernel mapping of the value cell now (both need PASSIVE),
                // and stamp a command object into the packet's private data;
                // Run() registers the write on the defer list, and the value
                // is published when the completion watermark reaches the
                // packet's own fence id (VioGpuAdapter::MFenceDeferOrWrite).
                const ULONGLONG fenceVa =
                    (ULONGLONG)pBuildPagingBuffer->SignalMonitoredFence.MonitoredFenceGpuVa;
                const UINT64 fenceVal =
                    pBuildPagingBuffer->SignalMonitoredFence.MonitoredFenceValue;
                ULONGLONG phys = 0;
                if (pAdapter == NULL || !pAdapter->VaShadowLookup(fenceVa, &phys))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s monitored-fence signal va=0x%llx val=%llu UNRESOLVED\n",
                              __FUNCTION__, fenceVa, fenceVal));
                    return STATUS_SUCCESS;
                }
                // One stamp per paging buffer: a second signal batched into
                // the same buffer has nowhere to park, so it writes now
                // (early visibility) rather than displacing the first.
                const BOOLEAN slotTaken =
                    (pBuildPagingBuffer->pDmaBufferPrivateData == pAdapter->m_PagingStampPriv);
                // Every fallback below writes the value immediately: early
                // visibility is the lesser evil against a lost signal, which
                // strands whatever waits on the fence.
                if (pBuildPagingBuffer->pDmaBufferPrivateData == NULL ||
                    pBuildPagingBuffer->DmaBufferPrivateDataSize < sizeof(void *) || slotTaken)
                {
                    // No side-band to reach the submit phase.
                    DbgPrint(TRACE_LEVEL_WARNING,
                             ("<--- %s monitored-fence signal va=0x%llx val=%llu written EARLY (%s)\n",
                              __FUNCTION__, fenceVa, fenceVal, slotTaken ? "stamp slot taken" : "no private data"));
                    pAdapter->MFenceWrite(phys, fenceVal);
                    return STATUS_SUCCESS;
                }
                volatile UINT64 *kva = pAdapter->MFenceMapPin(phys);
                if (kva == NULL)
                {
                    // Unmappable, or the map slot is pinned by another page.
                    pAdapter->MFenceWrite(phys, fenceVal);
                    return STATUS_SUCCESS;
                }
                VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(pAdapter);
                if (cmd == NULL)
                {
                    pAdapter->MFenceMapUnpin(phys);
                    pAdapter->MFenceWrite(phys, fenceVal);
                    return STATUS_SUCCESS;
                }
                cmd->SetMFenceWrite(kva, phys, fenceVal);
                // Stamp for BOTH submit paths: the non-virtual SubmitCommand
                // reads the first pointer slot; SubmitCommandVirtual gates
                // recovery on the mirror magic (empty body).
                if (pBuildPagingBuffer->DmaBufferPrivateDataSize >= sizeof(VIOGPU_DMA_PRIVATE))
                {
                    VIOGPU_DMA_PRIVATE *p = (VIOGPU_DMA_PRIVATE *)pBuildPagingBuffer->pDmaBufferPrivateData;
                    p->cmd = cmd->ToHandle();
                    p->magic = VIOGPU_DMA_PRIV_MAGIC;
                    p->bodySize = 0;
                }
                else
                {
                    *(void **)pBuildPagingBuffer->pDmaBufferPrivateData = cmd->ToHandle();
                }
                pAdapter->m_PagingStampPriv = pBuildPagingBuffer->pDmaBufferPrivateData;
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<--- %s monitored-fence signal va=0x%llx val=%llu phys=0x%llx DEFERRED\n",
                          __FUNCTION__, fenceVa, fenceVal, phys));
                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_UPDATE_PAGE_TABLE:
            {
                // GpuMmu, software page tables (GPUMMUCAPS reports
                // ExplicitPageTableInvalidation + CPU_VIRTUAL update mode):
                // nothing on the virtio path consumes GPU VAs, so the PTE
                // writes are accepted and only mirrored into the shadow that
                // resolves monitored-fence signal addresses.
                const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *upt = &pBuildPagingBuffer->UpdatePageTable;
                // GpuMmu residency: these PTE writes are the only paging op
                // that names the system pages behind an allocation (the
                // aperture-segment path never runs under GpuMmu), so the
                // guest backing of TYPE_3D resources is attached/detached
                // from here.
                if (upt->hAllocation != NULL)
                {
                    VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(upt->hAllocation);
                    if (allocation != NULL)
                    {
                        allocation->HandlePageTableUpdate(upt);
                    }
                }
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<--- %s (update page table level=%u start=%u num=%u mode=%d repeat=%u)\n",
                          __FUNCTION__,
                          upt->PageTableLevel,
                          upt->StartIndex,
                          upt->NumPageTableEntries,
                          upt->UpdateMode,
                          upt->Flags.Repeat));

                // Monitored-fence VA shadow.  Only leaf entries name data
                // pages, and Use64KBPages describes a different array
                // (pPageTableEntries64KB) with a 64KB stride than the 4KB
                // walk below -- the driver reports neither DualPteSupported
                // nor 64KB page support, so such an update is not ours to
                // mirror.
                if (pAdapter != NULL && upt->PageTableLevel == 0 && upt->pPageTableEntries != NULL &&
                    upt->NumPageTableEntries > 0 && !upt->Flags.Use64KBPages)
                {
                    const ULONGLONG vaBase = (ULONGLONG)upt->FirstPteVirtualAddress & ~((ULONGLONG)PAGE_SIZE - 1);
                    // Repeat means the array holds exactly ONE entry, whose
                    // value is replicated across the whole range: indexing it
                    // per iteration would read off the end of an OS-owned
                    // allocation.
                    const BOOLEAN repeat = upt->Flags.Repeat ? TRUE : FALSE;
                    // Insertions stay confined to the small updates that map
                    // fence storage; large mappings would only evict them.
                    // Removals are never skipped: a binding left behind
                    // resolves a later signal onto a page that has since been
                    // handed to someone else.
                    const BOOLEAN mayInsert = (upt->NumPageTableEntries <= 16);
                    for (UINT i = 0; i < upt->NumPageTableEntries; i++)
                    {
                        const DXGK_PTE *p = &upt->pPageTableEntries[repeat ? 0 : i];
                        const ULONGLONG va = vaBase + ((ULONGLONG)i << PAGE_SHIFT);
                        // Segment 0 is system memory, and only there is
                        // PageAddress a physical address; in any other
                        // segment it is an offset from that segment's base.
                        // A Zero entry resolves reads to the zero page rather
                        // than to PageAddress, and LargePage covers more than
                        // one page -- neither describes a byte of fence
                        // storage this driver may write through.
                        if (mayInsert && p->Valid && !p->Zero && !p->LargePage && p->Segment == 0)
                        {
                            pAdapter->VaShadowInsert(upt->hProcess, va, (ULONGLONG)p->PageAddress << PAGE_SHIFT);
                        }
                        else
                        {
                            pAdapter->VaShadowRemove(upt->hProcess, va);
                        }
                    }
                }
                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_FLUSH_TLB:
            {
                // No TLB exists -- there is no hardware GPU MMU behind the
                // reported page tables. Success no-op.
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<--- %s (flush tlb root segment=%u offset=0x%llx) no-op\n",
                          __FUNCTION__,
                          pBuildPagingBuffer->FlushTlb.RootPageTableAddress.SegmentId,
                          pBuildPagingBuffer->FlushTlb.RootPageTableAddress.SegmentOffset));
                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES:
        case DXGK_OPERATION_UPDATE_CONTEXT_ALLOCATION:
        case DXGK_OPERATION_VIRTUAL_TRANSFER:
        case DXGK_OPERATION_VIRTUAL_FILL:
        case DXGK_OPERATION_INIT_CONTEXT_RESOURCE:
            {
                // Success no-ops.  COPY_PAGE_TABLE_ENTRIES only moves PTEs
                // when the root page table grows, which the VA shadow does not
                // need to track (it is rebuilt from the leaf updates that
                // follow); VIRTUAL_TRANSFER/VIRTUAL_FILL are the GPU-VA
                // analogues of TRANSFER/FILL and carry the same rationale as
                // TRANSFER above; the context ops carry no state the driver
                // tracks.
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<--- %s (wddm2 op=%d, no-op)\n",
                          __FUNCTION__, pBuildPagingBuffer->Operation));
                return STATUS_SUCCESS;
            }
        default:
            {
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("<--- %s unhandled operation=%d\n",
                          __FUNCTION__, pBuildPagingBuffer->Operation));
                return STATUS_NOT_SUPPORTED;
            }
    };
}

#if 0
NTSTATUS
APIENTRY
VioGpu3DAcquireSwizzlingRange(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_ACQUIRESWIZZLINGRANGE *pAcquireSwizzlingRange)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    VIOGPU_ASSERT(pAcquireSwizzlingRange != NULL);

    VioGpuAllocation *allocation = reinte rpret_cast<VioGpuAllocation *>(pAcquireSwizzlingRange->hAllocation);

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s res_id=%d isBlob=%d, \n", __FUNCTION__, allocation->GetId(), allocation->IsBlob()));
    // TODO: Map blob

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DReleaseSwizzlingRange(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_RELEASESWIZZLINGRANGE *pReleaseSwizzlingRange)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    VIOGPU_ASSERT(pReleaseSwizzlingRange != NULL);

    VioGpuAllocation *allocation = reinter pret_cast<VioGpuAllocation *>(pReleaseSwizzlingRange->hAllocation);
    // TODO: Unmap blob
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s res_id=%d isBlob=%d, \n", __FUNCTION__, allocation->GetId(), allocation->IsBlob()));

    return STATUS_SUCCESS;
}
#endif

// DxgkDdiPatch is documented at PASSIVE_LEVEL and pageable; PAGED_CODE()
// is the tripwire in case dxgmms2 ever calls it higher than that.
NTSTATUS
APIENTRY
VioGpu3DPatch(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        // An error return from the submission DDIs is defined to bugcheck the
        // OS (0x119). dxgkrnl does not submit outside the Start..Stop window,
        // so this only guards a teardown race -- where skipping the address
        // fixups is recoverable and a bugcheck is not.  The packet's fence is
        // retired by the SubmitCommand that follows this call.
        return STATUS_SUCCESS;
    }
    return pAdapter->commander.Patch(pPatch);
};

// DxgkDdiSubmitCommand runs at DISPATCH_LEVEL and must be nonpageable: in
// the PAGE section it pages out under memory pressure and the next
// submission bugchecks D1 (EXECUTE fault at IRQL 2).
#pragma code_seg(push)
#pragma code_seg()
_IRQL_requires_(DISPATCH_LEVEL)
NTSTATUS
APIENTRY
VioGpu3DSubmitCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    // DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    // DbgPrint(TRACE_LEVEL_ERROR, ("Fake imp %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        // See VioGpu3DPatch: an error here is a guaranteed 0x119 bugcheck, so
        // the packet is dropped and its fence retired instead.
        pAdapter->CompleteFenceWithoutWork(pSubmitCommand->SubmissionFenceId,
                                           pSubmitCommand->NodeOrdinal,
                                           pSubmitCommand->EngineOrdinal);
        return STATUS_SUCCESS;
    }
    return pAdapter->commander.SubmitCommand(pSubmitCommand);
};
#pragma code_seg(pop)

NTSTATUS
APIENTRY
VioGpu3DCreateDevice(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEDEVICE *pCreateDevice)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }

    VioGpuDevice *pDevice = new (NonPagedPoolNx) VioGpuDevice(pAdapter);
    if (!pDevice)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s failed to allocate VioGpuDevice\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }
    pCreateDevice->hDevice = pDevice->ToHandle();


    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDestroyDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, pDeviceContext));

    VioGpuDevice *pDxContext = VioGpuDevice::FromHandle(pDeviceContext);

    if (pDxContext)
    {
        delete pDxContext;
    }

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDdiCreateContext(_In_ CONST HANDLE hDevice, _Inout_ DXGKARG_CREATECONTEXT *pCreateContext)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDevice = VioGpuDevice::FromHandle(hDevice);
    VIOGPU_ASSERT_CHK(pDevice != NULL);

    if (pCreateContext->Flags.GdiContext || pCreateContext->Flags.SystemContext) {
        DbgPrint(TRACE_LEVEL_WARNING, ("<---> %s context type: System(%d) GDI(%d) \n",
                                       __FUNCTION__,
                                       pCreateContext->Flags.SystemContext,
                                       pCreateContext->Flags.GdiContext));

        pCreateContext->hContext = pDevice->ToHandle();

        pCreateContext->ContextInfo.DmaBufferSize = 1024 * 1024;
        // Per-command side-band: the mirror struct lets build-phase DDIs
        // carry the DMA body to SubmitCommandVirtual by value (GPU VAs are
        // not CPU-mappable).
        pCreateContext->ContextInfo.DmaBufferPrivateDataSize = sizeof(VIOGPU_DMA_PRIVATE);

        pCreateContext->ContextInfo.AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
        pCreateContext->ContextInfo.PatchLocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;

        // Explicit (zero) context caps; single node, so the paging
        // companion is node 0.
        pCreateContext->ContextInfo.Caps.Value = 0;
        pCreateContext->ContextInfo.PagingCompanionNodeId = 0;
        // DMA buffers must live in the aperture segment (bit 0 =
        // segment 1).  Leaving the set at 0 makes VidMm take its
        // legacy contiguous-memory path (MSDN: "allocates contiguous
        // paged-locked memory") which never creates an allocation
        // object -- and 26100's GPU-VA pool mapping then dereferences
        // that never-created allocation (AddDmaBufferToPool AV at
        // NULL+8, bugcheck 0x3B) when the privileged pool of the CDD
        // context is built.  MSDN: only APERTURE segments may appear
        // in DmaBufferSegmentSet.
        pCreateContext->ContextInfo.DmaBufferSegmentSet = 0x1;

        return STATUS_SUCCESS;
    } else {
        pCreateContext->hContext = pDevice->ToHandle();

        pCreateContext->ContextInfo.DmaBufferSize = 1024 * 1024;
        // Per-command side-band: the mirror struct lets build-phase DDIs
        // carry the DMA body to SubmitCommandVirtual by value (GPU VAs are
        // not CPU-mappable).
        pCreateContext->ContextInfo.DmaBufferPrivateDataSize = sizeof(VIOGPU_DMA_PRIVATE);

        // GpuMmu virtual contexts (Flags.VirtualAddressing) submit by GPU
        // VA through DxgkDdiSubmitCommandVirtual; the allocation/patch-
        // location list sizes are simply unused on that path.
        pCreateContext->ContextInfo.AllocationListSize = 1024;
        pCreateContext->ContextInfo.PatchLocationListSize = 1024;

        // Explicit (zero) context caps; single node, so the paging
        // companion is node 0.
        pCreateContext->ContextInfo.Caps.Value = 0;
        pCreateContext->ContextInfo.PagingCompanionNodeId = 0;
        // Aperture segment only -- see the GDI/System branch above.
        pCreateContext->ContextInfo.DmaBufferSegmentSet = 0x1;
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("<---> %s virtual-addressing context: %d\n",
                  __FUNCTION__,
                  pCreateContext->Flags.VirtualAddressing));

        return STATUS_SUCCESS;
    }
};

NTSTATUS
APIENTRY
VioGpu3DDdiDestroyContext(_In_ CONST HANDLE hContext)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(hContext);

    return STATUS_SUCCESS;
};

//
// WDDM2 (GpuMmu) DDIs.
//

NTSTATUS
APIENTRY
VioGpu3DCreateProcess(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEPROCESS *pCreateProcess)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT_CHK(pCreateProcess != NULL);

    // Pure bookkeeping: the struct pointer is the hKmdProcess handle, and
    // the GPU-VA shadow keys its entries on that value.  There is no root
    // page table behind it -- nothing dereferences GPU VAs in the
    // rendering-bypass model.
    VIOGPU_WDDM2_PROCESS *pProcess = (VIOGPU_WDDM2_PROCESS *)
        ExAllocatePoolZero(NonPagedPoolNx, sizeof(VIOGPU_WDDM2_PROCESS), VIOGPU3D_PROCESS_TAG);
    if (pProcess == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed to allocate process struct\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }

    pProcess->DxgkProcess = pCreateProcess->hDxgkProcess;
    pProcess->Flags = pCreateProcess->Flags;
    pCreateProcess->hKmdProcess = (HANDLE)pProcess;

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<--- %s hDxgkProcess=%p flags=0x%x -> hKmdProcess=%p\n",
              __FUNCTION__,
              pCreateProcess->hDxgkProcess,
              pCreateProcess->Flags.Value,
              pProcess));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDestroyProcess(_In_ CONST HANDLE hAdapter, _In_ CONST HANDLE hKmdProcess)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s hKmdProcess=%p\n", __FUNCTION__, hKmdProcess));


    if (hKmdProcess != NULL)
    {
        ExFreePoolWithTag(hKmdProcess, VIOGPU3D_PROCESS_TAG);
    }
    return STATUS_SUCCESS;
}

// DxgkDdiSubmitCommandVirtual is the GpuMmu replacement for
// DxgkDdiSubmitCommand and runs on the same DISPATCH_LEVEL scheduler path, so
// it must not be pageable.  Its callees -- VioGpuCommander::SubmitCommandVirtual
// and PrepareSubmitVirtual -- are already outside the PAGE section.
#pragma code_seg(push)
#pragma code_seg()
NTSTATUS
APIENTRY
VioGpu3DSubmitCommandVirtual(_In_ CONST HANDLE hAdapter,
                             _In_ CONST DXGKARG_SUBMITCOMMANDVIRTUAL *pSubmitCommandVirtual)
{
    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        // See VioGpu3DSubmitCommand: drop the packet but retire its fence.
        pAdapter->CompleteFenceWithoutWork(pSubmitCommandVirtual->SubmissionFenceId,
                                           pSubmitCommandVirtual->NodeOrdinal,
                                           pSubmitCommandVirtual->EngineOrdinal);
        return STATUS_SUCCESS;
    }
    return pAdapter->commander.SubmitCommandVirtual(pSubmitCommandVirtual);
}
#pragma code_seg(pop)

VOID APIENTRY VioGpu3DSetRootPageTable(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETROOTPAGETABLE *pSetPageTable)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);

    // Bookkeeping no-op: no hardware walks these page tables, so the new
    // root assignment (after a grow/move) only needs to be acknowledged.
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s hContext=%p segment=%u offset=0x%llx entries=%u\n",
              __FUNCTION__,
              pSetPageTable->hContext,
              pSetPageTable->Address.SegmentId,
              pSetPageTable->Address.SegmentOffset,
              pSetPageTable->NumEntries));
}

SIZE_T
APIENTRY
VioGpu3DGetRootPageTableSize(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_GETROOTPAGETABLESIZE *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);

    // dxgkrnl only calls this for 2-level page-table configurations, so with
    // the 4-level geometry reported in GPUMMUCAPS it never runs; the DDI must
    // still be registered.  Answer consistently anyway: round the requested
    // PTE count up to a whole page and report how many entries that fits.
    SIZE_T size = ((SIZE_T)pArgs->NumberOfPte * VIOGPU_WDDM2_PTE_SIZE + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    if (size == 0)
    {
        size = PAGE_SIZE;
    }
    pArgs->NumberOfPte = (UINT)(size / VIOGPU_WDDM2_PTE_SIZE);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s NumberOfPte=%u -> %zu bytes\n", __FUNCTION__, pArgs->NumberOfPte, size));
    return size;
}

NTSTATUS
APIENTRY
VioGpu3DPresent(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_PRESENT *pPresent)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDevice = VioGpuDevice::FromHandle(hContext);
    VIOGPU_ASSERT_CHK(pDevice != NULL);

    return pDevice->Present(pPresent);
}

NTSTATUS
APIENTRY
VioGpu3DRender(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_RENDER *pRender)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDevice = VioGpuDevice::FromHandle(hContext);
    VIOGPU_ASSERT_CHK(pDevice != NULL);

    return pDevice->Render(pRender);
}

NTSTATUS
APIENTRY
VioGpu3DStopDeviceAndReleasePostDisplayOwnership(_In_ VOID *pDeviceContext,
                                                 _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                 _Out_ DXGK_DISPLAY_INFORMATION *DisplayInfo)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    return pAdapter->StopDeviceAndReleasePostDisplayOwnership(TargetId, DisplayInfo);
}

NTSTATUS
APIENTRY
VioGpu3DIsSupportedVidPn(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VIOGPU (%p) is being called when not active!", pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.IsSupportedVidPn(pIsSupportedVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendFunctionalVidPn(_In_ CONST HANDLE hAdapter,
                                 _In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendFunctionalVidPn(pRecommendFunctionalVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendVidPnTopology(_In_ CONST HANDLE hAdapter,
                               _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendVidPnTopology(pRecommendVidPnTopology);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendMonitorModes(_In_ CONST HANDLE hAdapter,
                              _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendMonitorModes(pRecommendMonitorModes);
}

NTSTATUS
APIENTRY
VioGpu3DEnumVidPnCofuncModality(_In_ CONST HANDLE hAdapter,
                                _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.EnumVidPnCofuncModality(pEnumCofuncModality);
}

NTSTATUS
APIENTRY
VioGpu3DSetVidPnSourceVisibility(_In_ CONST HANDLE hAdapter,
                                 _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.SetVidPnSourceVisibility(pSetVidPnSourceVisibility);
}

NTSTATUS
APIENTRY
VioGpu3DCommitVidPn(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.CommitVidPn(pCommitVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DUpdateActiveVidPnPresentPath(_In_ CONST HANDLE hAdapter,
                                     _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.UpdateActiveVidPnPresentPath(pUpdateActiveVidPnPresentPath);
}

NTSTATUS
APIENTRY
VioGpu3DQueryVidPnHWCapability(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.QueryVidPnHWCapability(pVidPnHWCaps);
}

NTSTATUS
APIENTRY
VioGpu3DDdiControlInterrupt(_In_ CONST HANDLE hAdapter,
                            _In_ CONST DXGK_INTERRUPT_TYPE InterruptType,
                            _In_ BOOLEAN EnableInterrupt)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(InterruptType);
    UNREFERENCED_PARAMETER(EnableInterrupt);

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiGetScanLine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_GETSCANLINE *pGetScanLine)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);

    // virtio-gpu has no per-line scanout model. Report always-in-vblank
    // with ScanLine 0; DWM and present-statistics callers treat that as
    // "vblank just occurred, proceed."
    pGetScanLine->InVerticalBlank = TRUE;
    pGetScanLine->ScanLine = 0;
    return STATUS_SUCCESS;
}

// END: Paged Code
#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg()
// BEGIN: Non-Paged Code

VOID VioGpu3DDpcRoutine(_In_ VOID *pDeviceContext)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsHardwareInit())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("VioGpu (%p) is being called when not active!", pAdapter));
        return;
    }
    pAdapter->DpcRoutine();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN
VioGpu3DInterruptRoutine(_In_ VOID *pDeviceContext, _In_ ULONG MessageNumber)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    return pAdapter->InterruptRoutine(MessageNumber);
}

NTSTATUS VioGpu3DSetVidPnSourceAddress(_In_ CONST HANDLE hAdapter,
                                       _In_ CONST DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    pAdapter->vidpn.SetVidPnSourceAddress(pSetVidPnSourceAddress);

    return STATUS_SUCCESS;
}

VOID VioGpu3DResetDevice(_In_ VOID *pDeviceContext)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    pAdapter->ResetDevice();
}

NTSTATUS
APIENTRY
VioGpu3DSystemDisplayEnable(_In_ VOID *pDeviceContext,
                            _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                            _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                            _Out_ UINT *Width,
                            _Out_ UINT *Height,
                            _Out_ D3DDDIFORMAT *ColorFormat)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    return pAdapter->vidpn.SystemDisplayEnable(TargetId, Flags, Width, Height, ColorFormat);
}

VOID APIENTRY VioGpu3DSystemDisplayWrite(_In_ VOID *pDeviceContext,
                                         _In_ VOID *Source,
                                         _In_ UINT SourceWidth,
                                         _In_ UINT SourceHeight,
                                         _In_ UINT SourceStride,
                                         _In_ UINT PositionX,
                                         _In_ UINT PositionY)
{
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    pAdapter->vidpn.SystemDisplayWrite(Source, SourceWidth, SourceHeight, SourceStride, PositionX, PositionY);
}

NTSTATUS
APIENTRY
VioGpu3DDdiPreemptCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PREEMPTCOMMAND *pPreemptCommand)
{
    // DxgkDdiPreemptCommand documents that any error return triggers
    // bugcheck 0x119 (arg1=2). A NULL deref here would also AV-crash
    // the host. Guard the argument and return SUCCESS.
    if (!pPreemptCommand)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s null pPreemptCommand\n", __FUNCTION__));
        return STATUS_SUCCESS;
    }

    // SchedulingCaps.PreemptionAware=1 lets dxgkrnl request engine
    // preemption; PreemptionCaps granularity NONE means it happens only at
    // packet boundaries.  A packet the commander has started is not
    // preemptible -- its body is already on the host and will execute -- so
    // the ack (DXGK_INTERRUPT_DMA_PREEMPTED with the true completed
    // watermark) is raised once that packet completes, and only the queued
    // packets behind it are handed back for resubmission.  Without the ack
    // the scheduler waits forever and declares a hardware hang (TDR).
    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);

    DbgPrint(TRACE_LEVEL_ERROR,
             ("<---> %s PreemptionFenceId=%d node=%u engine=%u\n",
              __FUNCTION__, pPreemptCommand->PreemptionFenceId,
              pPreemptCommand->NodeOrdinal, pPreemptCommand->EngineOrdinal));

    pAdapter->ReportDmaPreempted(pPreemptCommand->PreemptionFenceId,
                                 pPreemptCommand->NodeOrdinal,
                                 pPreemptCommand->EngineOrdinal);

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiRestartFromTimeout(_In_ CONST HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiCancelCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_CANCELCOMMAND *pCancelCommand)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCancelCommand);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiCalibrateGpuClock(_In_ CONST HANDLE hAdapter,
                             _In_ UINT32 NodeOrdinal,
                             _In_ UINT32 EngineOrdinal,
                             _Out_ DXGKARG_CALIBRATEGPUCLOCK *pClockCalibration)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);
    UNREFERENCED_PARAMETER(EngineOrdinal);

    // There is no GPU clock -- submissions complete CPU-side.  Report the
    // CPU performance counter as the "GPU" clock so the two timelines the
    // scheduler correlates are identical and any timestamp math is exact.
    // (Callable at DISPATCH_LEVEL: KeQueryPerformanceCounter is fine.)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter = KeQueryPerformanceCounter(&frequency);

    RtlZeroMemory(pClockCalibration, sizeof(*pClockCalibration));
    pClockCalibration->GpuFrequency = (ULONGLONG)frequency.QuadPart;
    pClockCalibration->GpuClockCounter = (ULONGLONG)counter.QuadPart;
    pClockCalibration->CpuClockCounter = (ULONGLONG)counter.QuadPart;

    return STATUS_SUCCESS;
}

VOID APIENTRY VioGpu3DDdiSetStablePowerState(_In_ CONST HANDLE hAdapter,
                                             _In_ CONST DXGKARG_SETSTABLEPOWERSTATE *pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);

    // No clocks or power states to pin; acknowledging is all that's
    // needed (profiling tools toggle this around benchmark runs).
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<---> %s Enabled=%d (no-op)\n", __FUNCTION__, pArgs ? pArgs->Enabled : -1));
}

NTSTATUS
APIENTRY
VioGpu3DDdiQueryCurrentFence(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYCURRENTFENCE *pCurrentFence)
{
    // UNREFERENCED_PARAMETER(hAdapter);
    // UNREFERENCED_PARAMETER(pCurrentFence);
    // DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);
    pCurrentFence->CurrentFence = InterlockedOr(&pAdapter->m_LastCompletedFenceId, 0);

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiResetEngine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_RESETENGINE *pResetEngine)
{
    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (pResetEngine == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // LastAbortedFenceId is [out] and must be written or the scheduler reads
    // an uninitialised id.  There is one node here, so an engine reset is the
    // adapter-wide fence sync-up: everything submitted is declared
    // aborted/complete and the node is ready for new packets.
    UINT syncedTo = pAdapter->ResetFenceStateFromTimeout();
    pResetEngine->LastAbortedFenceId = syncedTo;

    DbgPrint(TRACE_LEVEL_ERROR,
             ("<---> %s node=%u engine=%u LastAbortedFenceId=%u\n",
              __FUNCTION__,
              pResetEngine->NodeOrdinal,
              pResetEngine->EngineOrdinal,
              syncedTo));

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiQueryEngineStatus(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYENGINESTATUS *pQueryEngineStatus)
{
    UNREFERENCED_PARAMETER(hAdapter);

    // The scheduler calls this when it suspects a node has stopped making
    // progress (before declaring a TDR). The out parameter MUST be filled:
    // leaving EngineStatus untouched lets the scheduler read an
    // uninitialized Responsive bit and treat the engine as hung.
    //
    // This engine is paravirtual -- a worker thread draining a virtqueue
    // against the host renderer, not real hardware that can wedge. Command
    // completion is driven by host fence responses, so the engine is always
    // able to report progress. Report Responsive.
    if (pQueryEngineStatus)
    {
        pQueryEngineStatus->EngineStatus.Value = 0;
        pQueryEngineStatus->EngineStatus.Responsive = 1;
    }

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiCollectDbgInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COLLECTDBGINFO *pCollectDbgInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCollectDbgInfo);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));
    {
        VioGpuAdapter *adapter = VioGpuAdapter::FromHandle(hAdapter);
        if (adapter)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<---> %s fence submitted=%d completed=%d\n", __FUNCTION__,
                      adapter->m_LastSubmittedFenceId, adapter->m_LastCompletedFenceId));
        }
    }

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiResetFromTimeout(_In_ CONST HANDLE hAdapter)
{
    VioGpuAdapter *adapter = VioGpuAdapter::FromHandle(hAdapter);

    if (adapter)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s fence submitted=%d completed=%d\n", __FUNCTION__,
                  adapter->m_LastSubmittedFenceId, adapter->m_LastCompletedFenceId));

        // Sync the fence bookkeeping up to the last submission.  Without this
        // the watermark stays stuck below it after the timeout -- dxgkrnl's
        // post-reset accounting never balances and the node cannot restart.
        // In-flight commands are left to drain naturally (their now-stale
        // completions are squashed by the skip window the sync-up sets);
        // tearing them down here would race the host responses that still
        // reference them.
        UINT syncedTo = adapter->ResetFenceStateFromTimeout();
        DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence state synced to %u\n", __FUNCTION__, syncedTo));
    }

    return STATUS_SUCCESS;
};

#if defined(DBG)

#if defined(COM_DEBUG)

#define RHEL_DEBUG_PORT  ((PUCHAR)0x3F8)
#define TEMP_BUFFER_SIZE 256

void DebugPrintFuncSerial(CONST char *format, ...)
{
    char buf[TEMP_BUFFER_SIZE];
    NTSTATUS status;
    size_t len;
    va_list list;
    va_start(list, format);
    status = RtlStringCbVPrintfA(buf, sizeof(buf), format, list);
    if (status == STATUS_SUCCESS)
    {
        len = strlen(buf);
    }
    else
    {
        len = 2;
        buf[0] = 'O';
        buf[1] = '\n';
    }
    if (len)
    {
        WRITE_PORT_BUFFER_UCHAR(RHEL_DEBUG_PORT, (PUCHAR)buf, (ULONG)len);
        WRITE_PORT_UCHAR(RHEL_DEBUG_PORT, '\r');
    }
    va_end(list);
}
#endif

#if defined(PRINT_DEBUG)
void DebugPrintFuncKdPrint(CONST char *format, ...)
{
    va_list list;
    va_start(list, format);
    vDbgPrintEx(DPFLTR_DEFAULT_ID, 9 | DPFLTR_MASK, format, list);
    va_end(list);
}
#endif

#endif
#pragma code_seg(pop) // End Non-Paged Code

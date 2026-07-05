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
#if !DBG
#include "driver.tmh"
#endif

#pragma code_seg(push)
#pragma code_seg("INIT")

int nDebugLevel;
int virtioDebugLevel;
int bDebugPrint;
int bBreakAlways;

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

    bDebugPrint = 1;
    virtioDebugLevel = 0x5;
    bBreakAlways = 1;
    nDebugLevel = TRACE_LEVEL_INFORMATION;
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
extern "C" NTSTATUS DriverEntry(_In_ DRIVER_OBJECT *pDriverObject, _In_ UNICODE_STRING *pRegistryPath)
{
    PAGED_CODE();
    WPP_INIT_TRACING(pDriverObject, pRegistryPath)
    DbgPrint(TRACE_LEVEL_FATAL, ("---> VIOGPU FULL build on on %s %s\n", __DATE__, __TIME__));
    DRIVER_INITIALIZATION_DATA InitialData = {0};

    InitialData.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;

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
    WPP_CLEANUP(NULL);
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
    UNREFERENCED_PARAMETER(NodeOrdinal);

    if (NodeOrdinal >= 1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;
    pGetNodeMetadata->Flags.Value = 0;

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
    return STATUS_NOT_IMPLEMENTED;
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

    // Paging DMA buffers are recycled with their private-data area; no
    // path below stores a VioGpuCommand*, so a stale pointer from the
    // buffer's previous user would reach SubmitCommand and corrupt an
    // unrelated in-flight command's fence (see VioGpuDevice::Present).
    if (pBuildPagingBuffer->pDmaBufferPrivateData)
    {
        *(void **)pBuildPagingBuffer->pDmaBufferPrivateData = NULL;
    }

    switch (pBuildPagingBuffer->Operation)
    {
        case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
            {
                if (pBuildPagingBuffer->MapApertureSegment.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
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
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (map aperture segment) no allocation specified\n", __FUNCTION__));
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
                    DbgPrint(TRACE_LEVEL_ERROR,
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
                    DbgPrint(TRACE_LEVEL_ERROR,
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
                // processes never send their UMD-side unmap), but emitting
                // UNMAP_BLOB from the paging DMA is DISABLED: QEMU's unmap
                // completion is asynchronous (RCU-deferred region teardown)
                // and under process churn a suspended unmap parks the paging
                // fence past dxgkrnl's TDR budget (ResetFromTimeout observed
                // within seconds of DISCARD bursts, 2026-07-05).  The stale-
                // window concern is better fixed host-side by making
                // RES_UNREF drop any live mapping when the blob is destroyed.
#if 0 /* disabled, see comment above */
                if (allocation->IsBlob() && allocation->IsMappable() && allocation->GetId() != 0)
                {
                    const SIZE_T needed = sizeof(VIOGPU_COMMAND_HDR) + sizeof(UINT);
                    if (pBuildPagingBuffer->pDmaBuffer && pBuildPagingBuffer->DmaSize >= needed)
                    {
                        BYTE *dma = (BYTE *)pBuildPagingBuffer->pDmaBuffer;
                        VIOGPU_COMMAND_HDR hdr;
                        RtlZeroMemory(&hdr, sizeof(hdr));
                        hdr.type = VIOGPU_CMD_UNMAP_BLOB_BY_ID;
                        hdr.size = sizeof(UINT);
                        UINT rid = allocation->GetId();
                        RtlCopyMemory(dma, &hdr, sizeof(hdr));
                        RtlCopyMemory(dma + sizeof(hdr), &rid, sizeof(rid));
                        pBuildPagingBuffer->pDmaBuffer = dma + needed;
                    }
                    else
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("<--- %s discard res_id=%d: paging DMA too small (%u)\n",
                                  __FUNCTION__, allocation->GetId(), pBuildPagingBuffer->DmaSize));
                    }
                }
#endif

                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_NOTIFY_RESIDENCY:
            {
                if (pBuildPagingBuffer->NotifyResidency.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
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
                // The fence-signal operations are paged through the
                // DMA buffer in some scheduler paths; we don't track
                // them but the scheduler does. SUCCESS keeps the
                // pipeline moving; the fence itself is still managed
                // by the normal submit path.
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<--- %s (fence op=%d, deferred to submit path)\n",
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

NTSTATUS
APIENTRY
VioGpu3DPatch(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->commander.Patch(pPatch);
};

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
        // DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__,
        // pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->commander.SubmitCommand(pSubmitCommand);
};

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

        pCreateContext->ContextInfo.DmaBufferSegmentSet = 0;
        pCreateContext->ContextInfo.DmaBufferSize = 1024 * 1024;
        // Per-command side-band: each submission stores one VioGpuCommand*
        // at offset 0 (see Present/Render).
        pCreateContext->ContextInfo.DmaBufferPrivateDataSize = sizeof(VioGpuCommand *);

        pCreateContext->ContextInfo.AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
        pCreateContext->ContextInfo.PatchLocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;

        return STATUS_SUCCESS;
    } else {
        pCreateContext->hContext = pDevice->ToHandle();

        pCreateContext->ContextInfo.DmaBufferSegmentSet = 0;
        pCreateContext->ContextInfo.DmaBufferSize = 1024 * 1024;
        // Per-command side-band: each submission stores one VioGpuCommand*
        // at offset 0 (see Present/Render).
        pCreateContext->ContextInfo.DmaBufferPrivateDataSize = sizeof(VioGpuCommand *);

        pCreateContext->ContextInfo.AllocationListSize = 1024;
        pCreateContext->ContextInfo.PatchLocationListSize = 1024;

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
    // preemption.  The engine cannot actually preempt (PreemptionCaps
    // granularity is NONE), and the host executes everything submitted, so
    // any command dxgkrnl believes is in flight has effectively completed.
    // WDDM requires acknowledging the request by raising
    // DXGK_INTERRUPT_DMA_PREEMPTED with the preemption fence id and the
    // latest completed fence id; without it the GPU scheduler waits on the
    // preempt forever and declares a hardware hang (TDR).
    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);

    DbgPrint(TRACE_LEVEL_ERROR,
             ("<---> %s PreemptionFenceId=%d node=%u engine=%u -> notify DMA_PREEMPTED\n",
              __FUNCTION__, pPreemptCommand->PreemptionFenceId,
              pPreemptCommand->NodeOrdinal, pPreemptCommand->EngineOrdinal));

    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    interrupt.DmaPreempted.PreemptionFenceId = pPreemptCommand->PreemptionFenceId;
    interrupt.DmaPreempted.LastCompletedFenceId =
        (UINT)InterlockedOr(&pAdapter->m_LastCompletedFenceId, 0);
    interrupt.DmaPreempted.NodeOrdinal = pPreemptCommand->NodeOrdinal;
    interrupt.DmaPreempted.EngineOrdinal = pPreemptCommand->EngineOrdinal;
    pAdapter->NotifyInterrupt(&interrupt, true);

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
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pResetEngine);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

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
    UNREFERENCED_PARAMETER(hAdapter);

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

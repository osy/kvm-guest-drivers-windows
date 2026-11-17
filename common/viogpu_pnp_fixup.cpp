/*
 * Copyright (C) 2026 Turing Software, LLC
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

#include "helper.h"
#include "viogpu_pnp_fixup.h"
#if !DBG
#include "viogpu_pnp_fixup.tmh"
#endif

// The original IRP_MJ_PNP dispatch of the PDO's DriverObject, saved so we can
// forward everything we don't handle and restore it on removal. A single
// pointer is enough: virtio-gpu enumerates a single adapter and the hook is
// per-DriverObject, not per-device.
static PDRIVER_DISPATCH gOriginalPnpIrp;

// The PDO the fixup applies to. The patched dispatch belongs to the enumerator
// and is shared by every device it enumerates, so the device object has to be
// matched: only our own PDO's boot configuration wants a framebuffer resource.
static PDEVICE_OBJECT gFixupDeviceObject;

//
// SystemBootGraphicsInformation lets us recover the boot framebuffer range
// that win32k expects to find in the PDO's resource list. These definitions
// are not exposed by the WDK headers.
//
typedef enum _VIOGPU_SYSTEM_INFORMATION_CLASS
{
    VioGpuSystemBootGraphicsInformation = 0x7e
} VIOGPU_SYSTEM_INFORMATION_CLASS;

typedef enum _VIOGPU_SYSTEM_PIXEL_FORMAT
{
    VioGpuSystemPixelFormatUnknown,
    VioGpuSystemPixelFormatR8G8B8,
    VioGpuSystemPixelFormatR8G8B8X8,
    VioGpuSystemPixelFormatB8G8R8,
    VioGpuSystemPixelFormatB8G8R8X8
} VIOGPU_SYSTEM_PIXEL_FORMAT;

typedef struct _VIOGPU_SYSTEM_BOOT_GRAPHICS_INFORMATION
{
    LARGE_INTEGER FrameBuffer;
    ULONG Width;
    ULONG Height;
    ULONG PixelStride;
    ULONG Flags;
    VIOGPU_SYSTEM_PIXEL_FORMAT Format;
    ULONG DisplayRotation;
} VIOGPU_SYSTEM_BOOT_GRAPHICS_INFORMATION, *PVIOGPU_SYSTEM_BOOT_GRAPHICS_INFORMATION;

extern "C" NTSTATUS WINAPI ZwQuerySystemInformation(
    _In_ VIOGPU_SYSTEM_INFORMATION_CLASS SystemInformationClass,
    _Inout_ PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

// OR'd into the minor function of the IRP we send down so our own dispatch can
// recognize the reentry and forward it straight to the original handler
// instead of recursing.
#define IRP_MN_CUSTOM_INJECTED 0x80

static NTSTATUS GetFramebufferAddress(_Out_ ULONGLONG *pStartAddress, _Out_ ULONGLONG *pEndAddress)
{
    NTSTATUS Status;
    VIOGPU_SYSTEM_BOOT_GRAPHICS_INFORMATION SystemBootGraphicsInfo;
    ULONG PixelBytes;

    Status = ZwQuerySystemInformation(VioGpuSystemBootGraphicsInformation,
                                      &SystemBootGraphicsInfo,
                                      sizeof(SystemBootGraphicsInfo),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    if (SystemBootGraphicsInfo.Format == VioGpuSystemPixelFormatB8G8R8)
    {
        PixelBytes = 3;
    }
    else if (SystemBootGraphicsInfo.Format == VioGpuSystemPixelFormatB8G8R8X8)
    {
        PixelBytes = 4;
    }
    else
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    *pStartAddress = SystemBootGraphicsInfo.FrameBuffer.QuadPart;
    *pEndAddress = *pStartAddress + (ULONGLONG)SystemBootGraphicsInfo.Height * SystemBootGraphicsInfo.PixelStride * PixelBytes;

    return STATUS_SUCCESS;
}

static NTSTATUS InjectFramebufferResource(_Inout_ PCM_RESOURCE_LIST *ppResourceList)
{
    NTSTATUS status;
    PCM_RESOURCE_LIST pResourceList;
    SIZE_T resourceListSize;
    PCM_FULL_RESOURCE_DESCRIPTOR list;
    ULONGLONG framebufferStart, framebufferEnd;
    BOOLEAN foundFramebuffer;

    if (*ppResourceList == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    status = GetFramebufferAddress(&framebufferStart, &framebufferEnd);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    pResourceList = *ppResourceList;
    list = pResourceList->List;
    foundFramebuffer = FALSE;

    for (ULONG ix = 0; ix < pResourceList->Count; ++ix)
    {
        /* Process resources in CM_FULL_RESOURCE_DESCRIPTOR block number ix. */

        for (ULONG jx = 0; jx < list->PartialResourceList.Count && !foundFramebuffer; ++jx)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;
            ULONGLONG memoryStart, memoryLength, memoryEnd;

            desc = list->PartialResourceList.PartialDescriptors + jx;

            if (desc->Type != CmResourceTypeMemory && desc->Type != CmResourceTypeMemoryLarge)
            {
                continue;
            }
            memoryLength = RtlCmDecodeMemIoResource(desc, &memoryStart);
            memoryEnd = memoryStart + memoryLength;

            if (framebufferStart >= memoryStart && framebufferEnd <= memoryEnd)
            {
                foundFramebuffer = TRUE;
                break;
            }
        }

        /* Advance to next CM_FULL_RESOURCE_DESCRIPTOR block in memory. */

        list = (PCM_FULL_RESOURCE_DESCRIPTOR)(list->PartialResourceList.PartialDescriptors +
                                              list->PartialResourceList.Count);
    }

    if (!foundFramebuffer)
    {
        CM_FULL_RESOURCE_DESCRIPTOR newRes;
        ULONGLONG framebufferLength;

        /* We need to re-allocate with room for a new resource descriptor */
        resourceListSize = (UINT_PTR)list - (UINT_PTR)pResourceList;
        pResourceList = (PCM_RESOURCE_LIST)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                                       resourceListSize + sizeof(CM_FULL_RESOURCE_DESCRIPTOR),
                                                                       VIOGPUTAG);
        if (!pResourceList)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(pResourceList, *ppResourceList, resourceListSize);
        ExFreePoolWithTag(*ppResourceList, 0);
        RtlZeroMemory(&newRes, sizeof(newRes));
        newRes.PartialResourceList.Version = 1;
        newRes.PartialResourceList.Revision = 1;
        newRes.PartialResourceList.Count = 1;
        framebufferLength = framebufferEnd - framebufferStart;
        if (RtlCmEncodeMemIoResource(&newRes.PartialResourceList.PartialDescriptors[0],
                                     CmResourceTypeMemory,
                                     framebufferLength,
                                     framebufferStart) == STATUS_UNSUCCESSFUL)
        {
            RtlCmEncodeMemIoResource(&newRes.PartialResourceList.PartialDescriptors[0],
                                     CmResourceTypeMemoryLarge,
                                     framebufferLength,
                                     framebufferStart);
        }
        RtlCopyMemory((char *)pResourceList + resourceListSize, &newRes, sizeof(newRes));
        pResourceList->Count++;
        *ppResourceList = pResourceList;
    }
    return STATUS_SUCCESS;
}

static IO_COMPLETION_ROUTINE VioGpuQueryResourcesComplete;

static NTSTATUS VioGpuQueryResourcesComplete(_In_ PDEVICE_OBJECT pDevObj, _In_ PIRP pIrp, _In_ PVOID pContext)
{
    UNREFERENCED_PARAMETER(pDevObj);
    UNREFERENCED_PARAMETER(pIrp);

    KeSetEvent((PKEVENT)pContext, IO_NO_INCREMENT, FALSE);

    // Keeps the IRP alive so the caller can read IoStatus out of it and free it.
    return STATUS_MORE_PROCESSING_REQUIRED;
}

//
// Ask the enumerator for the device's boot configuration. On success the caller
// owns the returned resource list and is responsible for handing it on (or
// freeing it).
//
// The IRP is owned here rather than built with IoBuildSynchronousFsdRequest:
// that helper reports its result by writing the caller's IO_STATUS_BLOCK from
// the I/O manager's special kernel APC, and this dispatch runs inside PnP
// enumeration, where that APC can be deferred past our own return.
//
static NTSTATUS VioGpuQueryBootResources(_In_ PDEVICE_OBJECT pDevObj, _Outptr_result_maybenull_ PCM_RESOURCE_LIST *ppResourceList)
{
    KEVENT event;
    PIRP newIrp;
    PIO_STACK_LOCATION pStack;
    NTSTATUS status;

    *ppResourceList = NULL;

    newIrp = IoAllocateIrp(pDevObj->StackSize, FALSE);
    if (newIrp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    // PnP convention: a bus driver with no boot configuration for the device
    // completes the IRP without touching the status we seed here.
    newIrp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    newIrp->IoStatus.Information = 0;

    pStack = IoGetNextIrpStackLocation(newIrp);
    pStack->MajorFunction = IRP_MJ_PNP;
    pStack->MinorFunction = IRP_MN_QUERY_RESOURCES | IRP_MN_CUSTOM_INJECTED;

    IoSetCompletionRoutine(newIrp, VioGpuQueryResourcesComplete, &event, TRUE, TRUE, TRUE);

    // The completion routine runs on every outcome, so the event is always
    // signalled and the IRP is always still ours, whether the lower driver
    // completed inline or pended.
    (void)IoCallDriver(pDevObj, newIrp);
    KeWaitForSingleObject(&event,
                          Executive,  // WaitReason
                          KernelMode, // must be Kernelmode to prevent the stack getting paged out
                          FALSE,
                          NULL // indefinite wait
    );

    status = newIrp->IoStatus.Status;
    if (NT_SUCCESS(status))
    {
        *ppResourceList = (PCM_RESOURCE_LIST)newIrp->IoStatus.Information;
    }
    IoFreeIrp(newIrp);

    return status;
}

static NTSTATUS VioGpuDisplayFixupPnpIrp(IN PDEVICE_OBJECT pDevObj, IN PIRP pIrp)
{
    PIO_STACK_LOCATION pStack;
    NTSTATUS status;
    PCM_RESOURCE_LIST pResourceList;

    pStack = IoGetCurrentIrpStackLocation(pIrp);
    if (pStack->MajorFunction != IRP_MJ_PNP || pStack->MinorFunction != IRP_MN_QUERY_RESOURCES ||
        pDevObj != gFixupDeviceObject)
    {
        if (pStack->MajorFunction == IRP_MJ_PNP)
        {
            // This is the IRP we injected below reentering our dispatch; unset
            // the custom flag and let the original handler service it.
            pStack->MinorFunction &= ~IRP_MN_CUSTOM_INJECTED;
        }
        return gOriginalPnpIrp(pDevObj, pIrp);
    }

    // Only modify IRP_MN_QUERY_RESOURCES
    status = VioGpuQueryBootResources(pDevObj, &pResourceList);
    if (!NT_SUCCESS(status) || pResourceList == NULL)
    {
        // The enumerator has no boot configuration for this device, so there is
        // no list to add the framebuffer to. Forwarding the original IRP leaves
        // the stack behaving as if the dispatch were never patched.
        DbgPrint(TRACE_LEVEL_WARNING, ("IRP_MN_QUERY_RESOURCES yielded no resource list (0x%x), skipping fixup\n", status));
        return gOriginalPnpIrp(pDevObj, pIrp);
    }

    // pResourceList is owned here and passed up on the original IRP. A failure
    // to inject is not worth failing the query over.
    status = InjectFramebufferResource(&pResourceList);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("Failed to inject framebuffer resource (0x%x)\n", status));
    }

    pIrp->IoStatus.Information = (ULONG_PTR)pResourceList;
    pIrp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

void VioGpuInstallDisplayFixup(_In_ PDEVICE_OBJECT pPhysicalDeviceObject)
{
    PDRIVER_OBJECT pDriverObject = pPhysicalDeviceObject->DriverObject;

    // Recorded unconditionally: a reinstall runs AddDevice again against the
    // same enumerator, whose dispatch may already be patched at that point.
    gFixupDeviceObject = pPhysicalDeviceObject;

    if (gOriginalPnpIrp == NULL)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("Patching IRP_MJ_PNP to workaround double display bug\n"));
        gOriginalPnpIrp = pDriverObject->MajorFunction[IRP_MJ_PNP];
        pDriverObject->MajorFunction[IRP_MJ_PNP] = VioGpuDisplayFixupPnpIrp;
    }
}

void VioGpuRemoveDisplayFixup(_In_ PDEVICE_OBJECT pPhysicalDeviceObject)
{
    PDRIVER_OBJECT pDriverObject = pPhysicalDeviceObject->DriverObject;

    if (gOriginalPnpIrp == NULL)
    {
        return;
    }

    // A driver that patched the dispatch on top of us cannot be unlinked, and
    // restoring the saved pointer would drop it out of the chain.
    if (pDriverObject->MajorFunction[IRP_MJ_PNP] != VioGpuDisplayFixupPnpIrp)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("IRP_MJ_PNP was re-patched by another driver, leaving it alone\n"));
        return;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("Removing IRP_MJ_PNP patch\n"));
    pDriverObject->MajorFunction[IRP_MJ_PNP] = gOriginalPnpIrp;
    gOriginalPnpIrp = NULL;
    gFixupDeviceObject = NULL;
}

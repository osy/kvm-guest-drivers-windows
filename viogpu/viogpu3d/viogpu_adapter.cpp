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

#include "helper.h"
#include "driver.h"
#include "viogpu_adapter.h"
#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "viogpu_device.h"
#if !DBG
#include "viogpudo.tmh"
#endif

static UINT g_InstanceId = 0;

// HWClose waits this many 1s rounds for the worker thread before giving up on
// it (and leaking it rather than freeing the adapter underneath it).
#define HW_CLOSE_THREAD_WAIT_RETRIES 5

struct NOTIFY_CONTEXT
{
    DXGKRNL_INTERFACE *pDxgkInterface;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA *interrupt;
    BOOL triggerDpc;
};

BOOLEAN NotifyRoutine(PVOID ctx_void)
{
    // DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s\n", __FUNCTION__));
    NOTIFY_CONTEXT *ctx = (NOTIFY_CONTEXT *)ctx_void;
    DXGKRNL_INTERFACE *pDxgkInterface = ctx->pDxgkInterface;
    pDxgkInterface->DxgkCbNotifyInterrupt(pDxgkInterface->DeviceHandle, ctx->interrupt);
    if (ctx->triggerDpc)
    {
        pDxgkInterface->DxgkCbQueueDpc(pDxgkInterface->DeviceHandle);
    }

    return TRUE;
}

// Fence reporting, executed under the interrupt spinlock via
// DxgkCbSynchronizeExecution.  No DbgPrint in the sync routines (DIRQL);
// callers log from the returned outcome.
struct FENCE_REPORT_CONTEXT
{
    VioGpuAdapter *pAdapter;
    DXGKRNL_INTERFACE *pDxgk;
    UINT FenceId; // completed id, or preempt fence id
    UINT Node;
    UINT Engine;
    BOOLEAN Raised;
    // Preempt ack: deferred monitored-fence writes dropped from the
    // defer list (dxgkrnl resubmits their packets); the caller completes
    // them without writing.
    LIST_ENTRY Dropped;
    // Completed path: deferred monitored-fence writes the advanced
    // watermark released; the caller writes + completes them.
    LIST_ENTRY Ready;
};

static void MFenceDeferWriteReadyLocked(VioGpuAdapter *a, LIST_ENTRY *done);

static BOOLEAN ReportCompletedSyncRoutine(PVOID ctx_void)
{
    FENCE_REPORT_CONTEXT *ctx = (FENCE_REPORT_CONTEXT *)ctx_void;
    VioGpuAdapter *a = ctx->pAdapter;
    DXGKRNL_INTERFACE *dxgk = ctx->pDxgk;

    const LONG last = a->m_LastCompletedFenceId;
    if ((LONG)(ctx->FenceId - (UINT)last) <= 0)
    {
        ctx->Raised = FALSE; // duplicate/regressing
        return TRUE;
    }
    const LONG skipThrough = a->m_PreemptSkipThroughFenceId;
    if ((LONG)(ctx->FenceId - (UINT)skipThrough) <= 0)
    {
        ctx->Raised = FALSE; // dxgkrnl re-owned this packet at preempt ack
        return TRUE;
    }
    // Completion-contiguity window: record this id and raise only the highest
    // contiguously-completed one.  Node fence ids are dense (dxgkrnl
    // increments per submission), so byte i of m_Win marks
    // (m_WinBase + 1 + i) complete.
    UINT off = ctx->FenceId - a->m_WinBase;
    if ((LONG)off <= 0)
    {
        ctx->Raised = FALSE; // at/behind the window base: duplicate
        return TRUE;
    }
    if (off > VioGpuAdapter::VIOGPU_FENCE_WINDOW)
    {
        // Window overflow (>256 in-flight submissions): degrade to the
        // pre-window watermark jump rather than wedge.  Loud -- a jump can
        // falsely complete a parked gate.
        RtlZeroMemory(a->m_Win, sizeof(a->m_Win));
        a->m_WinBase = ctx->FenceId;
        a->m_LastCompletedFenceId = (LONG)ctx->FenceId;
    }
    else
    {
        a->m_Win[off - 1] = 1;
        UINT adv = 0;
        while (adv < VioGpuAdapter::VIOGPU_FENCE_WINDOW && a->m_Win[adv])
        {
            adv++;
        }
        if (adv == 0)
        {
            ctx->Raised = FALSE; // recorded; an earlier id is still pending
            return TRUE;
        }
        RtlMoveMemory(a->m_Win, a->m_Win + adv, VioGpuAdapter::VIOGPU_FENCE_WINDOW - adv);
        RtlZeroMemory(a->m_Win + (VioGpuAdapter::VIOGPU_FENCE_WINDOW - adv), adv);
        a->m_WinBase += adv;
        if ((LONG)(a->m_WinBase - (UINT)a->m_LastCompletedFenceId) <= 0)
        {
            ctx->Raised = FALSE;
            return TRUE;
        }
        a->m_LastCompletedFenceId = (LONG)a->m_WinBase;
    }

    // Publish every fence value the new watermark covers BEFORE telling
    // dxgkrnl the packet completed: the DPC the interrupt queues can wake
    // a waiter that reads the fence memory immediately.
    MFenceDeferWriteReadyLocked(a, &ctx->Ready);

    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    interrupt.DmaCompleted.SubmissionFenceId = (UINT)a->m_LastCompletedFenceId;
    interrupt.DmaCompleted.NodeOrdinal = ctx->Node;
    interrupt.DmaCompleted.EngineOrdinal = ctx->Engine;
    dxgk->DxgkCbNotifyInterrupt(dxgk->DeviceHandle, &interrupt);
    dxgk->DxgkCbQueueDpc(dxgk->DeviceHandle);
    ctx->Raised = TRUE;
    return TRUE;
}

static BOOLEAN ReportPreemptedSyncRoutine(PVOID ctx_void)
{
    FENCE_REPORT_CONTEXT *ctx = (FENCE_REPORT_CONTEXT *)ctx_void;
    VioGpuAdapter *a = ctx->pAdapter;
    DXGKRNL_INTERFACE *dxgk = ctx->pDxgk;

    // dxgkrnl defines DmaPreempted.LastCompletedFenceId as the last id the
    // GPU finished BEFORE the preemption, so report the watermark as it
    // stands and advance it afterwards.
    const UINT lastCompleted = (UINT)a->m_LastCompletedFenceId;

    // Everything submitted but not completed is declared preempted;
    // squash their original-id completions from here on (dxgkrnl
    // resubmits them under newer ids).
    a->m_PreemptSkipThroughFenceId = a->m_LastSubmittedFenceId;

    // Parked gate packets are in the acked window too: dxgkrnl re-owns and
    // resubmits them (the resubmission re-parks on the same token), and
    // everything deferred behind them is <= skipThrough -- squashed.  Clear
    // the hold state so the stale ids can't pin the watermark forever.
    RtlZeroMemory(a->m_Win, sizeof(a->m_Win));
    if ((LONG)((UINT)a->m_PreemptSkipThroughFenceId - a->m_WinBase) > 0)
    {
        a->m_WinBase = (UINT)a->m_PreemptSkipThroughFenceId;
    }
    // m_WinBase and m_LastCompletedFenceId are one watermark expressed twice,
    // and the deferred monitored-fence gate keys on the latter: leaving it
    // behind makes MFenceDeferReadyLocked unsatisfiable
    // for the first id dxgkrnl resubmits, which parks that packet forever and
    // pins the window base it would have advanced.
    if ((LONG)(a->m_WinBase - (UINT)a->m_LastCompletedFenceId) > 0)
    {
        a->m_LastCompletedFenceId = (LONG)a->m_WinBase;
    }

    // Parked monitored-fence writes are re-owned too: drop them without
    // writing (the resubmitted paging packet re-executes the write); the
    // caller completes their packets so the commander drains.
    while (!IsListEmpty(&a->m_MFenceDeferList))
    {
        InsertTailList(&ctx->Dropped, RemoveHeadList(&a->m_MFenceDeferList));
    }

    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    interrupt.DmaPreempted.PreemptionFenceId = ctx->FenceId;
    interrupt.DmaPreempted.LastCompletedFenceId = lastCompleted;
    interrupt.DmaPreempted.NodeOrdinal = ctx->Node;
    interrupt.DmaPreempted.EngineOrdinal = ctx->Engine;
    dxgk->DxgkCbNotifyInterrupt(dxgk->DeviceHandle, &interrupt);
    dxgk->DxgkCbQueueDpc(dxgk->DeviceHandle);
    ctx->Raised = TRUE;
    return TRUE;
}

static BOOLEAN ResetFenceStateSyncRoutine(PVOID ctx_void)
{
    FENCE_REPORT_CONTEXT *ctx = (FENCE_REPORT_CONTEXT *)ctx_void;
    VioGpuAdapter *a = ctx->pAdapter;

    // Same shape as the preempt ack, minus the interrupt: after a reset
    // dxgkrnl owns every packet it submitted, so declare them all complete,
    // squash their in-flight original-id completions (the host can still
    // answer for work we can no longer match), and re-base the contiguity
    // window on the new watermark.
    a->m_PreemptSkipThroughFenceId = a->m_LastSubmittedFenceId;
    RtlZeroMemory(a->m_Win, sizeof(a->m_Win));
    a->m_WinBase = (UINT)a->m_LastSubmittedFenceId;
    a->m_LastCompletedFenceId = a->m_LastSubmittedFenceId;

    // Deferred monitored-fence writes belong to packets dxgkrnl re-owns:
    // drop them without writing, exactly as the preempt path does.  The
    // caller completes their packets so the commander can drain.
    while (!IsListEmpty(&a->m_MFenceDeferList))
    {
        InsertTailList(&ctx->Dropped, RemoveHeadList(&a->m_MFenceDeferList));
    }

    ctx->FenceId = (UINT)a->m_LastCompletedFenceId;
    ctx->Raised = TRUE;
    return TRUE;
}

UINT VioGpuAdapter::ResetFenceStateFromTimeout(void)
{
    FENCE_REPORT_CONTEXT ctx = {};
    ctx.pAdapter = this;
    ctx.pDxgk = &m_DxgkInterface;
    InitializeListHead(&ctx.Dropped);
    InitializeListHead(&ctx.Ready);
    BOOLEAN bRet;
    m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle,
                                               ResetFenceStateSyncRoutine, &ctx, 0, &bRet);
    while (!IsListEmpty(&ctx.Dropped))
    {
        MFENCE_DEFER *d = CONTAINING_RECORD(RemoveHeadList(&ctx.Dropped), MFENCE_DEFER, Entry);
        MFenceMapUnpin(d->Phys); // dropped unwritten: dxgkrnl re-owns the packet
        delete d;
    }
    return ctx.FenceId;
}

BOOLEAN VioGpuAdapter::ReportDmaCompleted(UINT fenceId, UINT node, UINT engine)
{
    FENCE_REPORT_CONTEXT ctx = {};
    ctx.pAdapter = this;
    ctx.pDxgk = &m_DxgkInterface;
    ctx.FenceId = fenceId;
    ctx.Node = node;
    ctx.Engine = engine;
    InitializeListHead(&ctx.Dropped);
    InitializeListHead(&ctx.Ready);
    BOOLEAN bRet;
    m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle,
                                               ReportCompletedSyncRoutine, &ctx, 0, &bRet);
    // Values were published inside the sync routine (before the
    // interrupt); only the bookkeeping happens out here.
    while (!IsListEmpty(&ctx.Ready))
    {
        MFENCE_DEFER *d = CONTAINING_RECORD(RemoveHeadList(&ctx.Ready), MFENCE_DEFER, Entry);
        MFenceMapUnpin(d->Phys);
        delete d;
    }
    return ctx.Raised;
}

void VioGpuAdapter::ReportDmaPreempted(UINT preemptFenceId, UINT node, UINT engine)
{
    FENCE_REPORT_CONTEXT ctx = {};
    ctx.pAdapter = this;
    ctx.pDxgk = &m_DxgkInterface;
    ctx.FenceId = preemptFenceId;
    ctx.Node = node;
    ctx.Engine = engine;
    InitializeListHead(&ctx.Dropped);
    InitializeListHead(&ctx.Ready);
    BOOLEAN bRet;
    m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle,
                                               ReportPreemptedSyncRoutine, &ctx, 0, &bRet);
    while (!IsListEmpty(&ctx.Dropped))
    {
        MFENCE_DEFER *d = CONTAINING_RECORD(RemoveHeadList(&ctx.Dropped), MFENCE_DEFER, Entry);
        MFenceMapUnpin(d->Phys); // dropped unwritten: dxgkrnl re-owns the packet
        delete d;
    }
}

void VioGpuAdapter::CompleteFenceWithoutWork(UINT fenceId, UINT node, UINT engine)
{
    InterlockedExchange(&m_LastSubmittedFenceId, (LONG)fenceId);
    ReportDmaCompleted(fenceId, node, engine);
}

NTSTATUS VioGpuAdapter::NotifyInterrupt(DXGKARGCB_NOTIFY_INTERRUPT_DATA *interruptData, BOOL triggerDpc)
{
    NOTIFY_CONTEXT notify;
    notify.pDxgkInterface = &m_DxgkInterface;
    notify.interrupt = interruptData;
    notify.triggerDpc = triggerDpc;
    BOOLEAN bRet;
    return m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle, NotifyRoutine, &notify, 0, &bRet);
}

virtio_gpu_formats ColorFormat(UINT format)
{
    switch (format)
    {
        case D3DDDIFMT_A8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        case D3DDDIFMT_X8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        case D3DDDIFMT_A8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
        case D3DDDIFMT_X8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
}

PAGED_CODE_SEG_BEGIN

VioGpuAdapter::VioGpuAdapter(_In_ DEVICE_OBJECT *pPhysicalDeviceObject)
    : m_pPhysicalDevice(pPhysicalDeviceObject), m_MonitorPowerState(PowerDeviceD0), m_AdapterPowerState(PowerDeviceD0),
      commander(this), vidpn(this)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    *((UINT *)&m_Flags) = 0;
    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));
    RtlZeroMemory(&m_PointerShape, sizeof(m_PointerShape));
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    RtlZeroMemory(&m_VioDev, sizeof(m_VioDev));
    RtlZeroMemory(m_vaShadow, sizeof(m_vaShadow));
    RtlZeroMemory(m_MFenceMap, sizeof(m_MFenceMap));
    KeInitializeSpinLock(&m_MFenceMapLock);
    m_Id = g_InstanceId++;
    m_PendingWorks = 0;
    // Pool memory is NOT zeroed, and the vsync fence echo reports these
    // as completed ids from the first tick.
    m_LastCompletedFenceId = 0;
    m_PreemptSkipThroughFenceId = 0;
    m_LastSubmittedFenceId = 0;
    KeInitializeEvent(&m_ConfigUpdateEvent, SynchronizationEvent, FALSE);
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;
    m_ResolutionEvent = NULL;
    m_ResolutionEventHandle = NULL;
    m_u32NumCapsets = 0;
    m_u32NumScanouts = 0;
    KeInitializeSpinLock(&m_ThreadTokenLock);
    KeInitializeSpinLock(&m_PresentTokenLock);
    // Present-fence completion contexts: fixed-size, allocated at Escape
    // (PASSIVE) and freed from the response DPC (DISPATCH), which is exactly
    // the lookaside contract.
    ExInitializeNPagedLookasideList(&m_PresentFenceLookaside,
                                    NULL,
                                    NULL,
                                    POOL_NX_ALLOCATION,
                                    sizeof(PRESENT_FENCE_CTX),
                                    'fPgV',
                                    0);
    KeInitializeSpinLock(&m_PendingCreateLock);
    InitializeListHead(&m_PendingCreateList);
    InitializeListHead(&m_KmtMapList);
    InitializeListHead(&m_CookieMapList);
    KeInitializeSpinLock(&m_GateLock);
    InitializeListHead(&m_GateList);
    InitializeListHead(&m_PresentWaitList);
    InitializeListHead(&m_MFenceDeferList);
    // Parked-gate deadline scan.  Started here and cancelled in the
    // destructor; an empty-list pass is a spinlock acquire + list-head check
    // every period, which is noise.
    KeInitializeTimer(&m_GateExpiryTimer);
    KeInitializeDpc(&m_GateExpiryDpc, GateExpiryDpcRoutine, this);
    LARGE_INTEGER gateDue;
    gateDue.QuadPart = -10000LL * VIOGPU_GATE_EXPIRY_PERIOD_MS;
    KeSetTimerEx(&m_GateExpiryTimer, gateDue, VIOGPU_GATE_EXPIRY_PERIOD_MS, &m_GateExpiryDpc);
}

VioGpuAdapter::~VioGpuAdapter(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    CloseResolutionEvent();
    // Stop the gate-expiry scan before the list is torn down; flush so a
    // DPC already in flight has finished touching the adapter.
    KeCancelTimer(&m_GateExpiryTimer);
    KeFlushQueuedDpcs();
    VioGpuAdapterClose();
    HWClose();
    ExDeleteNPagedLookasideList(&m_PresentFenceLookaside);

    // Gate entries: every armed entry's event-ring fence has completed by
    // now (HWClose drains the ctrl queue), so anything left is a fired
    // entry whose VIOGPU_CMD_GATE never arrived (guest died between the
    // escape and the submit).  Parked consumers cannot exist here -- their
    // packets would still be running -- but complete any defensively so
    // the commander can drain.
    while (!IsListEmpty(&m_GateList))
    {
        GATE_TOKEN_CTX *g = CONTAINING_RECORD(RemoveHeadList(&m_GateList), GATE_TOKEN_CTX, Entry);
        while (!IsListEmpty(&g->Consumers))
        {
            GATE_CONSUMER *c = CONTAINING_RECORD(RemoveHeadList(&g->Consumers), GATE_CONSUMER, Entry);
            c->Cb(c->Ctx, NULL, NULL);
            delete c;
        }
        delete g;
    }
    // Packet-gated presents parked on a token that never retired: complete
    // them so the commander can drain (same contract as the gate consumers
    // above).
    while (!IsListEmpty(&m_PresentWaitList))
    {
        PRESENT_WAIT_CONSUMER *w =
            CONTAINING_RECORD(RemoveHeadList(&m_PresentWaitList), PRESENT_WAIT_CONSUMER, Entry);
        w->Cb(w->Ctx, NULL, NULL);
        delete w;
    }
    while (!IsListEmpty(&m_MFenceDeferList))
    {
        MFENCE_DEFER *d = CONTAINING_RECORD(RemoveHeadList(&m_MFenceDeferList), MFENCE_DEFER, Entry);
        // Teardown: no further completions are coming, so write now
        // (waiters get their values) before the mappings go away.
        *d->Kva = d->Value;
        KeMemoryBarrier();
        MFenceMapUnpin(d->Phys);
        delete d;
    }
    MFenceMapRelease();
    m_Id = 0;
}

BOOLEAN VioGpuAdapter::CheckHardware()
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_GRAPHICS_DRIVER_MISMATCH;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCI_COMMON_HEADER Header = {0};
    ULONG BytesRead;

    Status = m_DxgkInterface.DxgkCbReadDeviceSpace(m_DxgkInterface.DeviceHandle,
                                                   DXGK_WHICHSPACE_CONFIG,
                                                   &Header,
                                                   0,
                                                   sizeof(Header),
                                                   &BytesRead);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbReadDeviceSpace failed with status 0x%X\n", Status));
        return FALSE;
    }
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s VendorId = 0x%04X DeviceId = 0x%04X\n", __FUNCTION__, Header.VendorID, Header.DeviceID));
    if (Header.VendorID == REDHAT_PCI_VENDOR_ID && Header.DeviceID == 0x1050)
    {
        SetVgaDevice(Header.SubClass == PCI_SUBCLASS_VID_VGA_CTLR);
        return TRUE;
    }

    return FALSE;
}

#pragma warning(disable : 4702)
NTSTATUS VioGpuAdapter::StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                                    _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                                    _Out_ ULONG *pNumberOfViews,
                                    _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();

    NTSTATUS Status;
    VIOGPU_ASSERT(pDxgkStartInfo != NULL);
    VIOGPU_ASSERT(pDxgkInterface != NULL);
    VIOGPU_ASSERT(pNumberOfViews != NULL);
    VIOGPU_ASSERT(pNumberOfChildren != NULL);
    RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, sizeof(m_DxgkInterface));

    Status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle, &m_DeviceInfo);
    if (!NT_SUCCESS(Status))
    {
        VIOGPU_LOG_ASSERTION1("DxgkCbGetDeviceInformation failed with status 0x%X\n", Status);
        return Status;
    }

    if (!CheckHardware())
    {
        Status = STATUS_NO_MEMORY;
        DbgPrint(TRACE_LEVEL_ERROR, ("StartDevice failed to allocate memory\n"));
        return Status;
    }

    Status = GetRegisterInfo();
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("GetRegisterInfo failed with status 0x%X\n", Status));
    }

    Status = GetPCIInfo();
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("GetPCIInfo failed with status 0x%X\n", Status));
    }

    Status = HWInit(m_DeviceInfo.TranslatedResourceList);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("HWInit failed with status 0x%X\n", Status));
        return Status;
    }

    if (!AckFeature(VIRTIO_GPU_F_VIRGL))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu3D cannot start because virgl is not enabled\n"));
        return STATUS_UNSUCCESSFUL;
    }

    if (!AckFeature(VIRTIO_GPU_F_RESOURCE_BLOB))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu3D cannot start because blob resources are not enabled\n"));
        return STATUS_UNSUCCESSFUL;
    }

    if (!AckFeature(VIRTIO_GPU_F_CONTEXT_INIT))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu3D cannot start because context init is not enabled\n"));
        return STATUS_UNSUCCESSFUL;
    }

    Status = SetRegisterInfo(GetInstanceId(), 0);
    if (!NT_SUCCESS(Status))
    {
        VIOGPU_LOG_ASSERTION1("RegisterHWInfo failed with status 0x%X\n", Status);
        return Status;
    }

    Status = commander.Start();
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("VioGpuCommander::Start failed with status 0x%X\n", Status));
        VioGpuDbgBreak();
        return Status;
    }

    Status = vidpn.Start(pNumberOfViews, pNumberOfChildren);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("VioGpuVidPN::Start failed with status 0x%X\n", Status));
        VioGpuDbgBreak();
        // A failed StartDevice is followed by RemoveDevice, never StopDevice,
        // so the commander worker has to be torn down here or it outlives the
        // adapter it dereferences.
        commander.Stop();
        return STATUS_UNSUCCESSFUL;
    }

    m_Flags.DriverStarted = TRUE;

    m_AdapterLuid = pDxgkStartInfo->AdapterLuid;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::StopDevice(VOID)
{
    PAGED_CODE();
    commander.Stop();

    m_Flags.DriverStarted = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::DispatchIoRequest(_In_ ULONG VidPnSourceId, _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(pVideoRequestPacket);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    // The 3D driver does not implement any video IOCTLs; reporting
    // STATUS_SUCCESS would let callers read uninitialized response
    // data as if it had been populated.
    return STATUS_NOT_SUPPORTED;
}

PCHAR
DbgDevicePowerString(__in DEVICE_POWER_STATE Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerDeviceUnspecified:
            return "PowerDeviceUnspecified";
        case PowerDeviceD0:
            return "PowerDeviceD0";
        case PowerDeviceD1:
            return "PowerDeviceD1";
        case PowerDeviceD2:
            return "PowerDeviceD2";
        case PowerDeviceD3:
            return "PowerDeviceD3";
        case PowerDeviceMaximum:
            return "PowerDeviceMaximum";
        default:
            return "UnKnown Device Power State";
    }
}

PCHAR
DbgPowerActionString(__in POWER_ACTION Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerActionNone:
            return "PowerActionNone";
        case PowerActionReserved:
            return "PowerActionReserved";
        case PowerActionSleep:
            return "PowerActionSleep";
        case PowerActionHibernate:
            return "PowerActionHibernate";
        case PowerActionShutdown:
            return "PowerActionShutdown";
        case PowerActionShutdownReset:
            return "PowerActionShutdownReset";
        case PowerActionShutdownOff:
            return "PowerActionShutdownOff";
        case PowerActionWarmEject:
            return "PowerActionWarmEject";
        default:
            return "UnKnown Device Power State";
    }
}

NTSTATUS VioGpuAdapter::SetPowerState(_In_ ULONG HardwareUid,
                                      _In_ DEVICE_POWER_STATE DevicePowerState,
                                      _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ActionType);

    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s HardwareUid = 0x%x ActionType = %s DevicePowerState = %s AdapterPowerState = %s\n",
              __FUNCTION__,
              HardwareUid,
              DbgPowerActionString(ActionType),
              DbgDevicePowerString(DevicePowerState),
              DbgDevicePowerString(m_AdapterPowerState)));

    if (HardwareUid == DISPLAY_ADAPTER_HW_ID)
    {
        if (DevicePowerState == PowerDeviceD0)
        {
            vidpn.AcquirePostDisplayOwnership();

            if (m_AdapterPowerState == PowerDeviceD3)
            {
                DXGKARG_SETVIDPNSOURCEVISIBILITY Visibility;
                Visibility.VidPnSourceId = D3DDDI_ID_ALL;
                Visibility.Visible = FALSE;
                vidpn.SetVidPnSourceVisibility(&Visibility);
            }
            m_AdapterPowerState = DevicePowerState;
        }

        switch (DevicePowerState)
        {
            case PowerDeviceUnspecified:
            case PowerDeviceD0:
                {
                    // Only re-init from a torn-down state. A D1/D2 -> D0
                    // transition lands here with the adapter still live,
                    // since D1/D2 are no-ops on this device.
                    if (!IsHardwareInit())
                    {
                        VioGpuAdapterInit();
                    }
                }
                break;
            case PowerDeviceD1:
            case PowerDeviceD2:
                {
                    // virtio-gpu exposes no D1/D2 hardware state, so
                    // there is nothing to tear down or save; the queues
                    // remain live and ready for D0 traffic.
                    DbgPrint(TRACE_LEVEL_INFORMATION,
                             ("%s entering D%d (no teardown)\n",
                              __FUNCTION__, DevicePowerState - PowerDeviceD0));
                }
                break;
            case PowerDeviceD3:
                {
                    vidpn.Powerdown();
                    VioGpuAdapterClose();
                }
                break;
        }
        return STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpuAdapter::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                   _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    VIOGPU_ASSERT(pChildRelations != NULL);

    ULONG ChildRelationsCount = (ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR)) - 1;
    VIOGPU_ASSERT(ChildRelationsCount <= MAX_CHILDREN);

    for (UINT ChildIndex = 0; ChildIndex < ChildRelationsCount; ++ChildIndex)
    {
        pChildRelations[ChildIndex].ChildDeviceType = TypeVideoOutput;
        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = IsVgaDevice() ? HpdAwarenessAlwaysConnected
                                                                                   : HpdAwarenessInterruptible;
        // Virtual virtio-gpu scanouts have no physical connector.
        // VOT_OTHER is the documented catch-all; HD15 would identify
        // the output as analog VGA D-Sub and gate off HDR/VRR via
        // connector-type heuristics in the shell.
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = IsVgaDevice() ? D3DKMDT_VOT_INTERNAL
                                                                                                           : D3DKMDT_VOT_OTHER;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        pChildRelations[ChildIndex].AcpiUid = 0;
        pChildRelations[ChildIndex].ChildUid = ChildIndex;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    VIOGPU_ASSERT(pChildStatus != NULL);
    VIOGPU_ASSERT(pChildStatus->ChildUid < MAX_CHILDREN);

    switch (pChildStatus->Type)
    {
        case StatusConnection:
            {
                pChildStatus->HotPlug.Connected = IsDriverActive();
                return STATUS_SUCCESS;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_WARNING, ("Unknown pChildStatus->Type (0x%I64x) requested.", pChildStatus->Type));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuAdapter::QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pDeviceDescriptor != NULL);
    VIOGPU_ASSERT(ChildUid < MAX_CHILDREN);
    PBYTE edid = vidpn.GetEdidData(ChildUid);
    // Bound the copy by what actually backs the pointer: the built-in fallback
    // EDID is one 128-byte block, so bounding by EDID_RAW_BLOCK_SIZE would
    // serve adjacent .data for any offset past block 0.
    ULONG edidSize = vidpn.GetEdidSize();

    if (!edid)
    {
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    else if (pDeviceDescriptor->DescriptorOffset < edidSize)
    {
        ULONG len = min(pDeviceDescriptor->DescriptorLength, (edidSize - pDeviceDescriptor->DescriptorOffset));
        RtlCopyMemory(pDeviceDescriptor->DescriptorBuffer, (edid + pDeviceDescriptor->DescriptorOffset), len);
        pDeviceDescriptor->DescriptorLength = len;
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
}

NTSTATUS VioGpuAdapter::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryAdapterInfo != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_UMDRIVERPRIVATE:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(VIOGPU_ADAPTERINFO))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(VIOGPU_ADAPTERINFO) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(VIOGPU_ADAPTERINFO))) return STATUS_BUFFER_TOO_SMALL;
                }
                VIOGPU_ADAPTERINFO *info = (VIOGPU_ADAPTERINFO *)pQueryAdapterInfo->pOutputData;
                info->IamVioGPU = VIOGPU_IAM;
                // Report against m_u64GuestFeatures (what was actually
                // negotiated) so UMD never sees a flag we did not ack.
                // virtio-gpu silently ignores unset feature bits, so an
                // over-claimed hint would let UMD send fields the host
                // disregards.
                info->Flags.Supports3d = virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_VIRGL) &&
                                         virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_RESOURCE_BLOB) &&
                                         virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_CONTEXT_INIT);
                info->Flags.HasShmem = m_VioDev.shmem.available;
                // Wire selection: the UMD picks the BY_ID blob map/unmap wire
                // from this.  Always set, but it stays a flag because the
                // struct is UMD-facing ABI.
                info->Flags.Wddm2 = 1;
                info->Flags.Reserved = 0;
                info->SupportedCapsetIDs = m_supportedCapsetIDs;
                info->AdapterLuid = m_AdapterLuid;
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_DRIVERCAPS:
            {
                // Do NOT compare against sizeof(DXGK_DRIVERCAPS) here.  The
                // struct grows with DXGKDDI_INTERFACE_VERSION, and dxgkrnl
                // sizes the buffer for the version this driver REGISTERED, not
                // the version its headers were compiled against, so a sizeof()
                // check rejects a perfectly legal call, fails
                // DxgkDdiStartDevice, and leaves the device in code 43.  The
                // zero-fill and all field writes below stay inside
                // OutputDataSize.
                if (!pQueryAdapterInfo->OutputDataSize)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize is 0 (sizeof(DXGK_DRIVERCAPS) = 0x%zx)\n",
                              sizeof(DXGK_DRIVERCAPS)));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_DRIVERCAPS *pDriverCaps = (DXGK_DRIVERCAPS *)pQueryAdapterInfo->pOutputData;
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("InterruptMessageNumber = %d, WDDMVersion = %d\n",
                          pDriverCaps->InterruptMessageNumber,
                          pDriverCaps->WDDMVersion));
                RtlZeroMemory(pDriverCaps, pQueryAdapterInfo->OutputDataSize /*sizeof(DXGK_DRIVERCAPS)*/);
                // 2.2 matches the registered interface version; no cap above
                // the 2.0 set is claimed (no MPO/HwQueue/IoMmu), and dxgkrnl
                // has no version-alone check above 2.0.  (2.3 was tried and
                // bugchecks: see the note at InitialData.Version.)
                pDriverCaps->WDDMVersion = DXGKDDI_WDDMv2_2;
                pDriverCaps->HighestAcceptableAddress.QuadPart = (ULONG64)-1;
                // GPU virtual addressing is pure bookkeeping in the
                // rendering-bypass model (real work rides virtio escapes;
                // nothing ever dereferences a GPU VA), so claim GpuMmu but
                // NOT IoMmu.
                pDriverCaps->MemoryManagementCaps.VirtualAddressingSupported = 1;
                pDriverCaps->MemoryManagementCaps.GpuMmuSupported = 1;

                pDriverCaps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_NONE;
                pDriverCaps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_NONE;

                // Flip-model presents arrive via DxgkDdiPresent with Flags.Flip
                // (no MMIO PhysicalAddress is required); VioGpuDevice::Present
                // latches the new primary in m_sourceRes and the vsync Flip
                // scans it out by res_id.
                // TRUE routes every flip-model present through
                // DxgkDdiSetVidPnSourceAddress (which this driver fully
                // implements, incl. DIRQL) with the exact PrimaryAddress
                // dxgkrnl later matches against the vsync-reported
                // address. With FALSE, flips rode an empty Present
                // packet, the vsync reported a stale/zero address, no
                // queued flip ever CONFIRMED: DWM cFrameComplete stayed
                // 0 forever, refresh stats collapsed (~2 Hz), and UWP
                // apps never dismissed their splash (calc blank).
                pDriverCaps->FlipCaps.FlipOnVSyncMmIo = TRUE;

                // 0 told dxgkrnl this display can't queue flips at all;
                // windowed flip-model (composition) presents were never
                // bound by DWM (calc/UWP splash-blank, d3dtest9 probe
                // invisible). Allow one queued flip per vsync.
                pDriverCaps->MaxQueuedFlipOnVSync = 1;

                pDriverCaps->MemoryManagementCaps.SectionBackedPrimary = TRUE;

                pDriverCaps->SupportDirectFlip = 0;
                pDriverCaps->SchedulingCaps.MultiEngineAware = 1;
                pDriverCaps->SchedulingCaps.PreemptionAware = 1;

                pDriverCaps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;

                pDriverCaps->SupportSmoothRotation = FALSE;
                // The cap means "implements DxgkDdiStopDeviceAndReleasePost-
                // DisplayOwnership", which this driver does unconditionally --
                // it is not a statement about the PCI class of the device.
                pDriverCaps->SupportNonVGA = TRUE;

                // Disable pointer on viogpu3d for now
                // if (IsPointerEnabled()) {
                //    pDriverCaps->MaxPointerWidth = POINTER_SIZE;
                //    pDriverCaps->MaxPointerHeight = POINTER_SIZE;
                //    pDriverCaps->PointerCaps.Value = 0;
                //    pDriverCaps->PointerCaps.Color = 1;
                //}

                // Surely this is enough...
                // pDriverCaps->NumberOfSwizzlingRanges = 1024;

                // DXGK_VIDSCHCAPS: software-scheduled node. Beyond
                // MultiEngineAware/PreemptionAware (above), the WDDM2-only
                // bits stay 0: fence completion is CPU-side (64-bit atomics
                // fine -> No64BitAtomics=0), preempt requests stay PASSIVE
                // (LowIrqlPreemptCommand=0), and no HwQueue support is
                // claimed.
                //
                // FlipOnVSyncMmIo (TRUE above) is also load-bearing for
                // WDDM2+: dxgkrnl 26100 refuses to create ADAPTER_RENDER
                // for a WDDM2+ driver without MMIO-flip support (ETW:
                // "Driver reports WDDM version 2.0 or higher but does not
                // support FlipOnVSyncMmIo cap", DpiFdoStartAdapterFailed,
                // STATUS_INVALID_PARAMETER).  SetVidPnSourceAddress and the
                // flip latch service the MMIO-flip flow.

                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s Driver caps return\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_QUERYSEGMENT3:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT3))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(DXGK_QUERYSEGMENTOUT) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(DXGK_QUERYSEGMENTOUT)));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DbgPrint(TRACE_LEVEL_VERBOSE, ("QUERY SEG\n"));
                DXGK_QUERYSEGMENTOUT3 *pSegmentInfo = (DXGK_QUERYSEGMENTOUT3 *)pQueryAdapterInfo->pOutputData;

                if (m_VioDev.shmem.available)
                {
                    pSegmentInfo->NbSegment = 2;
                }
                else
                {
                    pSegmentInfo->NbSegment = 1;
                }

                if (pSegmentInfo->pSegmentDescriptor)
                {
                    DXGK_SEGMENTDESCRIPTOR3 *pSegmentDesc = pSegmentInfo->pSegmentDescriptor;
                    memset(pSegmentDesc, 0, sizeof(*pSegmentDesc) * pSegmentInfo->NbSegment);

                    // Room for the monitored-fence deferral to stamp a
                    // VIOGPU_DMA_PRIVATE into each paging buffer's private
                    // data (SIGNAL_MONITORED_FENCE); 0 forces the build-time
                    // early write and pollers see fence values before the GPU
                    // work completes.
                    pSegmentInfo->PagingBufferPrivateDataSize = sizeof(VIOGPU_DMA_PRIVATE);

                    // 0 = paging buffers from SYSTEM memory.  Segment 1 is the
                    // CPU-INVISIBLE aperture: pointing the paging pool there
                    // leaves VIDMM_DMA_POOL::BeginCPUAccess with nothing to
                    // map, and every TDR's debug-info collection bugchecks
                    // 0x7E instead of recovering (dxgmms1 null-class AV).
                    // VBoxMPWddm uses 0 as well.
                    pSegmentInfo->PagingBufferSegmentId = 0;
                    pSegmentInfo->PagingBufferSize = 10 * PAGE_SIZE;

                    //
                    // Fill out aperture segment descriptor
                    //
                    pSegmentDesc[0].BaseAddress.QuadPart = 0xC0000000;
                    // (SIZE_T) so the product is not evaluated in int -- 1GiB
                    // fits today, but doubling the constant would overflow.
                    pSegmentDesc[0].Size = (SIZE_T)256 * 1024 * 4096;
                    pSegmentDesc[0].CommitLimit = (SIZE_T)256 * 1024 * 4096;
                    // pSegmentDesc[0].CpuTranslatedAddress.QuadPart = 0xFFFFFFFE00000000;
                    pSegmentDesc[0].Flags.Aperture = TRUE;
                    pSegmentDesc[0].Flags.CacheCoherent = TRUE;
                    pSegmentDesc[0].Flags.CpuVisible = FALSE;
                    pSegmentDesc[0].Flags.DirectFlip = TRUE;

                    if (m_VioDev.shmem.available)
                    {
                        pSegmentDesc[1].BaseAddress.QuadPart = VioGpuAdapter::SHMEM_GPU_BASE_VA;
                        pSegmentDesc[1].Size = m_VioDev.shmem.length;
                        pSegmentDesc[1].CommitLimit = m_VioDev.shmem.length;
                        // FIXME: is this correct?
                        pSegmentDesc[1].CpuTranslatedAddress.QuadPart = m_PciResources.GetPciBar(m_VioDev.shmem.bar)->GetPA().QuadPart + m_VioDev.shmem.offset;
                        pSegmentDesc[1].Flags.Aperture = FALSE; // TRUE?
                        pSegmentDesc[1].Flags.CacheCoherent = TRUE;
                        pSegmentDesc[1].Flags.CpuVisible = TRUE;
                        pSegmentDesc[1].Flags.DirectFlip = TRUE;
                    }
                }
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s Requested segments\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }

        //
        // GpuMmu queries.  dxgkrnl aborts adapter start if a WDDM2 driver
        // refuses any of these, even the ones it has nothing to report for.
        //
        case DXGKQAITYPE_QUERYSEGMENT4:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT4))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(DXGK_QUERYSEGMENTOUT4) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(DXGK_QUERYSEGMENTOUT4)));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s QUERYSEGMENT4\n", __FUNCTION__));
                DXGK_QUERYSEGMENTOUT4 *pSegmentInfo = (DXGK_QUERYSEGMENTOUT4 *)pQueryAdapterInfo->pOutputData;

                // Same segment topology as QUERYSEGMENT3 above: segment 1 is
                // the CPU-invisible aperture, optional segment 2 the
                // CPU-visible host shmem window.
                UINT nbSegments = m_VioDev.shmem.available ? 2 : 1;

                // Two-call contract: the first call has NbSegment == 0 (and no
                // descriptor array); report only the segment count and touch
                // nothing else.
                if (pSegmentInfo->NbSegment == 0 || pSegmentInfo->pSegmentDescriptor == NULL)
                {
                    pSegmentInfo->NbSegment = nbSegments;
                    return STATUS_SUCCESS;
                }

                pSegmentInfo->NbSegment = nbSegments;
                // Room for the monitored-fence deferral (see QUERYSEGMENT3).
                pSegmentInfo->PagingBufferPrivateDataSize = sizeof(VIOGPU_DMA_PRIVATE);
                // 0 = paging buffers from SYSTEM memory (0x7E-on-TDR when
                // pointed at the CPU-invisible aperture; VBoxMPWddm uses 0).
                pSegmentInfo->PagingBufferSegmentId = 0;
                pSegmentInfo->PagingBufferSize = 10 * PAGE_SIZE;

                // pSegmentDescriptor is a BYTE* stride array: dxgkrnl OWNS
                // SegmentDescriptorStride (its per-element size, matching the
                // OS's DXGK_SEGMENTDESCRIPTOR4) and allocates NbSegment*stride
                // bytes.  Never invent the stride: a stride smaller than the
                // struct written here would overrun the OS array and corrupt
                // adjacent pool.
                SIZE_T stride = pSegmentInfo->SegmentDescriptorStride;
                if (stride < sizeof(DXGK_SEGMENTDESCRIPTOR4))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s SegmentDescriptorStride %Iu < sizeof(DXGK_SEGMENTDESCRIPTOR4) %Iu\n",
                              __FUNCTION__, stride, sizeof(DXGK_SEGMENTDESCRIPTOR4)));
                    return STATUS_INVALID_PARAMETER;
                }
                RtlZeroMemory(pSegmentInfo->pSegmentDescriptor, stride * nbSegments);

                //
                // Segment 1: aperture (mirrors pSegmentDesc[0] of the v3 arm).
                //
                DXGK_SEGMENTDESCRIPTOR4 *pDesc = (DXGK_SEGMENTDESCRIPTOR4 *)pSegmentInfo->pSegmentDescriptor;
                pDesc->BaseAddress.QuadPart = 0xC0000000;
                pDesc->Size = 256 * 1024 * 4096;
                pDesc->CommitLimit = 256 * 1024 * 4096;
                pDesc->Flags.Aperture = TRUE;
                pDesc->Flags.CacheCoherent = TRUE;
                pDesc->Flags.CpuVisible = FALSE;
                pDesc->Flags.DirectFlip = TRUE;

                if (nbSegments == 2)
                {
                    //
                    // Segment 2: CPU-visible shmem (mirrors pSegmentDesc[1]).
                    //
                    pDesc = (DXGK_SEGMENTDESCRIPTOR4 *)(pSegmentInfo->pSegmentDescriptor + stride);
                    pDesc->BaseAddress.QuadPart = VioGpuAdapter::SHMEM_GPU_BASE_VA;
                    pDesc->Size = m_VioDev.shmem.length;
                    pDesc->CommitLimit = m_VioDev.shmem.length;
                    pDesc->CpuTranslatedAddress.QuadPart = m_PciResources.GetPciBar(m_VioDev.shmem.bar)->GetPA().QuadPart +
                                                           m_VioDev.shmem.offset;
                    pDesc->Flags.Aperture = FALSE;
                    pDesc->Flags.CacheCoherent = TRUE;
                    pDesc->Flags.CpuVisible = TRUE;
                    pDesc->Flags.DirectFlip = TRUE;
                }

                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s Requested segments (v4)\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_HISTORYBUFFERPRECISION:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGKARG_HISTORYBUFFERPRECISION))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                // The scheduler's history-buffer timestamps come from the
                // CPU-side fence bookkeeping this driver reports, which is
                // 64-bit.
                DXGKARG_HISTORYBUFFERPRECISION *pPrecision =
                    (DXGKARG_HISTORYBUFFERPRECISION *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pPrecision, sizeof(*pPrecision));
                pPrecision->PrecisionBits = 64;

                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s history buffer precision return\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(UINT))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                // All-zero extension caps: no secure display, no virtual
                // mode support.  dxgkrnl sizes the struct by the interface
                // version WE registered, so zero exactly what it gave us.
                RtlZeroMemory(pQueryAdapterInfo->pOutputData, pQueryAdapterInfo->OutputDataSize);

                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s display drivercaps extension return\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_PHYSICALADAPTERCAPS:
            {
                // VidMm sizes the output by the interface version WE
                // registered, which can be smaller than the full header
                // struct compiled against the latest WDK -- so require only
                // the 2.0-era fields (a floor, not an equality); the zero-
                // fill below covers whatever size actually arrives, and
                // fields beyond the ones written stay zero = not claimed.
                if (pQueryAdapterInfo->InputDataSize < sizeof(DXGK_QUERYPHYSICALADAPTERCAPSIN) ||
                    pQueryAdapterInfo->OutputDataSize < RTL_SIZEOF_THROUGH_FIELD(DXGK_PHYSICALADAPTERCAPS, Flags))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_QUERYPHYSICALADAPTERCAPSIN *pIn =
                    (DXGK_QUERYPHYSICALADAPTERCAPSIN *)pQueryAdapterInfo->pInputData;
                DXGK_PHYSICALADAPTERCAPS *pCaps =
                    (DXGK_PHYSICALADAPTERCAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pCaps, pQueryAdapterInfo->OutputDataSize);

                // Single physical adapter, single execution node; node 0 is
                // also the paging node (matches GpuEngineTopology above).
                // DxgkPhysicalAdapterHandle is dxgkrnl's OWN handle for this
                // adapter -- MSDN: "Handle, which is passed to the kernel
                // mode driver as DXGKRNL_INTERFACE::DeviceHandle in
                // DxgkDdiStartDevice".  Anything else (a driver pointer, or
                // NULL) crashes dxgkrnl where it dereferences the handle
                // during node-name setup and VidMm adapter initialization.
                pCaps->NumExecutionNodes = 1;
                pCaps->PagingNodeIndex = 0;
                pCaps->DxgkPhysicalAdapterHandle = m_DxgkInterface.DeviceHandle;
                pCaps->Flags.GpuMmuSupported = 1;

                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<--- %s physical adapter caps (index %u)\n",
                          __FUNCTION__, pIn->PhysicalAdapterIndex));
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_64BITONLYCAPS:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_64_BIT_ONLY_CAPS))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                // A 32-bit (WOW) UMD ships alongside the 64-bit one, so the
                // adapter must NOT be marked 64-bit-only: SupportsOnly64Bit
                // makes dxgkrnl hide the adapter from every 32-bit process
                // (D3DKMTEnumAdapters* returns WARP only), and 32-bit apps
                // then silently render on WARP until they exhaust their 2 GB
                // address space.
                DXGK_64_BIT_ONLY_CAPS *p64 = (DXGK_64_BIT_ONLY_CAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(p64, sizeof(*p64));
                p64->SupportsOnly64Bit = 0;

                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s 64-bit-only caps return\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_GPUMMUCAPS:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_GPUMMUCAPS))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                // Software-only GPU MMU; the geometry constants are defined
                // with their rationale in viogpu_adapter.h.
                DXGK_GPUMMUCAPS *pGpuMmuCaps = (DXGK_GPUMMUCAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pGpuMmuCaps, sizeof(DXGK_GPUMMUCAPS));

                // Page tables are software-emulated bookkeeping: VidMm must
                // tell us about every update (ExplicitPageTableInvalidation)
                // and writes PTEs through CPU pointers (CPU_VIRTUAL mode).
                pGpuMmuCaps->ExplicitPageTableInvalidation = 1;
                pGpuMmuCaps->CacheCoherentMemorySupported = 1; // everything is system RAM
                pGpuMmuCaps->ZeroInPteSupported = 1;           // zero PTE == not present
                pGpuMmuCaps->ReadOnlyMemorySupported = 0;
                pGpuMmuCaps->NoExecuteMemorySupported = 0;
                pGpuMmuCaps->LargePageSupported = 0;
                pGpuMmuCaps->DualPteSupported = 0;

                pGpuMmuCaps->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
                pGpuMmuCaps->VirtualAddressBitCount = VIOGPU_WDDM2_VA_BIT_COUNT;
                pGpuMmuCaps->PageTableLevelCount = VIOGPU_WDDM2_PT_LEVELS;
                pGpuMmuCaps->LeafPageTableSizeFor64KPagesInBytes = 0; // no 64KB pages

                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s GpuMmu caps return\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_PAGETABLELEVELDESC:
            {
                if (pQueryAdapterInfo->InputDataSize < sizeof(DXGK_QUERYPAGETABLELEVELDESCIN) ||
                    pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_PAGE_TABLE_LEVEL_DESC))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_QUERYPAGETABLELEVELDESCIN *pLevelIn = (DXGK_QUERYPAGETABLELEVELDESCIN *)pQueryAdapterInfo->pInputData;
                if (pLevelIn->LevelIndex >= VIOGPU_WDDM2_PT_LEVELS)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s invalid page table level %u\n", __FUNCTION__, pLevelIn->LevelIndex));
                    return STATUS_INVALID_PARAMETER;
                }

                // All levels share the same geometry: 4KB tables of 512
                // 8-byte PTEs in system memory (segment id 0 is reserved for
                // system memory, which also caps a table at 4KB).  Level 0
                // is the leaf; level 3 is the root.
                DXGK_PAGE_TABLE_LEVEL_DESC *pLevelDesc = (DXGK_PAGE_TABLE_LEVEL_DESC *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pLevelDesc, sizeof(DXGK_PAGE_TABLE_LEVEL_DESC));
                pLevelDesc->PageTableIndexBitCount = VIOGPU_WDDM2_PT_INDEX_BITS;
                pLevelDesc->PageTableSegmentId = 0;              // system memory
                pLevelDesc->PagingProcessPageTableSegmentId = 0; // system memory
                pLevelDesc->PageTableSizeInBytes = VIOGPU_WDDM2_PT_SIZE;
                pLevelDesc->PageTableAlignmentInBytes = VIOGPU_WDDM2_PT_SIZE;

                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<--- %s page table level %u desc return\n", __FUNCTION__, pLevelIn->LevelIndex));
                return STATUS_SUCCESS;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s unknown type %d\n", __FUNCTION__, pQueryAdapterInfo->Type));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

// Defined in the non-paged section below; runs at DISPATCH from the completion DPC.
static void PresentFenceCb(void *ctx, void *unused1, void *unused2);

NTSTATUS VioGpuAdapter::Escape(_In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pEscape != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s Flags = %d\n", __FUNCTION__, pEscape->Flags.Value));
    PVIOGPU_ESCAPE pVioGpuEscape = (PVIOGPU_ESCAPE)pEscape->pPrivateDriverData;
    NTSTATUS status = STATUS_SUCCESS;

    // The guard must cover the whole VIOGPU_ESCAPE, not a pointer to one:
    // every case below reads and several write union members that extend far
    // past 8 bytes, and the per-case DataLength checks validate the caller's
    // own declared length, not the buffer dxgkrnl actually captured.
    UINT size = pEscape->PrivateDriverDataSize;
    if (size < sizeof(VIOGPU_ESCAPE))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s buffer too small %d, should be at least %zu\n",
                  __FUNCTION__,
                  size,
                  sizeof(VIOGPU_ESCAPE)));
        return STATUS_INVALID_BUFFER_SIZE;
    }

    switch (pVioGpuEscape->Type)
    {
        case VIOGPU_GET_DEVICE_ID:
            {
                CreateResolutionEvent();
                size = sizeof(ULONG);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                pVioGpuEscape->Id = m_Id;
                break;
            }
        case VIOGPU_GET_CUSTOM_RESOLUTION:
            {
                size = sizeof(VIOGPU_DISP_MODE);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                vidpn.EscapeCustomResoulution(&pVioGpuEscape->Resolution);
                break;
            }
        case VIOGPU_GET_CAPS:
            {
                size = sizeof(VIOGPU_CAPSET_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }

                if (pVioGpuEscape->Capset.CapsetId == 0 ||
                    pVioGpuEscape->Capset.CapsetId > VIRTIO_GPU_MAX_CAPSET_ID)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s capset id %llu out of range\n",
                              __FUNCTION__,
                              (ULONGLONG)pVioGpuEscape->Capset.CapsetId));
                    return STATUS_INVALID_PARAMETER_1;
                }
                if (!(m_supportedCapsetIDs & (1ull << pVioGpuEscape->Capset.CapsetId)))
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s capset id is not supported\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER_1;
                }
                CAPSET_INFO *pCapsetInfo = &m_capsetInfos[pVioGpuEscape->Capset.CapsetId];
                if (pCapsetInfo->max_version < pVioGpuEscape->Capset.Version)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s capset version is too low\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER_2;
                };

                PGPU_VBUFFER vbuf = NULL;
                if (!ctrlQueue.AskCapset(&vbuf,
                                         pVioGpuEscape->Capset.CapsetId,
                                         pCapsetInfo->max_size,
                                         pVioGpuEscape->Capset.Version) ||
                    vbuf == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s AskCapset failed for capset id %llu\n",
                              __FUNCTION__,
                              (ULONGLONG)pVioGpuEscape->Capset.CapsetId));
                    status = STATUS_IO_TIMEOUT;
                    break;
                }
                __try
                {
                    UCHAR *buf = ((PGPU_RESP_CAPSET)vbuf->resp_buf)->capset_data;
                    ULONG to_copy = min(pVioGpuEscape->Capset.Size, pCapsetInfo->max_size);
                    UCHAR *userCapset = VIOGPU_UM_PTR_AS(UCHAR *, pVioGpuEscape->Capset.Capset);
                    ProbeForWrite(userCapset, to_copy, sizeof(UCHAR));
                    memcpy(userCapset, buf, to_copy);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    DbgPrint(TRACE_LEVEL_WARNING, ("Failed to copy capset to user buffer"));
                    status = STATUS_INVALID_PARAMETER;
                }
                ctrlQueue.ReleaseBuffer(vbuf);

                break;
            }
        case VIOGPU_GET_PCI_INFO:
            {
                size = sizeof(VIOGPU_PCI_INFO_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                pVioGpuEscape->PciInfo.Domain = 0; // TODO: How to get domain?
                pVioGpuEscape->PciInfo.Bus = m_PciBus;
                pVioGpuEscape->PciInfo.Dev = m_PciDev;
                pVioGpuEscape->PciInfo.Func = m_PciFunc;
                break;
            }
        case VIOGPU_RES_INFO:
            {
                size = sizeof(VIOGPU_RES_INFO_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                // Cookie-first: handle resolution races dxgkrnl handle reuse
                // (GetHandleData returns NULL and KMT-map entries go stale),
                // which resolves one process's RES_INFO onto a different
                // allocation and adopts its res_id.
                VioGpuAllocation *allocation = NULL;
                if (pVioGpuEscape->ResourceInfo.LookupCookie != 0)
                {
                    allocation = CookieMapLookup(pVioGpuEscape->ResourceInfo.LookupCookie);
                }
                if (allocation == NULL)
                {
                    allocation = AllocationFromHandle(pVioGpuEscape->ResourceInfo.ResHandle);
                }
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s RES_INFO invalid handle %x\n", __FUNCTION__,
                                                 pVioGpuEscape->ResourceInfo.ResHandle));
                    return STATUS_INVALID_PARAMETER;
                }
                // Neither the cookie nor the handle proves the caller may see
                // this allocation: both are unvalidated escape payload, cookies
                // are drawn from a dense counter, and the KMT map is keyed on
                // the handle value alone.  This escape hands back a res_id and
                // maps the blob's BAR window into the calling process, so
                // require the allocation to be open on the calling device --
                // the same thing dxgkrnl checks for the DDIs that take a
                // handle.
                VioGpuDevice *pResDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (!allocation->IsOpenOn(pResDevice))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s RES_INFO res_id=%d not open on device %p\n",
                              __FUNCTION__, allocation->GetId(), pResDevice));
                    return STATUS_ACCESS_DENIED;
                }

                status = allocation->EscapeResourceInfo(&pVioGpuEscape->ResourceInfo);

                break;
            }
        case VIOGPU_RES_BUSY:
            {
                size = sizeof(VIOGPU_RES_BUSY_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuAllocation *allocation = AllocationFromHandle(pVioGpuEscape->ResourceBusy.ResHandle);
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s RES_BUSY invalid handle %x\n", __FUNCTION__,
                                                 pVioGpuEscape->ResourceBusy.ResHandle));
                    return STATUS_INVALID_PARAMETER;
                }
                // See RES_INFO: the KMT-map fallback resolves on the handle
                // value alone, so the caller's claim to this allocation still
                // has to be checked.  RES_BUSY can also block on another
                // allocation's busy event.
                VioGpuDevice *pBusyDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (!allocation->IsOpenOn(pBusyDevice))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s RES_BUSY res_id=%d not open on device %p\n",
                              __FUNCTION__, allocation->GetId(), pBusyDevice));
                    return STATUS_ACCESS_DENIED;
                }
                status = allocation->EscapeResourceBusy(&pVioGpuEscape->ResourceBusy);

                break;
            }
        case VIOGPU_CTX_INIT:
            {
                size = sizeof(VIOGPU_CTX_INIT_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuDevice *pDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (pDevice == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s no hDdevice(context) supplied\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }
                pDevice->m_Context.Init(&pVioGpuEscape->CtxInit);
                // The UMD references the context by its virtio id when it
                // targets cross-device submits (IMPORT present_ctx_id).
                pVioGpuEscape->CtxInit.CtxId = pDevice->m_Context.GetId();

                if (pVioGpuEscape->CtxInit.CapsetID == VIRTIO_GPU_CAPSET_VENUS ||
                    pVioGpuEscape->CtxInit.CapsetID == VIRTIO_GPU_CAPSET_NEPTUNE)
                {
                    bool has_virgl  = !!(m_supportedCapsetIDs & (1llu << VIRTIO_GPU_CAPSET_VIRGL));
                    bool has_virgl2 = !!(m_supportedCapsetIDs & (1llu << VIRTIO_GPU_CAPSET_VIRGL2));
                    if (!has_virgl && !has_virgl2)
                    {
                        DbgPrint(TRACE_LEVEL_ERROR, ("%s device does not support virgl\n", __FUNCTION__));
                        break;
                    }
                    VIOGPU_CTX_INIT_REQ VirglCtx;
                    memset(&VirglCtx, 0, sizeof(VirglCtx));
                    VirglCtx.CapsetID = has_virgl2 ? VIRTIO_GPU_CAPSET_VIRGL2 : VIRTIO_GPU_CAPSET_VIRGL;
                    VirglCtx.NumRings = 64;
                    memcpy(VirglCtx.DebugName, "virgl-shadow-win32", sizeof("virgl-shadow-win32") - 1);

                    pDevice->m_Virgl.Init(&VirglCtx);
                }

                break;
            }
        case VIOGPU_BLIT_INIT:
            {
                size = sizeof(VIOGPU_BLIT_INIT_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuDevice *pDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (pDevice == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s no hDdevice(context) supplied\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                if (!NT_SUCCESS(ObReferenceObjectByHandle(VioGpuUmHandleValue(pVioGpuEscape->BlitInit.EventUM),
                                                          SYNCHRONIZE | EVENT_MODIFY_STATE,
                                                          *ExEventObjectType,
                                                          UserMode,
                                                          (void **)&pDevice->m_hUM,
                                                          NULL)))
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s: Unable to reference user-mode event object 0x%llx\n", __FUNCTION__, pVioGpuEscape->BlitInit.EventUM));
                    return STATUS_INVALID_HANDLE;
                }

                if (!NT_SUCCESS(ObReferenceObjectByHandle(VioGpuUmHandleValue(pVioGpuEscape->BlitInit.EventKM),
                                                          SYNCHRONIZE | EVENT_MODIFY_STATE,
                                                          *ExEventObjectType,
                                                          UserMode,
                                                          (void **)&pDevice->m_hKM,
                                                          NULL)))
                {
                    ObDereferenceObject(pDevice->m_hUM);
                    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s: Unable to reference user-mode event object 0x%llx\n", __FUNCTION__, pVioGpuEscape->BlitInit.EventKM));
                    return STATUS_INVALID_HANDLE;
                }

                pDevice->m_pBlit = VIOGPU_UM_PTR_AS(PVIOGPU_BLIT_PRESENT, pVioGpuEscape->BlitInit.pBlitPresent);
                break;
            }
        case VIOGPU_SUBMIT_PRESENT_FENCE:
            {
                size = sizeof(VIOGPU_PRESENT_FENCE_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__, pVioGpuEscape->DataLength, size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuDevice *pDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (pDevice == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s no hDevice(context) supplied\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }
                // EventUM is optional.  The npt transport passes an event so
                // its guest-side waiter can observe GPU completion (windowed
                // presents block on it; the fence-feedback path also uses it);
                // a caller that only needs the flip-gate token may pass 0.
                PKEVENT pEvent = NULL;
                if (pVioGpuEscape->PresentFence.EventUM != 0 &&
                    !NT_SUCCESS(ObReferenceObjectByHandle(VioGpuUmHandleValue(pVioGpuEscape->PresentFence.EventUM),
                                                          SYNCHRONIZE | EVENT_MODIFY_STATE,
                                                          *ExEventObjectType,
                                                          UserMode,
                                                          (void **)&pEvent,
                                                          NULL)))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("---> %s: SUBMIT_PRESENT_FENCE bad event 0x%llx\n",
                              __FUNCTION__, pVioGpuEscape->PresentFence.EventUM));
                    return STATUS_INVALID_HANDLE;
                }

                PRESENT_FENCE_CTX *pCtx = (PRESENT_FENCE_CTX *)
                    ExAllocateFromNPagedLookasideList(&m_PresentFenceLookaside);
                if (pCtx == NULL)
                {
                    if (pEvent != NULL)
                    {
                        ObDereferenceObject(pEvent);
                    }
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                pCtx->pAdapter = this;
                pCtx->Token = PresentTokenSubmit();
                pCtx->pEvent = pEvent;

                // Pair the arm with the flip that follows it on this same
                // thread (the UMD arms and then presents without switching
                // threads; DxgkDdiPresent runs synchronously in the caller's
                // thread).  See the token block in viogpu_adapter.h for why
                // the pairing can be neither per-device nor the peeked
                // newest token.
                StampThreadToken(PsGetCurrentThreadId(), pCtx->Token);

                // Empty fenced SUBMIT_3D on the event ring.  The host defers the
                // used-ring response until the fence's D3DMetal proxy signals (real
                // GPU completion), so PresentFenceCb fires exactly when the frame
                // finished rendering on the host GPU.
                if (!ctrlQueue.SubmitCommand(NULL, 0, pDevice->m_Context.GetId(), TRUE,
                                             pVioGpuEscape->PresentFence.RingIdx,
                                             PresentFenceCb, pCtx))
                {
                    // The submit never queued, so PresentFenceCb will not fire
                    // and the token would gate every later flip forever.
                    // Retire it here, matching what the callback does.
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s SUBMIT_PRESENT_FENCE not queued; retiring token %llu\n",
                              __FUNCTION__, pCtx->Token));
                    PresentTokenRetire(pCtx->Token);
                    PresentWaitSweep();
                    if (pCtx->pEvent != NULL)
                    {
                        KeSetEvent(pCtx->pEvent, IO_NO_INCREMENT, FALSE);
                        ObDereferenceObject(pCtx->pEvent);
                    }
                    ExFreeToNPagedLookasideList(&m_PresentFenceLookaside, pCtx);
                    vidpn.OnPresentTokenRetired();
                }
                break;
            }
        case VIOGPU_ARM_GATE:
            {
                size = sizeof(VIOGPU_GATE_ARM_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__, pVioGpuEscape->DataLength, size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuDevice *pDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (pDevice == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s no hDevice(context) supplied\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }
                return GateArm(pDevice, pVioGpuEscape->GateArm.RingIdx, &pVioGpuEscape->GateArm.Token);
            }
        case VIOGPU_PRESENT_GATE_HINT:
            {
                return MarkThreadTokenPacketGate(PsGetCurrentThreadId()) ? STATUS_SUCCESS : STATUS_NOT_FOUND;
            }
        default:
            DbgPrint(TRACE_LEVEL_ERROR, ("%s: invalid Escape type 0x%x\n", __FUNCTION__, pVioGpuEscape->Type));
            status = STATUS_INVALID_PARAMETER;
    }

    return status;
}

NTSTATUS VioGpuAdapter::QueryInterface(_In_ CONST PQUERY_INTERFACE pQueryInterface)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryInterface != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s Version = %d\n", __FUNCTION__, pQueryInterface->Version));

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS VioGpuAdapter::StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                 _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(TargetId < MAX_CHILDREN);
    // SetPowerState's first argument is a HardwareUid -- the adapter's
    // own DISPLAY_ADAPTER_HW_ID, not a video-target id. Passing the
    // child TargetId here short-circuits the function (TargetId never
    // matches DISPLAY_ADAPTER_HW_ID), so the D0 wake-up is skipped.
    if (m_MonitorPowerState > PowerDeviceD0)
    {
        SetPowerState(DISPLAY_ADAPTER_HW_ID, PowerDeviceD0, PowerActionNone);
    }
    vidpn.ReleasePostDisplayOwnership(TargetId, pDisplayInfo);
    return StopDevice();
}

PAGED_CODE_SEG_END

//
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()

// SUBMIT_PRESENT_FENCE completion: the host retired the event-ring fence (real GPU
// completion), so wake the UMD's present thread.  Runs at DISPATCH from the response
// DPC drain -- must be non-paged.  The UMD's own handle keeps the event referenced,
// so ObDereferenceObject here never triggers deletion at raised IRQL.
static void PresentFenceCb(void *ctx, void *, void *)
{
    VioGpuAdapter::PRESENT_FENCE_CTX *pCtx = (VioGpuAdapter::PRESENT_FENCE_CTX *)ctx;
    VioGpuAdapter *pAdapter = pCtx->pAdapter;

    // Publish the retirement BEFORE waking anyone, so a waiter that runs the
    // instant it is signalled already sees the token as done.
    pAdapter->PresentTokenRetire(pCtx->Token);
    // Release any packet-gated present parked on a now-retired token.
    pAdapter->PresentWaitSweep();

    if (pCtx->pEvent != NULL)
    {
        KeSetEvent(pCtx->pEvent, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(pCtx->pEvent);
    }

    ExFreeToNPagedLookasideList(&pAdapter->m_PresentFenceLookaside, pCtx);

    // A flip may have been waiting on exactly this token; scan it out now
    // rather than at the next vsync tick (which would cost up to a full
    // refresh period of latency).  Only wakes the flip thread -- the scanout
    // itself stays at PASSIVE_LEVEL where FlushToScreen expects to run.
    pAdapter->vidpn.OnPresentTokenRetired();
}

// ---- monitored-fence gates -------------------------------------------------
//
// GateArm runs at PASSIVE (escape); GateFenceCb at DISPATCH (response DPC
// drain); GateConsume at PASSIVE (commander worker).  m_GateLock is the only
// lock; the completion-watermark hold rides the interrupt lock via the
// SynchronizeExecution routines below (a driver lock may not be held across
// DxgkCbSynchronizeExecution -- see the fence-report comment block).

NTSTATUS VioGpuAdapter::GateArm(VioGpuDevice *pDevice, ULONG ringIdx, ULONGLONG *outToken)
{
    GATE_TOKEN_CTX *g = new (NonPagedPoolNx) GATE_TOKEN_CTX;
    if (g == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(g, sizeof(*g));
    g->pAdapter = this;
    g->Token = (ULONGLONG)InterlockedIncrement64(&m_GateTokenNext);
    InitializeListHead(&g->Consumers);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_GateLock, &oldIrql);
    InsertTailList(&m_GateList, &g->Entry);
    KeReleaseSpinLock(&m_GateLock, oldIrql);

    *outToken = g->Token;

    // Empty fenced SUBMIT_3D on the event ring: the host defers its
    // used-ring response until the armed proxy fires at real GPU
    // completion, so GateFenceCb runs exactly when the queue's submitted
    // work has drained on the host GPU.
    if (!ctrlQueue.SubmitCommand(NULL, 0, pDevice->m_Context.GetId(), TRUE, ringIdx, GateFenceCb, g))
    {
        // The submit never queued (vbuf pool exhausted), so GateFenceCb
        // will never fire and any VIOGPU_CMD_GATE on this token would park
        // forever (node timeout -> TDR).  No callback is pending, so unlink
        // and free the token and fail the arm; the guest then skips gating
        // this batch rather than hanging on it.
        KeAcquireSpinLock(&m_GateLock, &oldIrql);
        RemoveEntryList(&g->Entry);
        KeReleaseSpinLock(&m_GateLock, oldIrql);
        delete g;
        *outToken = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

void VioGpuAdapter::GateFenceCb(void *ctx, void *, void *)
{
    GATE_TOKEN_CTX *g = (GATE_TOKEN_CTX *)ctx;
    VioGpuAdapter *pAdapter = g->pAdapter;

    LIST_ENTRY consumers;
    InitializeListHead(&consumers);

    KIRQL oldIrql;
    KeAcquireSpinLock(&pAdapter->m_GateLock, &oldIrql);
    g->Fired = TRUE;
    if (g->Expired)
    {
        // The deadline already completed this token's parked consumers.  A
        // late fire arriving at all means the host was slow, not dead --
        // log it so an expiry burst can be told apart from a worker death
        // (which never produces this line).
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s: LATE fire for expired gate token %llu (host slow, not dead)\n",
                  __FUNCTION__, g->Token));
    }
    while (!IsListEmpty(&g->Consumers))
    {
        InsertTailList(&consumers, RemoveHeadList(&g->Consumers));
    }
    // Free at fire once a consumer has been seen (the normal flow); a
    // resubmitted packet that arrives later treats the missing token as
    // fired.  A fired-unconsumed entry stays for the in-flight
    // VIOGPU_CMD_GATE to find.
    BOOLEAN freeIt = g->Consumed;
    if (freeIt)
    {
        RemoveEntryList(&g->Entry);
    }
    KeReleaseSpinLock(&pAdapter->m_GateLock, oldIrql);

    while (!IsListEmpty(&consumers))
    {
        GATE_CONSUMER *c = CONTAINING_RECORD(RemoveHeadList(&consumers), GATE_CONSUMER, Entry);
        // Completing the packet reports its id through the contiguity
        // window, advancing the watermark past the gate naturally.
        c->Cb(c->Ctx, NULL, NULL);
        delete c;
    }
    if (freeIt)
    {
        delete g;
    }
}

// ---- packet-gated presents (see the block comment in viogpu_adapter.h) ----

// Park a flip present's DMA packet until `token` retires.  Runs at PASSIVE
// (commander worker, VioGpuCommand::Run); the callback fires from
// PresentWaitSweep at DISPATCH (retire DPC / expiry DPC) or inline here when
// the token has already retired.
void VioGpuAdapter::PresentWaitConsume(ULONGLONG token, void (*cb)(void *, void *, void *), void *ctx, UINT fenceId)
{
    PRESENT_WAIT_CONSUMER *w = new (NonPagedPoolNx) PRESENT_WAIT_CONSUMER;
    BOOLEAN immediate = (w == NULL); // OOM: complete EARLY, degraded but safe
    if (w != NULL)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_PresentTokenLock, &oldIrql);
        if (PresentTokenRetiredLocked(token))
        {
            immediate = TRUE;
        }
        else
        {
            w->Token = token;
            w->Cb = cb;
            w->Ctx = ctx;
            w->ParkTime = KeQueryInterruptTime();
            InsertTailList(&m_PresentWaitList, &w->Entry);
        }
        KeReleaseSpinLock(&m_PresentTokenLock, oldIrql);
    }

    if (immediate)
    {
        if (w != NULL)
        {
            delete w;
        }
        else
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s: OOM parking present token %llu; completing fence_id=%u EARLY\n",
                      __FUNCTION__, token, fenceId));
        }
        cb(ctx, NULL, NULL);
    }
}

// Fire every parked consumer whose token has retired.  <= DISPATCH; the
// callbacks run outside the lock (QueueRunningCb is DPC-safe by the same
// contract GateFenceCb relies on).
void VioGpuAdapter::PresentWaitSweep(void)
{
    LIST_ENTRY fired;
    InitializeListHead(&fired);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PresentTokenLock, &oldIrql);
    for (LIST_ENTRY *e = m_PresentWaitList.Flink; e != &m_PresentWaitList;)
    {
        PRESENT_WAIT_CONSUMER *w = CONTAINING_RECORD(e, PRESENT_WAIT_CONSUMER, Entry);
        e = e->Flink;
        if (PresentTokenRetiredLocked(w->Token))
        {
            RemoveEntryList(&w->Entry);
            InsertTailList(&fired, &w->Entry);
        }
    }
    KeReleaseSpinLock(&m_PresentTokenLock, oldIrql);

    while (!IsListEmpty(&fired))
    {
        PRESENT_WAIT_CONSUMER *w = CONTAINING_RECORD(RemoveHeadList(&fired), PRESENT_WAIT_CONSUMER, Entry);
        w->Cb(w->Ctx, NULL, NULL);
        delete w;
    }
}

// Parked-gate deadline scan (period + rationale at the declaration).  Runs
// at DISPATCH; the consumer callback (VioGpuCommand::QueueRunningCb) is
// DPC-safe by contract -- GateFenceCb already invokes it from the response
// DPC drain.
VOID VioGpuAdapter::GateExpiryDpcRoutine(_In_ struct _KDPC *Dpc,
                                         _In_opt_ PVOID DeferredContext,
                                         _In_opt_ PVOID SystemArgument1,
                                         _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    VioGpuAdapter *a = (VioGpuAdapter *)DeferredContext;
    if (a == NULL)
    {
        return;
    }

    LIST_ENTRY expired;
    InitializeListHead(&expired);
    const ULONGLONG now = KeQueryInterruptTime();

    KIRQL oldIrql;
    KeAcquireSpinLock(&a->m_GateLock, &oldIrql);
    for (LIST_ENTRY *e = a->m_GateList.Flink; e != &a->m_GateList; e = e->Flink)
    {
        GATE_TOKEN_CTX *g = CONTAINING_RECORD(e, GATE_TOKEN_CTX, Entry);
        if (g->Fired || g->ParkTime == 0 || IsListEmpty(&g->Consumers))
        {
            continue;
        }
        if (now - g->ParkTime < VIOGPU_GATE_PARK_DEADLINE_100NS)
        {
            continue;
        }
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s: gate token %llu parked %llu ms > deadline; completing its "
                  "DMA EARLY (host worker dead or badly stalled)\n",
                  __FUNCTION__, g->Token, (now - g->ParkTime) / 10000ULL));
        while (!IsListEmpty(&g->Consumers))
        {
            InsertTailList(&expired, RemoveHeadList(&g->Consumers));
        }
        // Entry stays listed: its GateFenceCb is still pending and frees it
        // on a late fire (Consumed set).  If the fire never comes (dead
        // worker), the entry is swept at adapter teardown.
        g->Consumed = TRUE;
        g->Expired = TRUE;
        g->ParkTime = 0;
    }
    KeReleaseSpinLock(&a->m_GateLock, oldIrql);

    while (!IsListEmpty(&expired))
    {
        GATE_CONSUMER *c = CONTAINING_RECORD(RemoveHeadList(&expired), GATE_CONSUMER, Entry);
        c->Cb(c->Ctx, NULL, NULL);
        delete c;
    }

    // Same deadline for packet-gated presents: a present token that never
    // retires (dead host worker) must not park its packet into dxgkrnl's
    // TDR budget either.
    LIST_ENTRY expiredPresents;
    InitializeListHead(&expiredPresents);
    KeAcquireSpinLock(&a->m_PresentTokenLock, &oldIrql);
    for (LIST_ENTRY *e = a->m_PresentWaitList.Flink; e != &a->m_PresentWaitList;)
    {
        PRESENT_WAIT_CONSUMER *w = CONTAINING_RECORD(e, PRESENT_WAIT_CONSUMER, Entry);
        e = e->Flink;
        if (now - w->ParkTime < VIOGPU_GATE_PARK_DEADLINE_100NS)
        {
            continue;
        }
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s: present token %llu parked %llu ms > deadline; completing its "
                  "DMA EARLY (host worker dead or badly stalled)\n",
                  __FUNCTION__, w->Token, (now - w->ParkTime) / 10000ULL));
        RemoveEntryList(&w->Entry);
        InsertTailList(&expiredPresents, &w->Entry);
    }
    KeReleaseSpinLock(&a->m_PresentTokenLock, oldIrql);

    while (!IsListEmpty(&expiredPresents))
    {
        PRESENT_WAIT_CONSUMER *w =
            CONTAINING_RECORD(RemoveHeadList(&expiredPresents), PRESENT_WAIT_CONSUMER, Entry);
        w->Cb(w->Ctx, NULL, NULL);
        delete w;
    }
}

void VioGpuAdapter::GateConsume(ULONGLONG token, void (*cb)(void *, void *, void *), void *ctx, UINT fenceId)
{
    GATE_CONSUMER *c = new (NonPagedPoolNx) GATE_CONSUMER;
    BOOLEAN immediate = (c == NULL);
    if (c != NULL)
    {
        c->Cb = cb;
        c->Ctx = ctx;
        c->FenceId = fenceId;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_GateLock, &oldIrql);
    GATE_TOKEN_CTX *g = NULL;
    for (LIST_ENTRY *e = m_GateList.Flink; e != &m_GateList; e = e->Flink)
    {
        GATE_TOKEN_CTX *cand = CONTAINING_RECORD(e, GATE_TOKEN_CTX, Entry);
        if (cand->Token == token)
        {
            g = cand;
            break;
        }
    }
    // Only an entry we unlink here may be freed below; a still-armed entry
    // (the OOM case) stays listed with its GateFenceCb callback pending.
    GATE_TOKEN_CTX *gToDelete = NULL;
    if (g == NULL || g->Fired)
    {
        // Unknown token (already fired+freed, or a resubmission after the
        // fire) or fired-waiting-for-consume: complete immediately.  A
        // fired-unconsumed entry is ours to free now.
        if (g != NULL)
        {
            RemoveEntryList(&g->Entry);
            gToDelete = g;
        }
        immediate = TRUE;
    }
    else if (c != NULL)
    {
        // Deadline starts when the first consumer parks (an armed token with
        // no parked packet blocks nothing).  A re-park after an expiry (the
        // preempt-resubmit path) restamps, giving the resubmission its own
        // full deadline.
        if (IsListEmpty(&g->Consumers))
        {
            g->ParkTime = KeQueryInterruptTime();
        }
        InsertTailList(&g->Consumers, &c->Entry);
        g->Consumed = TRUE;
    }
    else
    {
        // OOM: cannot park this consumer.  Leave the armed entry listed
        // (its fence callback is still pending and will fire it later) and
        // complete this fence EARLY -- degraded but memory-safe.  Freeing
        // g here would leave GateFenceCb walking a dead entry.
        immediate = TRUE;
    }
    KeReleaseSpinLock(&m_GateLock, oldIrql);

    if (immediate)
    {
        if (gToDelete != NULL)
        {
            delete gToDelete;
        }
        if (c != NULL)
        {
            delete c;
        }
        cb(ctx, NULL, NULL);
        if (c == NULL)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s: OOM parking gate token %llu; completing fence_id=%u EARLY\n",
                      __FUNCTION__, token, fenceId));
        }
    }
}

// ---- gate-hold sync routines (interrupt-lock domain) -----------------------

// A deferred monitored-fence value becomes visible when the completion
// watermark covers the signal's own packet: lastCompleted >= fenceId.
// The contiguity window guarantees the watermark never covers an
// incomplete id, so every earlier node submission (gates included) has
// truly completed by then -- which is exactly when dxgkrnl learns this
// packet completed, the moment the contract requires the value.
static BOOLEAN MFenceDeferReadyLocked(VioGpuAdapter *a, UINT fenceId)
{
    return (LONG)((UINT)a->m_LastCompletedFenceId - fenceId) >= 0;
}

// Publish every MFENCE_DEFER value the current watermark covers.  Runs
// inside the interrupt lock at DIRQL, so it is nothing but bare stores
// through KVAs pinned at build time -- no lock, no mapping call, no
// allocation.  Written entries move to `done` for the caller to unpin
// and free outside the lock.
static void MFenceDeferWriteReadyLocked(VioGpuAdapter *a, LIST_ENTRY *done)
{
    for (LIST_ENTRY *e = a->m_MFenceDeferList.Flink; e != &a->m_MFenceDeferList;)
    {
        VioGpuAdapter::MFENCE_DEFER *d = CONTAINING_RECORD(e, VioGpuAdapter::MFENCE_DEFER, Entry);
        e = e->Flink;
        if (MFenceDeferReadyLocked(a, d->FenceId))
        {
            *d->Kva = d->Value;
            KeMemoryBarrier();
            RemoveEntryList(&d->Entry);
            InsertTailList(done, &d->Entry);
        }
    }
}

// ---- GPU-VA shadow ---------------------------------------------------------
//
// Reached only from DxgkDdiBuildPagingBuffer, which dxgkrnl serializes and
// calls at PASSIVE_LEVEL, so the table needs no lock of its own.

void VioGpuAdapter::VaShadowInsert(HANDLE hProcess, ULONGLONG vaPage, ULONGLONG physPage)
{
    const ULONG start = (ULONG)((vaPage >> PAGE_SHIFT) & (VA_SHADOW_SIZE - 1));
    ULONG tomb = VA_SHADOW_SIZE; // first tombstone seen, if any
    for (ULONG probe = 0; probe < VA_SHADOW_SIZE; probe++)
    {
        const ULONG idx = (start + probe) & (VA_SHADOW_SIZE - 1);
        VaShadowEntry *e = &m_vaShadow[idx];
        if (e->vaPage == vaPage && e->hProcess == hProcess)
        {
            e->phys = physPage; // update in place
            return;
        }
        if (e->vaPage == 0)
        {
            VaShadowEntry *slot = (tomb != VA_SHADOW_SIZE) ? &m_vaShadow[tomb] : e;
            slot->hProcess = hProcess;
            slot->vaPage = vaPage;
            slot->phys = physPage;
            return;
        }
        if (e->phys == 0 && tomb == VA_SHADOW_SIZE)
        {
            tomb = idx; // reuse the earliest tombstone
        }
    }
    if (tomb != VA_SHADOW_SIZE)
    {
        m_vaShadow[tomb].hProcess = hProcess;
        m_vaShadow[tomb].vaPage = vaPage;
        m_vaShadow[tomb].phys = physPage;
    }
}

void VioGpuAdapter::VaShadowRemove(HANDLE hProcess, ULONGLONG vaPage)
{
    const ULONG start = (ULONG)((vaPage >> PAGE_SHIFT) & (VA_SHADOW_SIZE - 1));
    for (ULONG probe = 0; probe < VA_SHADOW_SIZE; probe++)
    {
        const ULONG idx = (start + probe) & (VA_SHADOW_SIZE - 1);
        VaShadowEntry *e = &m_vaShadow[idx];
        if (e->vaPage == 0)
        {
            return;
        }
        if (e->vaPage != vaPage || e->hProcess != hProcess)
        {
            continue;
        }
        e->phys = 0;      // tombstone: probes past this slot must still work
        e->hProcess = NULL;
        // Nothing probes past the end of a chain, so a tombstone that ends
        // one can be emptied outright -- along with the run of tombstones
        // behind it.  Without this, unmapped entries accumulate until every
        // probe walks the whole table.
        const ULONG next = (idx + 1) & (VA_SHADOW_SIZE - 1);
        if (m_vaShadow[next].vaPage != 0)
        {
            return;
        }
        for (ULONG back = 0; back < VA_SHADOW_SIZE; back++)
        {
            VaShadowEntry *t = &m_vaShadow[(idx - back) & (VA_SHADOW_SIZE - 1)];
            if (t->vaPage == 0 || t->phys != 0)
            {
                break;
            }
            t->vaPage = 0;
        }
        return;
    }
}

// Resolve a GPU VA with no process to key on: walk the VA's probe chain and
// accept the result only when every address space holding that VA agrees on
// the page behind it.  A disagreement means the VA alone cannot name the
// caller's fence storage; writing either candidate would land 8 bytes in
// another process's fence memory, so the signal is refused instead.
BOOLEAN VioGpuAdapter::VaShadowLookup(ULONGLONG va, ULONGLONG *physOut)
{
    const ULONGLONG vaPage = va & ~((ULONGLONG)PAGE_SIZE - 1);
    const ULONG start = (ULONG)((vaPage >> PAGE_SHIFT) & (VA_SHADOW_SIZE - 1));
    ULONGLONG found = 0;
    for (ULONG probe = 0; probe < VA_SHADOW_SIZE; probe++)
    {
        const ULONG idx = (start + probe) & (VA_SHADOW_SIZE - 1);
        VaShadowEntry *e = &m_vaShadow[idx];
        if (e->vaPage == 0)
        {
            break;
        }
        if (e->vaPage != vaPage || e->phys == 0)
        {
            continue;
        }
        if (found != 0 && found != e->phys)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s: va=0x%llx maps 0x%llx and 0x%llx in different address spaces\n",
                      __FUNCTION__, va, found, e->phys));
            return FALSE;
        }
        found = e->phys;
    }
    if (found == 0)
    {
        return FALSE;
    }
    *physOut = found + (va & (PAGE_SIZE - 1));
    return TRUE;
}

void VioGpuAdapter::MFenceMapRelease(void)
{
    KIRQL oldIrql;
    for (ULONG i = 0; i < MFENCE_MAP_SLOTS; i++)
    {
        KeAcquireSpinLock(&m_MFenceMapLock, &oldIrql);
        volatile UINT64 *kva = m_MFenceMap[i].Kva;
        m_MFenceMap[i].Kva = NULL;
        m_MFenceMap[i].PhysPage = 0;
        KeReleaseSpinLock(&m_MFenceMapLock, oldIrql);
        if (kva != NULL)
        {
            MmUnmapIoSpace((PVOID)kva, PAGE_SIZE);
        }
    }
}

// Perform the monitored-fence value write.  <= DISPATCH_LEVEL; callers are the
// commander worker and the paging build path (PASSIVE) and the completion DPC.
void VioGpuAdapter::MFenceWrite(ULONGLONG phys, UINT64 value)
{
    const ULONGLONG physPage = phys & ~((ULONGLONG)PAGE_SIZE - 1);
    const ULONG offset = (ULONG)(phys & (PAGE_SIZE - 1));
    if (offset + sizeof(UINT64) > PAGE_SIZE)
    {
        // A fence value straddling two pages would need both mappings, and
        // dxgkrnl aligns monitored-fence storage so it never does.
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s: unaligned phys=0x%llx val=%llu\n", __FUNCTION__, phys, value));
        return;
    }

    const ULONG idx = (ULONG)((physPage >> PAGE_SHIFT) % MFENCE_MAP_SLOTS);
    KIRQL oldIrql;

    KeAcquireSpinLock(&m_MFenceMapLock, &oldIrql);
    if (m_MFenceMap[idx].PhysPage == physPage && m_MFenceMap[idx].Kva != NULL)
    {
        *(volatile UINT64 *)((PUCHAR)m_MFenceMap[idx].Kva + offset) = value;
        KeMemoryBarrier();
        KeReleaseSpinLock(&m_MFenceMapLock, oldIrql);
        return;
    }
    KeReleaseSpinLock(&m_MFenceMapLock, oldIrql);

    // Map outside the lock: MmMapIoSpaceEx reserves system PTEs, which is far
    // too long to hold a spin lock across.
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)physPage;
    volatile UINT64 *mapped = (volatile UINT64 *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE);
    if (mapped == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s: map failed phys=0x%llx val=%llu\n", __FUNCTION__, phys, value));
        return;
    }

    volatile UINT64 *stale;
    KeAcquireSpinLock(&m_MFenceMapLock, &oldIrql);
    if (m_MFenceMap[idx].PhysPage == physPage && m_MFenceMap[idx].Kva != NULL)
    {
        stale = mapped; // another caller installed the same page meanwhile
        mapped = m_MFenceMap[idx].Kva;
    }
    else if (m_MFenceMap[idx].Kva != NULL && m_MFenceMap[idx].Pins > 0)
    {
        // The slot's page is pinned by outstanding MFENCE_DEFER records
        // whose KVAs must stay valid: don't evict it.  Write through the
        // transient mapping instead and drop it.
        stale = mapped;
    }
    else
    {
        stale = m_MFenceMap[idx].Kva;
        m_MFenceMap[idx].PhysPage = physPage;
        m_MFenceMap[idx].Kva = mapped;
    }
    *(volatile UINT64 *)((PUCHAR)mapped + offset) = value;
    KeMemoryBarrier();
    KeReleaseSpinLock(&m_MFenceMapLock, oldIrql);

    // Safe to unmap once the slot no longer names it: every write through a
    // slot happens under the lock we just dropped.
    if (stale != NULL)
    {
        MmUnmapIoSpace((PVOID)stale, PAGE_SIZE);
    }
}

volatile UINT64 *VioGpuAdapter::MFenceMapPin(ULONGLONG phys)
{
    const ULONGLONG physPage = phys & ~((ULONGLONG)PAGE_SIZE - 1);
    const ULONG offset = (ULONG)(phys & (PAGE_SIZE - 1));
    if (offset + sizeof(UINT64) > PAGE_SIZE)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s: unaligned phys=0x%llx\n", __FUNCTION__, phys));
        return NULL;
    }

    const ULONG idx = (ULONG)((physPage >> PAGE_SHIFT) % MFENCE_MAP_SLOTS);
    KIRQL oldIrql;

    KeAcquireSpinLock(&m_MFenceMapLock, &oldIrql);
    if (m_MFenceMap[idx].PhysPage == physPage && m_MFenceMap[idx].Kva != NULL)
    {
        m_MFenceMap[idx].Pins++;
        volatile UINT64 *kva = (volatile UINT64 *)((PUCHAR)m_MFenceMap[idx].Kva + offset);
        KeReleaseSpinLock(&m_MFenceMapLock, oldIrql);
        return kva;
    }
    KeReleaseSpinLock(&m_MFenceMapLock, oldIrql);

    // Map outside the lock (PASSIVE): MmMapIoSpaceEx reserves system PTEs.
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)physPage;
    volatile UINT64 *mapped = (volatile UINT64 *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE);
    if (mapped == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s: map failed phys=0x%llx\n", __FUNCTION__, phys));
        return NULL;
    }

    volatile UINT64 *stale;
    volatile UINT64 *base;
    KeAcquireSpinLock(&m_MFenceMapLock, &oldIrql);
    if (m_MFenceMap[idx].PhysPage == physPage && m_MFenceMap[idx].Kva != NULL)
    {
        stale = mapped; // another caller installed the same page meanwhile
        base = m_MFenceMap[idx].Kva;
        m_MFenceMap[idx].Pins++;
    }
    else if (m_MFenceMap[idx].Kva != NULL && m_MFenceMap[idx].Pins > 0)
    {
        // Direct-mapped collision with a pinned page.  Evicting it would
        // invalidate outstanding KVAs, and an un-cached private mapping
        // has no unpin bookkeeping -- report failure; the caller falls
        // back to the immediate write.
        stale = mapped;
        base = NULL;
    }
    else
    {
        stale = m_MFenceMap[idx].Kva;
        m_MFenceMap[idx].PhysPage = physPage;
        m_MFenceMap[idx].Kva = mapped;
        m_MFenceMap[idx].Pins = 1;
        base = mapped;
    }
    KeReleaseSpinLock(&m_MFenceMapLock, oldIrql);

    if (stale != NULL)
    {
        MmUnmapIoSpace((PVOID)stale, PAGE_SIZE);
    }
    return (base != NULL) ? (volatile UINT64 *)((PUCHAR)base + offset) : NULL;
}

void VioGpuAdapter::MFenceMapUnpin(ULONGLONG phys)
{
    const ULONGLONG physPage = phys & ~((ULONGLONG)PAGE_SIZE - 1);
    const ULONG idx = (ULONG)((physPage >> PAGE_SHIFT) % MFENCE_MAP_SLOTS);
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_MFenceMapLock, &oldIrql);
    if (m_MFenceMap[idx].PhysPage == physPage && m_MFenceMap[idx].Pins > 0)
    {
        m_MFenceMap[idx].Pins--;
    }
    KeReleaseSpinLock(&m_MFenceMapLock, oldIrql);
}

struct MFENCE_INSERT_CONTEXT
{
    VioGpuAdapter *pAdapter;
    VioGpuAdapter::MFENCE_DEFER *pDefer;
    BOOLEAN Inserted;
};

static BOOLEAN MFenceDeferInsertRoutine(PVOID ctx_void)
{
    MFENCE_INSERT_CONTEXT *ctx = (MFENCE_INSERT_CONTEXT *)ctx_void;
    VioGpuAdapter *a = ctx->pAdapter;
    if (MFenceDeferReadyLocked(a, ctx->pDefer->FenceId))
    {
        // Already covered (preempt skip-through ran the watermark past
        // this id): no later completion is guaranteed to pop the entry,
        // so the caller writes immediately.
        ctx->Inserted = FALSE;
        return TRUE;
    }
    InsertTailList(&a->m_MFenceDeferList, &ctx->pDefer->Entry);
    ctx->Inserted = TRUE;
    return TRUE;
}

void VioGpuAdapter::MFenceDeferOrWrite(UINT fenceId, volatile UINT64 *kva, ULONGLONG phys, UINT64 value)
{
    MFENCE_DEFER *d = new (NonPagedPoolNx) MFENCE_DEFER;
    if (d == NULL)
    {
        // OOM: early visibility over a lost signal.
        *kva = value;
        KeMemoryBarrier();
        MFenceMapUnpin(phys);
        return;
    }
    d->FenceId = fenceId;
    d->Kva = kva;
    d->Phys = phys;
    d->Value = value;

    MFENCE_INSERT_CONTEXT sctx = {};
    sctx.pAdapter = this;
    sctx.pDefer = d;
    BOOLEAN bRet;
    m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle,
                                               MFenceDeferInsertRoutine, &sctx, 0, &bRet);
    if (!sctx.Inserted)
    {
        *kva = value;
        KeMemoryBarrier();
        MFenceMapUnpin(phys);
        delete d;
    }
}

VOID VioGpuAdapter::DpcRoutine(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_VBUFFER pvbuf = NULL;
    UINT len = 0;
    ULONG reason;
    while ((reason = InterlockedExchange((PLONG)&m_PendingWorks, 0)) != 0)
    {
        if ((reason & ISR_REASON_DISPLAY))
        {
            while ((pvbuf = ctrlQueue.DequeueBuffer(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s ctrlQueue pvbuf = %p len = %d\n", __FUNCTION__, pvbuf, len));

                PGPU_CTRL_HDR pcmd = (PGPU_CTRL_HDR)pvbuf->buf;
                PGPU_CTRL_HDR resp = (PGPU_CTRL_HDR)pvbuf->resp_buf;

                // resp_buf is allocated in GetBuf alongside the vbuf,
                // so in normal flow it's never NULL -- but defensive:
                // a vbuf rebuilt without a response (zero resp_size)
                // would land here with resp == NULL, and the error
                // check below would deref it.
                if (!resp)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--> %s pvbuf=%p has no resp_buf for cmd_type=0x%x\n",
                              __FUNCTION__, pvbuf, pcmd ? pcmd->type : 0));
                    if (pvbuf->complete_cb != NULL &&
                        InterlockedExchange(&pvbuf->complete_fired, 1) == 0)
                    {
                        pvbuf->complete_cb(pvbuf->complete_ctx, pvbuf->buf, NULL);
                    }
                    if (pvbuf->auto_release)
                    {
                        ctrlQueue.ReleaseBuffer(pvbuf);
                    }
                    continue;
                }

                if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
                {
                    if (pcmd->type == VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB)
                    {
                        PGPU_RES_CREATE_BLOB blob_req = (PGPU_RES_CREATE_BLOB)pvbuf->buf;
                        DbgPrint(TRACE_LEVEL_FATAL,
                                 ("!!!!! Command %x (create blob) for res_id=%d blob_id=%lld flags=%x failed: %x\n",
                                  pcmd->type,
                                  blob_req->resource_id,
                                  blob_req->blob_id,
                                  blob_req->blob_flags,
                                  resp->type));
                    }
                    else if (pcmd->type == VIRTIO_GPU_CMD_RESOURCE_UNREF)
                    {
                        // Name the resource: a failure here is normally a
                        // double-unref, which is only actionable if the id is
                        // known.
                        PGPU_RES_UNREF unref_req = (PGPU_RES_UNREF)pvbuf->buf;
                        DbgPrint(TRACE_LEVEL_FATAL,
                                 ("!!!!! Command %x (unref) res_id=%d failed: %x\n",
                                  pcmd->type, unref_req->resource_id, resp->type));
                    }
                    else
                    {
                        DbgPrint(TRACE_LEVEL_FATAL, ("!!!!! Command %x failed: %x\n", pcmd->type, resp->type));
                    }
                    // The completion callback fires unconditionally
                    // below: callbacks that care about host-side
                    // failure (Ask*/Create*) must inspect resp->type
                    // before treating the call as successful. The
                    // command-submission path (QueueRunningCb) does
                    // not yet surface the error to DXGK; without the
                    // per-fence tracking that lives in the venus
                    // backend, the fence still completes from DXGK's
                    // point of view.
                }
                if (resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE,
                             ("<--- %s type = %xlu flags = %lu fence_id = %llu ctx_id = %lu cmd_type = %lu\n",
                              __FUNCTION__,
                              resp->type,
                              resp->flags,
                              resp->fence_id,
                              resp->ctx_id,
                              pcmd->type));
                }
                if (pvbuf->complete_cb != NULL &&
                    InterlockedExchange(&pvbuf->complete_fired, 1) == 0)
                {
                    pvbuf->complete_cb(pvbuf->complete_ctx, pvbuf->buf, pvbuf->resp_buf);
                }
                if (pvbuf->auto_release)
                {
                    ctrlQueue.ReleaseBuffer(pvbuf);
                }
            };
        }
        if ((reason & ISR_REASON_CURSOR))
        {
            while ((pvbuf = m_CursorQueue.DequeueCursor(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("---> %s m_CursorQueue pvbuf = %p len = %u\n", __FUNCTION__, pvbuf, len));
                m_CursorQueue.ReleaseBuffer(pvbuf);
            };
        }
        if (reason & ISR_REASON_CHANGE)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s ConfigChanged\n", __FUNCTION__));
            KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    // DxgkCbNotifyDpc commits the interrupt notifications that
    // DxgkCbNotifyInterrupt queued so the scheduler acts on them. DMA
    // completions and CRTC vsyncs are notified from the command-worker and
    // flip threads (NotifyInterrupt -> DxgkCbQueueDpc), which do not set
    // m_PendingWorks. Calling this only after draining a hardware ISR
    // reason dropped every such notification, so the scheduler never
    // observed DMA fences completing and timed the engine out. Commit
    // unconditionally: with no pending notifications it is a cheap no-op.
    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VOID VioGpuAdapter::ResetDevice(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    // PnP fault / surprise-removal recovery path. StopDevice +
    // VioGpuAdapterClose handle the full teardown and reinit; this
    // DDI runs outside that flow and just needs the device left
    // in a quiescent state: queues with interrupts disabled, then a
    // virtio device reset.
    if (IsHardwareInit())
    {
        ctrlQueue.DisableInterrupt();
        m_CursorQueue.DisableInterrupt();
        virtio_device_reset(&m_VioDev);
    }
}

#pragma code_seg(pop) // End Non-Paged Code

PAGED_CODE_SEG_BEGIN
NTSTATUS VioGpuAdapter::WriteRegistryString(_In_ HANDLE DevInstRegKeyHandle,
                                            _In_ PCWSTR pszwValueName,
                                            _In_ PCSTR pszValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    ANSI_STRING AnsiStrValue;
    UNICODE_STRING UnicodeStrValue;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    RtlInitAnsiString(&AnsiStrValue, pszValue);
    Status = RtlAnsiStringToUnicodeString(&UnicodeStrValue, &AnsiStrValue, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlAnsiStringToUnicodeString failed with Status: 0x%X\n", Status));
        return Status;
    }

    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &UnicodeStrValueName,
                           0,
                           REG_SZ,
                           UnicodeStrValue.Buffer,
                           UnicodeStrValue.MaximumLength);

    RtlFreeUnicodeString(&UnicodeStrValue);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::WriteRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle,
                                           _In_ PCWSTR pszwValueName,
                                           _In_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwSetValueKey(DevInstRegKeyHandle, &UnicodeStrValueName, 0, REG_DWORD, pdwValue, sizeof(DWORD));

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::ReadRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle,
                                          _In_ PCWSTR pszwValueName,
                                          _Inout_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    ULONG ulRes;
    UCHAR Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(DWORD)];
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwQueryValueKey(DevInstRegKeyHandle,
                             &UnicodeStrValueName,
                             KeyValuePartialInformation,
                             Buf,
                             sizeof(Buf),
                             &ulRes);

    if (Status == STATUS_SUCCESS)
    {
        if (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Type == REG_DWORD &&
            (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->DataLength == sizeof(DWORD)))
        {
            ASSERT(((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->DataLength == sizeof(DWORD));
            *pdwValue = *((PDWORD) & (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Data));
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
            VioGpuDbgBreak();
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwQueryValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::SetRegisterInfo(_In_ ULONG Id, _In_ DWORD MemSize)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCSTR StrHWInfoChipType = "QEMU VIRTIO GPU";
    PCSTR StrHWInfoDacType = "VIRTIO GPU";
    PCSTR StrHWInfoAdapterString = "VIRTIO GPU";
    PCSTR StrHWInfoBiosString = "SEABIOS VIRTIO GPU";

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    do
    {
        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.ChipType", StrHWInfoChipType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for ChipType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.DacType", StrHWInfoDacType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed DacType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.AdapterString", StrHWInfoAdapterString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for AdapterString with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.BiosString", StrHWInfoBiosString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for BiosString with Status: 0x%X", Status));
            break;
        }

        DWORD MemorySize = MemSize;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"HardwareInformation.MemorySize", &MemorySize);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for MemorySize with Status: 0x%X", Status));
            break;
        }

        DWORD DeviceId = Id;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"VioGpuAdapterID", &DeviceId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for VioGpuAdapterID with Status: 0x%X", Status));
        }
    } while (0);

    ZwClose(DevInstRegKeyHandle);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::GetRegisterInfo(void)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    DWORD value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"HWCursor", &value);
    if (NT_SUCCESS(Status))
    {
        SetPointerEnabled(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"FlexResolution", &value);
    if (NT_SUCCESS(Status))
    {
        SetFlexResolution(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"UsePhysicalMemory", &value);
    if (NT_SUCCESS(Status))
    {
        SetUsePhysicalMemory(!!value);
    }

    ZwClose(DevInstRegKeyHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::GetPCIInfo(void)
{
    PAGED_CODE();

    NTSTATUS status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    ULONG len;
    UINT32 pciBus;
	UINT32 pciAddr;

	status = IoGetDeviceProperty(m_pPhysicalDevice, DevicePropertyBusNumber, sizeof(pciBus), (PVOID)&pciBus, &len);
	if(!NT_SUCCESS(status)) {
		DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoGetDeviceProperty failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, status));
		return status;
	}

	status = IoGetDeviceProperty(m_pPhysicalDevice, DevicePropertyAddress, sizeof(pciAddr), (PVOID)&pciAddr, &len);
	if(!NT_SUCCESS(status)) {
		DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoGetDeviceProperty failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, status));
		return status;
	}

    m_PciBus = pciBus;
    m_PciDev = (pciAddr >> 16) & 0xFFFF;
    m_PciFunc = pciAddr & 0xFFFF;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return status;
}
PAGED_CODE_SEG_END

PAGED_CODE_SEG_BEGIN

NTSTATUS VioGpuAdapter::VioGpuAdapterInit()
{
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (IsHardwareInit())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Already Initialized\n"));
        VioGpuDbgBreak();
        return status;
    }
    status = VirtIoDeviceInit();
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio device, error %x\n", status));
        VioGpuDbgBreak();
        return status;
    }

    m_u64HostFeatures = virtio_get_features(&m_VioDev);
    m_u64GuestFeatures = 0;
    do
    {
        struct virtqueue *vqs[2];
        if (!AckFeature(VIRTIO_F_VERSION_1))
        {
            status = STATUS_UNSUCCESSFUL;
            break;
        }
#if (NTDDI_VERSION >= NTDDI_WIN10)
        AckFeature(VIRTIO_F_ACCESS_PLATFORM);
#endif

        // Ack the feature bits the driver implements so the host
        // actually honours the corresponding fields in ctx_init and
        // resource_uuid commands; an unset bit makes those fields
        // silently ignored.
        AckFeature(VIRTIO_GPU_F_CONTEXT_INIT);
        AckFeature(VIRTIO_GPU_F_RESOURCE_UUID);
        AckFeature(VIRTIO_GPU_F_VIRGL);
        AckFeature(VIRTIO_GPU_F_RESOURCE_BLOB);

        status = virtio_set_features(&m_VioDev, m_u64GuestFeatures);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s virtio_set_features failed with %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            break;
        }

        status = virtio_find_queues(&m_VioDev, 2, vqs);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("virtio_find_queues failed with error %x\n", status));
            VioGpuDbgBreak();
            break;
        }

        if (!ctrlQueue.Init(&m_VioDev, vqs[0], 0) || !m_CursorQueue.Init(&m_VioDev, vqs[1], 1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio queues\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        virtio_get_config(&m_VioDev,
                          FIELD_OFFSET(GPU_CONFIG, num_scanouts),
                          &m_u32NumScanouts,
                          sizeof(m_u32NumScanouts));

        virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, num_capsets), &m_u32NumCapsets, sizeof(m_u32NumCapsets));
    } while (0);
    if (status == STATUS_SUCCESS)
    {
        virtio_device_ready(&m_VioDev);
        SetHardwareInit(TRUE);
    }
    else
    {
        virtio_add_status(&m_VioDev, VIRTIO_CONFIG_S_FAILED);
        VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return status;
}

void VioGpuAdapter::VioGpuAdapterClose()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s\n", __FUNCTION__));

    if (IsHardwareInit())
    {
        // The ISR samples IsHardwareInit() and then dereferences
        // ctrlQueue / m_CursorQueue / m_VioDev. Flip the flag and
        // disable interrupts under DxgkCb-synchronisation so the
        // ISR cannot read TRUE here and then touch state that the
        // teardown below is about to invalidate.
        BOOLEAN syncRet = FALSE;
        m_DxgkInterface.DxgkCbSynchronizeExecution(
            m_DxgkInterface.DeviceHandle,
            [](PVOID p) -> BOOLEAN {
                VioGpuAdapter *self = (VioGpuAdapter *)p;
                self->SetHardwareInit(FALSE);
                self->ctrlQueue.DisableInterrupt();
                self->m_CursorQueue.DisableInterrupt();
                return TRUE;
            },
            this, 0, &syncRet);
        virtio_device_reset(&m_VioDev);
        virtio_delete_queues(&m_VioDev);
        ctrlQueue.Close();
        m_CursorQueue.Close();
        virtio_device_shutdown(&m_VioDev);
        vidpn.Powerdown();
    }
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuAdapter::AckFeature(UINT64 Feature)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_u64HostFeatures, Feature))
    {
        virtio_feature_enable(m_u64GuestFeatures, Feature);
        return TRUE;
    }
    return FALSE;
}

NTSTATUS VioGpuAdapter::VirtIoDeviceInit()
{
    PAGED_CODE();

    return virtio_device_initialize(&m_VioDev,
                                    &VioGpuSystemOps,
                                    static_cast<IVioGpuPCI *>(this),
                                    m_PciResources.IsMSIEnabled());
}

VOID VioGpuAdapter::CreateResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL && m_ResolutionEventHandle != NULL)
    {
        return;
    }
    DECLARE_UNICODE_STRING_SIZE(DeviceNumber, 10);
    DECLARE_UNICODE_STRING_SIZE(EventName, 256);

    RtlIntegerToUnicodeString(m_Id, 10, &DeviceNumber);
    NTSTATUS status = RtlUnicodeStringPrintf(&EventName,
                                             L"%ws%ws%ws",
                                             BASE_NAMED_OBJECTS,
                                             RESOLUTION_EVENT_NAME,
                                             DeviceNumber.Buffer);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlUnicodeStringPrintf failed 0x%x\n", status));
        return;
    }
    m_ResolutionEvent = IoCreateNotificationEvent(&EventName, &m_ResolutionEventHandle);
    if (m_ResolutionEvent == NULL)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
        return;
    }
    KeClearEvent(m_ResolutionEvent);
    ObReferenceObject(m_ResolutionEvent);
}

VOID VioGpuAdapter::NotifyResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("NotifyResolutionEvent\n"));
        KeSetEvent(m_ResolutionEvent, IO_NO_INCREMENT, FALSE);
        KeClearEvent(m_ResolutionEvent);
    }
}

VOID VioGpuAdapter::CloseResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEventHandle != NULL)
    {
        ZwClose(m_ResolutionEventHandle);
        m_ResolutionEventHandle = NULL;
    }

    if (m_ResolutionEvent != NULL)
    {
        ObDereferenceObject(m_ResolutionEvent);
        m_ResolutionEvent = NULL;
    }
}

NTSTATUS VioGpuAdapter::HWInit(PCM_RESOURCE_LIST pResList)
{
    PAGED_CODE();

    NTSTATUS status = STATUS_SUCCESS;
    HANDLE threadHandle = 0;
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    UINT size = 0;
    do
    {
        if (!m_PciResources.Init(GetDxgkInterface(), pResList))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Incomplete resources\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        status = VioGpuAdapterInit();
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Failed initialize adapter %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            break;
        }

        size = ctrlQueue.QueryAllocation() + m_CursorQueue.QueryAllocation();
        DbgPrint(TRACE_LEVEL_FATAL, ("%s size %d\n", __FUNCTION__, size));
        ASSERT(size);

        if (!m_GpuBuf.Init(size))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize buffers\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        ctrlQueue.SetGpuBuf(&m_GpuBuf);
        m_CursorQueue.SetGpuBuf(&m_GpuBuf);

        if (!resourceIdr.Init(1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize id generator\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        if (!ctxIdr.Init(1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize id generator\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        m_supportedCapsetIDs = 0;
        for (UINT32 i = 0; i < m_u32NumCapsets; i++)
        {
            PGPU_VBUFFER vbuf = NULL;

            ctrlQueue.AskCapsetInfo(&vbuf, i);
            PGPU_RESP_CAPSET_INFO resp = (PGPU_RESP_CAPSET_INFO)vbuf->resp_buf;

            if (!resp)
            {
                DbgPrint(TRACE_LEVEL_FATAL, ("%s Failed to get info for capset %d", __FUNCTION__, i));
                continue;
            }

            ULONG capset_id = resp->capset_id;
            if (capset_id > 63 || capset_id <= 0)
            {
                continue; // Invalid capset id, capsets ids are in range from 1 to 63 per specification
            }
            m_capsetInfos[capset_id].id = capset_id;
            m_capsetInfos[capset_id].max_size = resp->capset_max_size;
            m_capsetInfos[capset_id].max_version = resp->capset_max_version;
            m_supportedCapsetIDs |= 1ull << capset_id;
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("CAPSET INFO %d    id: %d; version: %d; size: %d\n",
                      i,
                      capset_id,
                      resp->capset_max_size,
                      resp->capset_max_version));
        }

    } while (0);

    // Propagate a negotiation failure from the do/while(0) block; the
    // worker thread and frame segment cannot start on a half-initialised
    // virtio device.
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s aborting HWInit after negotiation failure status=0x%x\n",
                  __FUNCTION__, status));
        return status;
    }

    status = PsCreateSystemThread(&threadHandle,
                                  (ACCESS_MASK)0,
                                  NULL,
                                  (HANDLE)0,
                                  NULL,
                                  VioGpuAdapter::ThreadWork,
                                  this);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to create system thread, status %x\n", __FUNCTION__, status));
        VioGpuDbgBreak();
        return status;
    }
    // ObReferenceObjectByHandle must succeed or HWClose has no way to
    // wait on / dereference the running kernel thread. On failure,
    // signal the thread to exit and fail HWInit so no orphan worker
    // outlives this call.
    status = ObReferenceObjectByHandle(threadHandle,
                                       THREAD_ALL_ACCESS,
                                       NULL,
                                       KernelMode,
                                       (PVOID *)(&m_pWorkThread),
                                       NULL);
    ZwClose(threadHandle);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s ObReferenceObjectByHandle failed status=0x%x; signalling worker to exit\n",
                  __FUNCTION__, status));
        m_pWorkThread = NULL;
        m_bStopWorkThread = TRUE;
        KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        VioGpuDbgBreak();
        return status;
    }

    // FIXME: bar 0 is not required to be present
    PHYSICAL_ADDRESS fb_pa = m_PciResources.GetPciBar(0)->GetPA();
    // The framebuffer segment is described with 32-bit sizes; clamp instead of
    // letting the cast wrap (a >= 4GiB BAR 0 would truncate to 0 and trip the
    // ASSERT(size) in frameSegment.Init).
    ULONGLONG fb_bar_size = m_PciResources.GetPciBar(0)->GetSize();
    UINT fb_size = (fb_bar_size > MAXULONG) ? MAXULONG : (UINT)fb_bar_size;
    /*if (fb_pa.QuadPart == 0 && fb_size == 0) {
        DbgPrint(TRACE_LEVEL_WARNING, ("%s bar 0 is empty, trying 2\n", __FUNCTION__));
        fb_pa = m_PciResources.GetPciBar(2)->GetPA();
        fb_size = m_PciResources.GetPciBar(2)->GetSize();
    }*/

    DbgPrint(TRACE_LEVEL_INFORMATION, ("%s framebuffer %p +0x%x\n", __FUNCTION__, fb_pa.QuadPart, fb_size));

    // FIXME
#if NTDDI_VERSION > NTDDI_WINBLUE
    UINT req_size = 0x1000000;
#else
    UINT req_size = 0x800000;
#endif

    if (!IsUsePhysicalMemory() || fb_pa.QuadPart == 0 || fb_size < req_size)
    {
        fb_pa.QuadPart = 0LL;
        fb_size = max(req_size, fb_size);
    }

    if (!frameSegment.Init(fb_size, &fb_pa))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to allocate FB memory segment\n", __FUNCTION__));
        status = STATUS_INSUFFICIENT_RESOURCES;
        VioGpuDbgBreak();
        return status;
    }

    return status;
}

NTSTATUS VioGpuAdapter::HWClose(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    SetHardwareInit(FALSE);

    LARGE_INTEGER timeout = {0};
    timeout.QuadPart = Int32x32To64(1000, -10000);

    m_bStopWorkThread = TRUE;
    KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);

    // The worker exists only from a successful HWInit onwards: a StartDevice
    // that failed earlier (CheckHardware, PciResources.Init, VioGpuAdapterInit)
    // still reaches here through ~VioGpuAdapter on the PnP remove that follows,
    // and KeWaitForSingleObject(NULL) bugchecks.
    if (m_pWorkThread != NULL)
    {
        // A wedged host can keep the worker inside a ring wait past the
        // timeout.  Dereferencing and freeing the adapter out from under a
        // live thread is worse than leaking the reference, so on a persistent
        // timeout we deliberately keep both the ETHREAD reference and the
        // frame segment alive.
        BOOLEAN exited = FALSE;

        for (UINT i = 0; i < HW_CLOSE_THREAD_WAIT_RETRIES; i++)
        {
            if (KeWaitForSingleObject(m_pWorkThread, Executive, KernelMode, FALSE, &timeout) != STATUS_TIMEOUT)
            {
                exited = TRUE;
                break;
            }
            KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        }

        if (!exited)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> Failed to exit the worker thread; leaking it to avoid a UAF\n"));
            VioGpuDbgBreak();
            DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s\n", __FUNCTION__));
            return STATUS_SUCCESS;
        }

        ObDereferenceObject(m_pWorkThread);
        m_pWorkThread = NULL;
    }

    frameSegment.Close();

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

BOOLEAN FindUpdateRect(_In_ ULONG NumMoves,
                       _In_ D3DKMT_MOVE_RECT *pMoves,
                       _In_ ULONG NumDirtyRects,
                       _In_ PRECT pDirtyRect,
                       _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                       _Out_ PRECT pUpdateRect)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Rotation);
    BOOLEAN updated = FALSE;

    if (pUpdateRect == NULL)
    {
        return FALSE;
    }

    if (NumMoves == 0 && NumDirtyRects == 0)
    {
        pUpdateRect->bottom = 0;
        pUpdateRect->left = 0;
        pUpdateRect->right = 0;
        pUpdateRect->top = 0;
    }

    for (ULONG i = 0; i < NumMoves; i++)
    {
        PRECT pRect = &pMoves[i].DestRect;
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    for (ULONG i = 0; i < NumDirtyRects; i++)
    {
        PRECT pRect = &pDirtyRect[i];
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    if (Rotation == D3DKMDT_VPPR_ROTATE90 || Rotation == D3DKMDT_VPPR_ROTATE270)
    {
    }
    return updated;
}

NTSTATUS VioGpuAdapter::UpdateChildStatus(BOOLEAN connect)
{
    PAGED_CODE();
    NTSTATUS Status(STATUS_SUCCESS);
    DXGK_CHILD_STATUS ChildStatus;
    PDXGKRNL_INTERFACE pDXGKInterface(GetDxgkInterface());

    // Dedupe against the cached state: DXGK only needs to see actual
    // transitions, and the cache also gates the hotplug-disconnect
    // direction.
    if (!!m_scanoutConnected[0] == !!connect)
    {
        return STATUS_SUCCESS;
    }
    m_scanoutConnected[0] = connect ? TRUE : FALSE;

    RtlZeroMemory(&ChildStatus, sizeof(ChildStatus));

    ChildStatus.Type = StatusConnection;
    ChildStatus.ChildUid = 0;
    ChildStatus.HotPlug.Connected = connect;
    Status = pDXGKInterface->DxgkCbIndicateChildStatus(pDXGKInterface->DeviceHandle, &ChildStatus);
    if (Status != STATUS_SUCCESS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--- %s DxgkCbIndicateChildStatus failed with status %x\n ", __FUNCTION__, Status));
    }
    return Status;
}

PAGED_CODE_SEG_END

BOOLEAN VioGpuAdapter::InterruptRoutine(_In_ ULONG MessageNumber)
{
    if (!IsHardwareInit())
    {
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s MessageNumber = %d\n", __FUNCTION__, MessageNumber));
    BOOLEAN serviced = TRUE;
    ULONG intReason = 0;
    // return FALSE;
    if (m_PciResources.IsMSIEnabled())
    {
        switch (MessageNumber)
        {
            case 0:
                intReason = ISR_REASON_CHANGE;
                break;
            case 1:
                intReason = ISR_REASON_DISPLAY;
                break;
            case 2:
                intReason = ISR_REASON_CURSOR;
                break;
            default:
                serviced = FALSE;
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("---> %s Unknown Interrupt Reason MessageNumber%d\n", __FUNCTION__, MessageNumber));
        }
    }
    else
    {
        UNREFERENCED_PARAMETER(MessageNumber);
        UCHAR isrstat = virtio_read_isr_status(&m_VioDev);

        // Per virtio 1.x: bit 0 = queue notification, bit 1 = config change.
        // Either or both may be set; missing the bitmask decode silently
        // dropped queue completions when config-change rode the same INTx.
        if (isrstat & 0x01)
        {
            intReason |= (ISR_REASON_DISPLAY | ISR_REASON_CURSOR);
        }
        if (isrstat & VIRTIO_PCI_ISR_CONFIG)
        {
            intReason |= ISR_REASON_CHANGE;
        }
        if (intReason == 0)
        {
            serviced = FALSE;
        }
    }

    if (serviced)
    {
        InterlockedOr((PLONG)&m_PendingWorks, intReason);
        m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return serviced;
}

void VioGpuAdapter::ThreadWork(_In_ PVOID Context)
{
    VioGpuAdapter *pdev = reinterpret_cast<VioGpuAdapter *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpuAdapter::ThreadWorkRoutine(void)
{
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;)
    {
        KeWaitForSingleObject(&m_ConfigUpdateEvent, Executive, KernelMode, FALSE, NULL);

        if (m_bStopWorkThread)
        {
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }

        ConfigChanged();
        NotifyResolutionEvent();
    }
}

void VioGpuAdapter::ConfigChanged(void)
{
    DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
    UINT32 events_read, events_clear = 0;
    virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_read), &events_read, sizeof(events_read));
    if (events_read & VIRTIO_GPU_EVENT_DISPLAY)
    {
        vidpn.GetDisplayInfo();
        events_clear |= VIRTIO_GPU_EVENT_DISPLAY;
        virtio_set_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_clear), &events_clear, sizeof(events_clear));

        // Probe per-scanout enable state and emit child-status
        // transitions in both directions. With MAX_CHILDREN==1 this
        // only walks scanout 0, but the pattern survives a future
        // multi-monitor refactor.
        PGPU_VBUFFER vbuf = NULL;
        if (ctrlQueue.AskDisplayInfo(&vbuf))
        {
            for (UINT i = 0; i < MAX_CHILDREN && i < m_u32NumScanouts; i++)
            {
                ULONG xres = 0, yres = 0;
                BOOLEAN connected = ctrlQueue.GetDisplayInfo(vbuf, i, &xres, &yres);
                UpdateChildStatus(connected);
            }
            ctrlQueue.ReleaseBuffer(vbuf);
        }
        else
        {
            // Fall back to the previous always-connect behaviour if
            // the host did not give us info.
            UpdateChildStatus(TRUE);
        }
    }
}

VOID VioGpuAdapter::PendingCreatePush(VioGpuAllocation *allocation)
{
    KIRQL irql;
    allocation->m_PendingCreateThread = KeGetCurrentThread();
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    InsertTailList(&m_PendingCreateList, &allocation->m_PendingCreateEntry);
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
}

VioGpuAllocation *VioGpuAdapter::PendingCreatePop(VOID)
{
    // FIFO per thread: the oldest un-opened allocation created on the
    // calling thread is the one this in-create open refers to.
    PKTHREAD self = KeGetCurrentThread();
    VioGpuAllocation *found = NULL;
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    for (LIST_ENTRY *e = m_PendingCreateList.Flink; e != &m_PendingCreateList; e = e->Flink)
    {
        VioGpuAllocation *a = CONTAINING_RECORD(e, VioGpuAllocation, m_PendingCreateEntry);
        if (a->m_PendingCreateThread == self)
        {
            RemoveEntryList(e);
            a->m_PendingCreateThread = NULL;
            found = a;
            break;
        }
    }
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    return found;
}

VOID VioGpuAdapter::PendingCreateRemove(VioGpuAllocation *allocation)
{
    // Walk-and-match (no flag): tolerates allocations destroyed without
    // ever being opened (aborted creates).
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    for (LIST_ENTRY *e = m_PendingCreateList.Flink; e != &m_PendingCreateList; e = e->Flink)
    {
        if (e == &allocation->m_PendingCreateEntry)
        {
            RemoveEntryList(e);
            break;
        }
    }
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
}

// The three allocation maps share one lock and one node type; only the key
// and the list head differ.

static VioGpuAdapter::ALLOC_MAP_ENTRY *AllocMapFind(LIST_ENTRY *head, ULONGLONG key)
{
    for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink)
    {
        VioGpuAdapter::ALLOC_MAP_ENTRY *n = CONTAINING_RECORD(e, VioGpuAdapter::ALLOC_MAP_ENTRY, entry);
        if (n->key == key)
        {
            return n;
        }
    }
    return NULL;
}

// Move every node matching the predicate onto `reap` for the caller to free
// outside the lock.
static VOID AllocMapUnlink(LIST_ENTRY *head, LIST_ENTRY *reap, ULONGLONG key, BOOLEAN byKey,
                           VioGpuAllocation *allocation)
{
    for (LIST_ENTRY *e = head->Flink; e != head;)
    {
        LIST_ENTRY *next = e->Flink;
        VioGpuAdapter::ALLOC_MAP_ENTRY *n = CONTAINING_RECORD(e, VioGpuAdapter::ALLOC_MAP_ENTRY, entry);
        if ((byKey ? n->key == key : TRUE) && (allocation == NULL || n->allocation == allocation))
        {
            RemoveEntryList(e);
            InsertTailList(reap, e);
        }
        e = next;
    }
}

static VOID AllocMapReap(LIST_ENTRY *reap)
{
    while (!IsListEmpty(reap))
    {
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(reap), VioGpuAdapter::ALLOC_MAP_ENTRY, entry), VIOGPUTAG);
    }
}

VOID VioGpuAdapter::KmtMapInsert(D3DKMT_HANDLE handle, VioGpuAllocation *allocation)
{
    ALLOC_MAP_ENTRY *node = (ALLOC_MAP_ENTRY *)ExAllocatePoolZero(NonPagedPoolNx, sizeof(*node), VIOGPUTAG);
    if (node == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s OOM for handle=%x\n", __FUNCTION__, handle));
        return;
    }
    node->key = handle;
    node->allocation = allocation;
    LIST_ENTRY reap;
    InitializeListHead(&reap);
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    // dxgkrnl reuses handle values as soon as they are destroyed, while our
    // entry for the old allocation can outlive it (deferred teardown,
    // create/destroy retry storms).  Any existing entry with this value is
    // therefore stale: left in place, lookups resolve the dead binding and
    // handle-keyed removal deletes the live one instead.
    AllocMapUnlink(&m_KmtMapList, &reap, handle, TRUE, NULL);
    InsertTailList(&m_KmtMapList, &node->entry);
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    AllocMapReap(&reap);
}

VOID VioGpuAdapter::KmtMapRemove(D3DKMT_HANDLE handle, VioGpuAllocation *allocation)
{
    // Match on both fields: a bare handle match can hit a newer allocation's
    // binding after dxgkrnl reissued the handle value.
    LIST_ENTRY reap;
    InitializeListHead(&reap);
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    AllocMapUnlink(&m_KmtMapList, &reap, handle, TRUE, allocation);
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    AllocMapReap(&reap);
}

VOID VioGpuAdapter::KmtMapRemoveByAllocation(VioGpuAllocation *allocation)
{
    LIST_ENTRY reap;
    InitializeListHead(&reap);
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    AllocMapUnlink(&m_KmtMapList, &reap, 0, FALSE, allocation);
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    AllocMapReap(&reap);
}

VioGpuAllocation *VioGpuAdapter::KmtMapLookup(D3DKMT_HANDLE handle)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    ALLOC_MAP_ENTRY *n = AllocMapFind(&m_KmtMapList, handle);
    VioGpuAllocation *found = n ? n->allocation : NULL;
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    return found;
}

VOID VioGpuAdapter::CookieMapInsert(ULONGLONG cookie, VioGpuAllocation *allocation)
{
    ALLOC_MAP_ENTRY *node = (ALLOC_MAP_ENTRY *)ExAllocatePoolZero(NonPagedPoolNx, sizeof(*node), VIOGPUTAG);
    if (node == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s OOM for cookie=%llx\n", __FUNCTION__, cookie));
        return;
    }
    node->key = cookie;
    node->allocation = allocation;
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    InsertTailList(&m_CookieMapList, &node->entry);
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
}

VOID VioGpuAdapter::CookieMapRemoveByAllocation(VioGpuAllocation *allocation)
{
    LIST_ENTRY reap;
    InitializeListHead(&reap);
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    AllocMapUnlink(&m_CookieMapList, &reap, 0, FALSE, allocation);
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    AllocMapReap(&reap);
}

VioGpuAllocation *VioGpuAdapter::CookieMapLookup(ULONGLONG cookie)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    ALLOC_MAP_ENTRY *n = AllocMapFind(&m_CookieMapList, cookie);
    VioGpuAllocation *found = n ? n->allocation : NULL;
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    return found;
}

ULONGLONG VioGpuAdapter::ShmemAlloc(SIZE_T size)
{
    ULONG pages = (ULONG)((size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (pages == 0)
    {
        pages = 1;
    }
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    if (m_ShmemBitmapBuffer == NULL)
    {
        if (!m_VioDev.shmem.available)
        {
            KeReleaseSpinLock(&m_PendingCreateLock, irql);
            return (ULONGLONG)-1;
        }
        ULONG pageCount = (ULONG)(m_VioDev.shmem.length >> PAGE_SHIFT);
        ULONG bytes = ((pageCount + 31) / 32) * sizeof(ULONG);
        m_ShmemBitmapBuffer = (PULONG)ExAllocatePoolZero(NonPagedPoolNx, bytes, VIOGPUTAG);
        if (m_ShmemBitmapBuffer == NULL)
        {
            KeReleaseSpinLock(&m_PendingCreateLock, irql);
            return (ULONGLONG)-1;
        }
        m_ShmemPageCount = pageCount;
        RtlInitializeBitMap(&m_ShmemBitmap, m_ShmemBitmapBuffer, pageCount);
        // Keep the low window clear of KMD grants: VidMm placements and host
        // bookkeeping both start at 0, so handing out offsets from halfway up
        // the window keeps the two allocators apart.
        m_ShmemSearchBase = pageCount / 2;
    }
    // First fit from a FIXED base, never from a rolling bump pointer.  The
    // bitmap recycles freed space either way, but a hint that only advances
    // hands every allocation a BAR window this boot has never mapped, and the
    // cost of the user-mode mapping built over that window in
    // EscapeResourceInfo (MmMapLockedPagesSpecifyCache, and the matching
    // MmUnmapLockedPages at destroy) grows with the number of DISTINCT BAR
    // ranges ever mapped -- enough to degrade heap create/destroy ~12x after a
    // few hundred of them, for the rest of the boot.  First fit keeps a
    // churning workload inside one small window; the search stays under 1 us.
    ULONG index = RtlFindClearBitsAndSet(&m_ShmemBitmap, pages, m_ShmemSearchBase);
    if (index == 0xFFFFFFFF)
    {
        index = RtlFindClearBitsAndSet(&m_ShmemBitmap, pages, 0);
    }
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    if (index == 0xFFFFFFFF)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s no space for %zu bytes\n", __FUNCTION__, size));
        return (ULONGLONG)-1;
    }
    return (ULONGLONG)index << PAGE_SHIFT;
}

VOID VioGpuAdapter::ShmemFree(ULONGLONG offset, SIZE_T size)
{
    ULONG pages = (ULONG)((size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (pages == 0)
    {
        pages = 1;
    }
    ULONG index = (ULONG)(offset >> PAGE_SHIFT);
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    if (m_ShmemBitmapBuffer != NULL && index + pages <= m_ShmemPageCount)
    {
        RtlClearBits(&m_ShmemBitmap, index, pages);
    }
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
}

VioGpuAllocation *VioGpuAdapter::AllocationByResId(UINT resId)
{
    // Resolve through the open-time bindings (every UMD-visible blob is
    // opened on a device before it is mapped).
    VioGpuAllocation *found = NULL;
    KIRQL irql;
    KeAcquireSpinLock(&m_PendingCreateLock, &irql);
    for (LIST_ENTRY *e = m_KmtMapList.Flink; e != &m_KmtMapList; e = e->Flink)
    {
        ALLOC_MAP_ENTRY *n = CONTAINING_RECORD(e, ALLOC_MAP_ENTRY, entry);
        if (n->allocation != NULL && n->allocation->GetId() == resId)
        {
            found = n->allocation;
            break;
        }
    }
    KeReleaseSpinLock(&m_PendingCreateLock, irql);
    return found;
}

VioGpuAllocation *VioGpuAdapter::AllocationFromHandle(D3DKMT_HANDLE handle)
{
    DXGKARGCB_GETHANDLEDATA getHandleData;
    getHandleData.hObject = handle;
    getHandleData.Type = DXGK_HANDLE_ALLOCATION;
    getHandleData.Flags.DeviceSpecific = 0;
    VOID *raw = m_DxgkInterface.DxgkCbGetHandleData(&getHandleData);
    VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(raw);
    if (allocation == NULL)
    {
        // WDDM2: GetHandleData returns NULL for allocation handles (1.3
        // resolves the identical flow).  Fall back to the bindings learned
        // at DxgkDdiOpenAllocation.
        allocation = KmtMapLookup(handle);
        if (allocation == NULL)
        {
            // Routine under WDDM2: VidMm-internal allocations (DMA pool
            // buffers) never pass through CreateAllocation, so they have
            // no driver object and no KMT-map entry.  Callers that
            // actually need one log at ERROR themselves.
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("%s handle=%x unresolvable (GetHandleData raw=%p, no KMT-map entry)\n",
                      __FUNCTION__, handle, raw));
        }
    }
    return allocation;
}

VioGpuResource *VioGpuAdapter::ResourceFromHandle(D3DKMT_HANDLE handle)
{
    DXGKARGCB_GETHANDLEDATA getHandleData;
    getHandleData.hObject = handle;
    getHandleData.Type = DXGK_HANDLE_RESOURCE;
    getHandleData.Flags.DeviceSpecific = 0;
    return VioGpuResource::FromHandle(m_DxgkInterface.DxgkCbGetHandleData(&getHandleData));
}

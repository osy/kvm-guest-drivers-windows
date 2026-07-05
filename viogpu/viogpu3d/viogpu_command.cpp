#include "viogpu_command.h"
#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"

#pragma code_seg(push)
#pragma code_seg()

VioGpuCommand::VioGpuCommand(VioGpuAdapter *adapter)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    m_pAdapter = adapter;
    m_pCommander = &adapter->commander;
    m_pDevice = NULL;

    m_FenceId = 0;
    m_NodeOrdinal = 0;
    m_EngineOrdinal = 0;
    m_NullRendering = FALSE;
    m_pendingCallbacks = 0;
    m_pDmaBuffer = NULL;
    m_pCommand = NULL;
    m_pEnd = NULL;

    m_allocations = NULL;
    m_allocationsLength = 0;

    list_entry.Blink = NULL;
    list_entry.Flink = NULL;
};

VioGpuCommand::~VioGpuCommand()
{
    // Tripping this means a cmd was freed while a queue completion
    // callback was still going to dereference `this`. In the current
    // code the only delete path is Run() -> `end:`, reached only when
    // the body is fully drained and the last submit's callback has
    // already fired and re-queued the cmd onto the running list --
    // so the count must be zero. A future caller that frees the cmd
    // from a different path (an error tearing down a partially-
    // submitted command) would need to wait for outstanding callbacks
    // first.
    LONG pending = m_pendingCallbacks;
    if (pending != 0)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s cmd=%p destroyed with %d outstanding callbacks\n",
                  __FUNCTION__, this, pending));
        ASSERT(pending == 0);
    }
}

void VioGpuCommand::AddPending()
{
    InterlockedIncrement(&m_pendingCallbacks);
}

LONG VioGpuCommand::DropPending()
{
    LONG remaining = InterlockedDecrement(&m_pendingCallbacks);
    if (remaining < 0)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s cmd=%p pending underflow %d\n",
                  __FUNCTION__, this, remaining));
        InterlockedExchange(&m_pendingCallbacks, 0);
        return 0;
    }
    return remaining;
}

void VioGpuCommand::PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_FenceId = pSubmitCommand->SubmissionFenceId;
    m_NodeOrdinal = pSubmitCommand->NodeOrdinal;
    m_EngineOrdinal = pSubmitCommand->EngineOrdinal;
    if (m_pDmaBuffer)
    {
        m_pCommand = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionStartOffset;
        m_pEnd = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionEndOffset;
    }
    m_pDevice = VioGpuDevice::FromHandle(pSubmitCommand->hContext);

    // Capture the only submit flag we react to. Paging / ContextSwitch /
    // Flip can legitimately arrive with an empty DMA range; Run() falls
    // through to the fence-completion arm in that case, so they need
    // no special handling. NullRendering does need to short-circuit so
    // the runtime's submission-overhead profiling does not actually
    // execute the body.
    m_NullRendering = pSubmitCommand->Flags.NullRendering ? TRUE : FALSE;
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

void VioGpuCommand::Run()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    if (m_NullRendering)
    {
        // The runtime asked us to simulate insertion of the DMA buffer
        // without executing its body. Skip straight to the fence
        // completion at `end:` so the submission is timed without the
        // host running anything.
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("<---> %s fence_id=%d NullRendering: skipping body\n",
                  __FUNCTION__, m_FenceId));
        goto end;
    }

    while (m_pCommand < m_pEnd)
    {

        VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)m_pCommand;
        m_pCommand += sizeof(VIOGPU_COMMAND_HDR);

        void *cmdBody = m_pCommand;
        m_pCommand += cmdHdr->size;

        DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s fence_id=%d running command=%d\n", __FUNCTION__, m_FenceId, cmdHdr->type));

        switch (cmdHdr->type)
        {
            case VIOGPU_CMD_SUBMIT:
                {
                    PBYTE submitCmd = new (NonPagedPoolNx) BYTE[cmdHdr->size];
                    if (!submitCmd)
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d OOM allocating submit buffer (size=%u); skipping command\n",
                                  __FUNCTION__,
                                  m_FenceId,
                                  cmdHdr->size));
                        goto end;
                    }
                    RtlCopyMemory(submitCmd, cmdBody, cmdHdr->size);

                    AddPending();
                    m_pAdapter->ctrlQueue.SubmitCommand(submitCmd,
                                                        cmdHdr->size,
                                                        (cmdHdr->flags & VIOGPU_EXECBUF_VIRGL) != 0 ? m_pDevice->m_Virgl.GetId() : m_pDevice->m_Context.GetId(),
                                                        (cmdHdr->flags & VIOGPU_EXECBUF_RING_IDX) != 0,
                                                        cmdHdr->ring_idx,
                                                        VioGpuCommand::QueueRunningCb,
                                                        this);
                    return;
                }

            case VIOGPU_CMD_SUBMIT_ON_CTX:
                {
                    // Submit to an explicit virtio context: the payload is a
                    // VIOGPU_SUBMIT_ON_CTX_HDR naming the target context (the
                    // transport context that owns a swapchain the presenting
                    // device flips) followed by the EXECBUF bytes.
                    if (cmdHdr->size <= sizeof(VIOGPU_SUBMIT_ON_CTX_HDR))
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d SUBMIT_ON_CTX payload too small (size=%u); skipping\n",
                                  __FUNCTION__, m_FenceId, cmdHdr->size));
                        goto end;
                    }
                    VIOGPU_SUBMIT_ON_CTX_HDR *ctxHdr = (VIOGPU_SUBMIT_ON_CTX_HDR *)cmdBody;
                    const ULONG payloadSize = cmdHdr->size - sizeof(VIOGPU_SUBMIT_ON_CTX_HDR);

                    PBYTE submitCmd = new (NonPagedPoolNx) BYTE[payloadSize];
                    if (!submitCmd)
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d OOM allocating submit buffer (size=%u); skipping command\n",
                                  __FUNCTION__, m_FenceId, payloadSize));
                        goto end;
                    }
                    RtlCopyMemory(submitCmd, (PBYTE)cmdBody + sizeof(VIOGPU_SUBMIT_ON_CTX_HDR),
                                  payloadSize);

                    // Rate-gated (see the flip-log comment in viogpu_device.cpp:
                    // unthrottled per-DMA serial logging paces the pipeline).
                    static LONG s_submitOnCtxLogCount = 0;
                    const LONG socLogN = InterlockedIncrement(&s_submitOnCtxLogCount);
                    if (socLogN <= 8 || (socLogN & 63) == 0)
                    {
                        DbgPrint(TRACE_LEVEL_INFORMATION,
                                 ("submit_on_ctx n=%d fence=%d ctx=%u ring=%u\n",
                                  socLogN, m_FenceId, ctxHdr->ctx_id, cmdHdr->ring_idx));
                    }

                    AddPending();
                    m_pAdapter->ctrlQueue.SubmitCommand(submitCmd,
                                                        payloadSize,
                                                        ctxHdr->ctx_id,
                                                        (cmdHdr->flags & VIOGPU_EXECBUF_RING_IDX) != 0,
                                                        cmdHdr->ring_idx,
                                                        VioGpuCommand::QueueRunningCb,
                                                        this);
                    return;
                }

            case VIOGPU_CMD_TRANSFER_TO_HOST:
            case VIOGPU_CMD_TRANSFER_FROM_HOST:
                {
                    VIOGPU_TRANSFER_CMD *transferCmd = (VIOGPU_TRANSFER_CMD *)cmdBody;

                    AddPending();
                    m_pAdapter->ctrlQueue.TransferHostCmd(cmdHdr->type == VIOGPU_CMD_TRANSFER_TO_HOST,
                                                          (cmdHdr->flags & VIOGPU_EXECBUF_VIRGL) != 0 ? m_pDevice->m_Virgl.GetId() : m_pDevice->m_Context.GetId(),
                                                          false,
                                                          0,
                                                          transferCmd,
                                                          VioGpuCommand::QueueRunningCb,
                                                          this);
                    return;
                }

            case VIOGPU_CMD_UNMAP_BLOB_BY_ID:
                {
                    // DISCARD_CONTENT paging op: tear down the host mapping
                    // of every listed res_id.  VidMm frees the segment range
                    // when a dead process's blob is discarded, and a stale
                    // host mapping there poisons the next blob mapped into
                    // the reused range (dead transport rings, 2026-07-04).
                    const UINT *res_ids = (const UINT *)cmdBody;
                    const size_t count = cmdHdr->size / sizeof(UINT);

                    AddPending();
                    for (size_t i = 0; i < count; i++)
                    {
                        if (!res_ids[i])
                        {
                            continue;
                        }
                        AddPending();
                        static LONG s_discardLogCount = 0;
                        const LONG dlogN = InterlockedIncrement(&s_discardLogCount);
                        if (dlogN <= 16 || (dlogN & 63) == 0)
                        {
                            DbgPrint(TRACE_LEVEL_WARNING,
                                     ("<---> %s fence_id=%d DISCARD-unmap blob res_id=%d n=%d\n",
                                      __FUNCTION__, m_FenceId, res_ids[i], dlogN));
                        }
                        m_pAdapter->ctrlQueue.ResourceUnmapBlob(res_ids[i], 0,
                                                                VioGpuCommand::QueueRunningCb,
                                                                this);
                    }
                    if (DropPending() == 0)
                    {
                        break;
                    }
                    return;
                }

            case VIOGPU_CMD_MAP_BLOB:
            case VIOGPU_CMD_UNMAP_BLOB:
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s fence_id=%d running map/unmap blob, next=%d, curr=%p, end=%p\n", __FUNCTION__, m_FenceId, ((VIOGPU_COMMAND_HDR *)m_pCommand)->type, m_pCommand, m_pEnd));

                    const BOOLEAN isMap = (cmdHdr->type == VIOGPU_CMD_MAP_BLOB);
                    ULONG *map_idx = (ULONG *)cmdBody;

                    size_t num_maps = cmdHdr->size / sizeof(ULONG);

                    // Validate every index -- bounds, non-NULL, blob, and
                    // mappability -- before issuing anything. Doing the
                    // mappability check here (rather than in the issue loop)
                    // keeps the issue loop free of any early exit that could
                    // leave host ops in flight while we jump to `end:`.
                    BOOLEAN valid = TRUE;
                    for (size_t i = 0; i < num_maps; i++) {
                        if (map_idx[i] >= m_allocationsLength)
                        {
                            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence_id=%d map/unmap blob %d: invalid index=%u\n", __FUNCTION__, m_FenceId, i, map_idx[i]));
                            valid = FALSE; break;
                        }
                        VioGpuAllocation *a = m_allocations[map_idx[i]];
                        if (a == NULL)
                        {
                            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence_id=%d map/unmap blob %d: allocation %d is NULL\n", __FUNCTION__, m_FenceId, i, map_idx[i]));
                            valid = FALSE; break;
                        }
                        if (!a->IsBlob())
                        {
                            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence_id=%d map/unmap blob %d: allocation %d is not blob\n", __FUNCTION__, m_FenceId, i, map_idx[i]));
                            valid = FALSE; break;
                        }
                        if (!a->IsMappable())
                        {
                            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence_id=%d res_id=%d cannot map unmappable blob (flags=%d)\n", __FUNCTION__, m_FenceId, a->GetId(), a->m_Blob.Options.blob_flags));
                            valid = FALSE; break;
                        }
                    }
                    if (!valid)
                        goto end;

                    // Issue each map/unmap that actually changes state, every
                    // one carrying QueueRunningCb. A loop guard holds one
                    // pending ref so a host response arriving mid-issue cannot
                    // re-queue this command before the whole batch is posted.
                    // Whoever drops the count to zero -- the guard release below
                    // when nothing is in flight, otherwise the final host
                    // completion -- re-enters Run() exactly once to reach `end:`.
                    // The guard guarantees the DMA fence is always completed,
                    // including the case where every blob is already in the
                    // requested state and no host command is issued at all.
                    AddPending();
                    for (size_t i = 0; i < num_maps; i++) {
                        VioGpuAllocation *allocation = m_allocations[map_idx[i]];
                        UINT ctxId = m_pDevice->m_Context.GetId();
                        AddPending();
                        BOOLEAN issued = isMap
                            ? allocation->MapBlob(ctxId, VioGpuCommand::QueueRunningCb, this)
                            : allocation->UnmapBlob(ctxId, VioGpuCommand::QueueRunningCb, this);
                        DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s fence_id=%d %s blob res_id=%d issued=%d\n", __FUNCTION__, m_FenceId, isMap ? "map" : "unmap", allocation->GetId(), issued));
                        if (!issued)
                        {
                            // Already in the requested state: no host round-trip
                            // and therefore no callback -- undo the tentative ref.
                            DropPending();
                        }
                    }
                    if (DropPending() == 0)
                    {
                        // Nothing was actually issued to the host (all blobs
                        // already in the requested state). No callback will
                        // re-queue us, so fall through to complete the fence.
                        break;
                    }
                    // At least one host map/unmap is in flight; the completion
                    // that drops the count to zero re-queues us to reach `end:`.
                    return;
                }

            case VIOGPU_CMD_NOP:
            default:
                {
                    // DO NOTHING
                    break;
                }
        }
    }

end:
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s finished fence_id=%d, this=%p, m_pAdapter=%p\n", __FUNCTION__, m_FenceId, this, m_pAdapter));

    if (m_allocations)
    {
        for (UINT i = 0; i < m_allocationsLength; i++)
        {
            if (m_allocations[i])
            {
                m_allocations[i]->UnmarkBusy();
            }
        }
        delete m_allocations;
    }

    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    interrupt.DmaCompleted.SubmissionFenceId = m_FenceId;
    interrupt.DmaCompleted.NodeOrdinal = m_NodeOrdinal;
    interrupt.DmaCompleted.EngineOrdinal = m_EngineOrdinal;
    m_pAdapter->NotifyInterrupt(&interrupt, true);

    m_pCommander->CommandFinished();

    // The commander runs one command at a time (VIOGPU_MAX_RUNNING == 1) and
    // dxgkrnl serializes SubmitCommand, so completions arrive in submission
    // order and the reported fence only ever advances.
    InterlockedExchange(&m_pAdapter->m_LastCompletedFenceId, m_FenceId);

    delete this;
}

NTSTATUS VioGpuCommand::AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    m_allocations = new (NonPagedPoolNx) VioGpuAllocation *[allocationListLength];
    if (!m_allocations)
    {
        m_allocationsLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_allocationsLength = allocationListLength;
    for (UINT i = 0; i < allocationListLength; i++)
    {
        VioGpuDeviceAllocation *deviceAllocation = VioGpuDeviceAllocation::FromHandle(allocationList[i].hDeviceSpecificAllocation);
        if (deviceAllocation)
        {
            m_allocations[i] = deviceAllocation->GetAllocation();
            m_allocations[i]->MarkBusy();
        }
        else
        {
            m_allocations[i] = NULL;
        }
    }
    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()

void VioGpuCommand::QueueRunning()
{
    m_pCommander->QueueRunning(this);
}

void VioGpuCommand::QueueRunningCb(void *cmd, void *, void *)
{
    VioGpuCommand *self = (VioGpuCommand *)cmd;
    // Pair with the AddPending() that ran before the matching submit.
    // Re-queue only when this was the LAST outstanding submission: a
    // single DMA body can issue several async ops (e.g. a multi-index
    // MAP_BLOB), and Run() must re-enter exactly once -- when they have
    // all completed -- to advance past the command and reach `end:`.
    // Dropping to zero is the unique edge that re-queues; an earlier
    // completion just decrements. Drop before queue so the dtor's
    // zero-pending assertion can't observe a transient count.
    if (self->DropPending() == 0)
        self->QueueRunning();
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

VioGpuCommander::VioGpuCommander(VioGpuAdapter *pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    m_pAdapter = pAdapter;
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;

    KeInitializeEvent(&m_QueueEvent, SynchronizationEvent, FALSE);

    InitializeListHead(&m_SubmittedQueue);
    InitializeListHead(&m_RunningQueue);
    KeInitializeSpinLock(&m_Lock);

    m_running = 0;
}

NTSTATUS VioGpuCommander::Start()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    HANDLE threadHandle = 0;

    NTSTATUS status = PsCreateSystemThread(&threadHandle,
                                           (ACCESS_MASK)0,
                                           NULL,
                                           (HANDLE)0,
                                           NULL,
                                           VioGpuCommander::ThreadWork,
                                           this);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to create command worker thread, status %x\n", __FUNCTION__, status));
        VioGpuDbgBreak();
        return status;
    }

    ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)(&m_pWorkThread), NULL);

    ZwClose(threadHandle);

    return status;
}

void VioGpuCommander::Stop()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    LARGE_INTEGER timeout = {0};
    timeout.QuadPart = Int32x32To64(1000, -10000);

    m_bStopWorkThread = TRUE;
    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);

    if (KeWaitForSingleObject(m_pWorkThread, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("---> Failed to exit the worker thread\n"));
        VioGpuDbgBreak();
    }
}

void VioGpuCommander::ThreadWork(PVOID Context)
{
    PAGED_CODE();

    VioGpuCommander *pdev = reinterpret_cast<VioGpuCommander *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpuCommander::ThreadWorkRoutine(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;)
    {
        KeWaitForSingleObject(&m_QueueEvent, Executive, KernelMode, FALSE, NULL);

        if (m_bStopWorkThread)
        {
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }

        while (m_running < VIOGPU_MAX_RUNNING)
        {
            VioGpuCommand *command = DequeueSubmitted();
            if (command == NULL)
            {
                break;
            }
            QueueRunning(command);
            m_running++;
        }

        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s Running command\n", __FUNCTION__));
        while (true)
        {
            VioGpuCommand *command = DequeueRunning();
            if (command == NULL)
            {
                break;
            }
            command->Run();
        }
    }
}

void VioGpuCommander::CommandFinished()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_running--;
    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

NTSTATUS VioGpuCommander::Patch(const DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s \n", __FUNCTION__));

    VioGpuDevice *pDevice = VioGpuDevice::FromHandle(pPatch->hContext);

    for (UINT i = 0; i < pPatch->AllocationListSize; i++)
    {
        const DXGK_ALLOCATIONLIST *allocList = &pPatch->pAllocationList[i];
        VioGpuDeviceAllocation *deviceAllocation = VioGpuDeviceAllocation::FromHandle(allocList->hDeviceSpecificAllocation);
        VioGpuAllocation *allocation = deviceAllocation ? deviceAllocation->GetAllocation() : nullptr;
        if (allocation && allocation->IsBlob())
        {

            allocation->m_Blob.MapOffset = allocList->PhysicalAddress.QuadPart - VioGpuAdapter::SHMEM_GPU_BASE_VA;
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s res_id=%d base=%p addr=%p off=%llx\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           pDevice->m_pAdapter->GetShmemPA(),
                                           allocList->PhysicalAddress.QuadPart,
                                           allocation->m_Blob.MapOffset));
        }
    }

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()
NTSTATUS VioGpuCommander::SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    VIOGPU_ASSERT(pSubmitCommand != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s fence_id=%d\n", __FUNCTION__, pSubmitCommand->SubmissionFenceId));

    VioGpuCommand *cmd = NULL;
    if (pSubmitCommand->pDmaBufferPrivateData)
    {
        cmd = VioGpuCommand::FromHandle(*(void **)pSubmitCommand->pDmaBufferPrivateData);
    }

    if (!cmd)
    {
        cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
    }

    cmd->PrepareSubmit(pSubmitCommand);
    QueueSubmitted(cmd);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql,
                                                        Irql) _IRQL_raises_(DISPATCH_LEVEL) void VioGpuCommander::
                                                                                                    LockQueue(KIRQL *Irql)
{
    KIRQL SavedIrql = KeGetCurrentIrql();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, SavedIrql));

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_Lock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_Lock);
    }
    else
    {
        VioGpuDbgBreak();
    }
    *Irql = SavedIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

_IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void VioGpuCommander::UnlockQueue(KIRQL Irql)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, Irql));

    if (Irql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_Lock, Irql);
    }
    else
    {
        KeReleaseSpinLockFromDpcLevel(&m_Lock);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuCommand *VioGpuCommander::DequeueRunning()
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    PLIST_ENTRY result = NULL;
    if (!IsListEmpty(&m_RunningQueue))
    {
        result = RemoveHeadList(&m_RunningQueue);
    }
    UnlockQueue(oldIrql);
    if (!result)
    {
        return NULL;
    }
    return CONTAINING_RECORD(result, VioGpuCommand, list_entry);
}

void VioGpuCommander::QueueRunning(VioGpuCommand *cmd)
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    InsertTailList(&m_RunningQueue, &cmd->list_entry);
    UnlockQueue(oldIrql);

    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

VioGpuCommand *VioGpuCommander::DequeueSubmitted()
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    PLIST_ENTRY result = NULL;
    if (!IsListEmpty(&m_SubmittedQueue))
    {
        result = RemoveHeadList(&m_SubmittedQueue);
    }
    UnlockQueue(oldIrql);
    if (!result)
    {
        return NULL;
    }
    return CONTAINING_RECORD(result, VioGpuCommand, list_entry);
}

void VioGpuCommander::QueueSubmitted(VioGpuCommand *cmd)
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    InsertTailList(&m_SubmittedQueue, &cmd->list_entry);
    UnlockQueue(oldIrql);

    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

#pragma code_seg(pop)

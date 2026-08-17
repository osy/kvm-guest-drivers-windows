#pragma once

#include "handle.h"

#define VIOGPU_MAX_RUNNING 1

class VioGpuAdapter;
class VioGpuDevice;
class VioGpuContext;
class VioGpuAllocation;
class VioGpuCommander;

// DMA private-data layout.  Under GpuMmu the submit DDI only sees a GPU VA
// for the DMA buffer, so the build-phase DDIs (Present/Render) mirror the
// command body they wrote into the packet's private data, which dxgkrnl
// carries to SubmitCommandVirtual by value.
//
// `magic` is the "WE own this private data" flag: SubmitCommandVirtual also
// runs for dxgkrnl-originated paging packets whose private data is not ours
// (stale bytes).  It must NOT interpret those bytes as a VioGpuCommand* --
// HandleBase::FromHandle makes a virtual call through the candidate pointer,
// so a garbage pointer wild-calls through a garbage vtable.  Only
// Present/Render packets stamp magic, so a clear/mismatched magic means the
// packet is not ours and completes fence-only.
//
// `cmd` is first because a paging buffer's private data can be as small as a
// single pointer, and that slot alone is what the non-virtual submit path
// reads.
#define VIOGPU_DMA_PRIV_MAGIC 0x56503244u /* 'VP2D' */
#define VIOGPU_DMA_PRIV_BODY_MAX 1024u
typedef struct _VIOGPU_DMA_PRIVATE
{
    void *cmd; /* must stay first: see above */
    ULONG magic;
    ULONG bodySize; /* 0 = no body (flip); >MAX = oversized (fence-only) */
    UCHAR body[VIOGPU_DMA_PRIV_BODY_MAX];
} VIOGPU_DMA_PRIVATE;

// Claim the packet's private data as ours (Present/Render entry).  Private
// data too small for the mirror still gets its command pointer cleared, so a
// recycled buffer cannot hand a stale pointer to the submit path.
inline void VioGpuDmaPrivClaim(void *priv, ULONG privSize)
{
    if (!priv || privSize < sizeof(void *))
    {
        return;
    }
    *(void **)priv = NULL;
    if (privSize < sizeof(VIOGPU_DMA_PRIVATE))
    {
        return;
    }
    VIOGPU_DMA_PRIVATE *p = (VIOGPU_DMA_PRIVATE *)priv;
    p->magic = VIOGPU_DMA_PRIV_MAGIC;
    p->bodySize = 0;
}

// Lifetime invariant: VioGpuCommand passes `this` as the complete_ctx
// of SubmitCommand / TransferHostCmd / MapBlob / UnmapBlob, which the
// host responds to from a DPC. The object must outlive every such
// in-flight callback.
//
// In the current flow this holds because Run() is the only path that
// frees the object (the `end:` arm) and Run() is only re-entered via
// QueueRunningCb -> QueueRunning -> commander queue -> Run(), so a
// cmd that submitted is never deleted while its callback is in flight.
// AddPending / DropPending track outstanding async submissions and the
// dtor asserts the count is zero -- a future change that frees a cmd
// from another path would trip the assert before the host's DPC could
// dereference freed memory.
class VioGpuCommand final : public HandleBase<"VIOGCOMM"_M, VioGpuCommand>
{
  public:
    VioGpuCommand(VioGpuAdapter *adapter);
    ~VioGpuCommand();

    void Run();
    // Hand a queued, never-started command back to dxgkrnl at a preempt
    // ack: it is re-stamped into the packet's private data (undoing the
    // submit's consume-once) so the resubmission recovers this same object
    // -- monitored-fence write, oversized-body capture, busy allocations --
    // and executes it once, in resubmission order.  Nothing is executed
    // or reported here.  A packet that carried no private data is
    // fence-only and is simply freed.
    void Discard();

    void PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);
    void PrepareSubmitVirtual(const DXGKARG_SUBMITCOMMANDVIRTUAL *pSubmitCommand);

    // Build phase (Render/Present): mirror the just-built body into the packet
    // private data, and for a body too large for the mirror window capture a
    // heap copy on this command while the build-time DMA buffer is still
    // CPU-valid, so the submit phase can still execute it.
    void MirrorBody(void *priv, ULONG privSize, const void *body, SIZE_T size);
    // Submit phase: recover that body.  Build-time DMA buffer VAs are never
    // dereferenced here -- under GpuMmu the mapping behind them is not
    // guaranteed to outlive the build call.
    void RecoverMirroredBody(const void *pPrivateData, ULONG privateDataSize);
    void QueueRunning();
    static void QueueRunningCb(void *cmd, void *, void *);

    // Fire the DXGK DMA_COMPLETED interrupt for this command's fence and
    // advance m_LastCompletedFenceId. Once-guarded (m_notified) so it runs
    // exactly once whether reached from the response DPC (the reliable path:
    // fired inside DpcRoutine, committed by the same pass's DxgkCbNotifyDpc)
    // or the worker-thread Run() epilogue (fallback for commands that finish
    // synchronously with no outstanding async submission). Non-paged /
    // DISPATCH-safe so the DPC path is legal.
    void NotifyCompletion();

    void SetDmaBuf(char *pDmaBuffer)
    {
        m_pDmaBuffer = pDmaBuffer;
    }

    // Deferred monitored-fence signal (DXGK_OPERATION_SIGNAL_MONITORED_FENCE):
    // BuildPagingBuffer resolves the fence GPU VA, pins a kernel mapping of
    // the value cell, and stamps this command into the paging buffer's
    // private data; Run() registers (fenceId, kva, value) on the adapter's
    // defer list and completes normally.  The value is published when the
    // completion watermark reaches this packet's own fence id -- inside the
    // sync routine, before the DMA_COMPLETED interrupt -- so
    // GetCompletedValue pollers can't observe it before the GPU work it
    // covers, and the packet is never parked.
    void SetMFenceWrite(volatile UINT64 *kva, ULONGLONG phys, UINT64 value)
    {
        m_MFenceKva = kva;
        m_MFencePhys = phys;
        m_MFenceValue = value;
    }

    // Render's allocations arrive as DXGK_ALLOCATIONLIST, Present's as the
    // other view of the same union (DXGK_PRESENTALLOCATIONINFO).  The two have
    // different strides, so each needs its own walk; only the element type
    // differs, hence the template.
    template <typename T> NTSTATUS AttachAllocations(T *allocations, UINT allocationListLength);

    LIST_ENTRY list_entry;

  private:
    VioGpuAdapter *m_pAdapter;
    VioGpuCommander *m_pCommander;
    VioGpuDevice *m_pDevice;

    VioGpuAllocation **m_allocations;
    UINT m_allocationsLength;

    UINT m_FenceId;

  public:
    UINT FenceId() const
    {
        return m_FenceId;
    }

  private:
    // The node/engine dxgkrnl submitted this DMA buffer on. The
    // DMA_COMPLETED interrupt must report these back unchanged so the
    // scheduler matches the completion to the right engine; reporting a
    // fixed 0/0 strands submissions on any non-zero engine and the
    // scheduler eventually declares the engine hung (TDR).
    UINT m_NodeOrdinal;
    UINT m_EngineOrdinal;
    // DXGK_SUBMITCOMMANDFLAGS.NullRendering: the runtime is timing
    // the submission path itself (profiling) and wants the fence to
    // complete without executing the DMA body.
    BOOLEAN m_NullRendering;

    // Outstanding async submissions where `this` is the complete_ctx.
    // Tracked so the dtor can assert no callback is still pending.
    volatile LONG m_pendingCallbacks;

    // Once-guard for NotifyCompletion(): the DMA_COMPLETED interrupt must fire
    // exactly once per command even though both the DPC completion path and the
    // Run() epilogue can reach it.
    volatile LONG m_notified;

    void AddPending();
    // Returns the post-decrement count so callers can re-queue the command
    // exactly once -- when the last outstanding async submission completes.
    LONG DropPending();

    char *m_pDmaBuffer;
    char *m_pCommand;
    char *m_pEnd;

    // Heap copy of the body Run() executes, and its length.  Bodies that fit
    // the mirror window are copied out of the packet private data at submit;
    // oversized ones are captured at build time, because the private-data
    // buffer is only valid for the duration of the DDI call and the DMA
    // buffer is a GPU VA by then.
    PUCHAR m_privBodyCopy;
    SIZE_T m_privBodySize;

    // The packet's private data as seen at submit, kept only so Discard()
    // can re-stamp this command into it.
    void *m_pPriv;
    ULONG m_PrivSize;
    BOOLEAN m_PrivVirtual;

    // Deferred monitored-fence write (see SetMFenceWrite).  Kva NULL = none;
    // Phys is the pin bookkeeping handle (MFenceMapUnpin).
    volatile UINT64 *m_MFenceKva;
    ULONGLONG m_MFencePhys;
    UINT64 m_MFenceValue;
};

class VioGpuCommander
{
  public:
    VioGpuCommander(VioGpuAdapter *pAdapter);

    NTSTATUS Start();
    void Stop();

    void CommandFinished();

    NTSTATUS Patch(const DXGKARG_PATCH *pPatch);
    NTSTATUS SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);
    NTSTATUS SubmitCommandVirtual(const DXGKARG_SUBMITCOMMANDVIRTUAL *pSubmitCommand);

    _IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql, Irql) _IRQL_raises_(DISPATCH_LEVEL) void LockQueue(
                                                                                                        KIRQL *Irql);
    _IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void UnlockQueue(KIRQL Irql);

    VioGpuCommand *DequeueRunning();
    void QueueRunning(VioGpuCommand *cmd);

    VioGpuCommand *DequeueSubmitted();
    void QueueSubmitted(VioGpuCommand *cmd);

    // Preempt request: stop starting queued packets and report the highest
    // fence id ever dequeued for execution (the preempt target).  Taken
    // under the queue lock so a dequeue cannot slip between the two.
    void PreemptHold(UINT *lastDequeued);
    // Preempt ack: resume, discarding every queued packet with an id at or
    // below dropThrough (dxgkrnl re-owns and resubmits those); 0 resumes
    // without discarding.  Callable at DISPATCH (the completion DPC).
    void PreemptRelease(UINT dropThrough);

  private:
    static void ThreadWork(PVOID Context);
    void ThreadWorkRoutine(void);

    VioGpuAdapter *m_pAdapter;

    KEVENT m_QueueEvent;
    PETHREAD m_pWorkThread;
    BOOLEAN m_bStopWorkThread;

    LIST_ENTRY m_SubmittedQueue;
    LIST_ENTRY m_RunningQueue;

    KSPIN_LOCK m_Lock;

    UINT m_running;

    // Under m_Lock.
    BOOLEAN m_PreemptHold;
    UINT m_LastDequeuedFenceId;
    BOOLEAN m_DropActive;
    UINT m_DropThroughFenceId;
};

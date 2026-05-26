#pragma once

#include "handle.h"

#define VIOGPU_MAX_RUNNING 1

class VioGpuAdapter;
class VioGpuDevice;
class VioGpuContext;
class VioGpuAllocation;
class VioGpuCommander;

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

    void PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);
    void QueueRunning();
    static void QueueRunningCb(void *cmd, void *, void *);

    void SetDmaBuf(char *pDmaBuffer)
    {
        m_pDmaBuffer = pDmaBuffer;
    }

    NTSTATUS AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength);

    LIST_ENTRY list_entry;

  private:
    VioGpuAdapter *m_pAdapter;
    VioGpuCommander *m_pCommander;
    VioGpuDevice *m_pDevice;

    VioGpuAllocation **m_allocations;
    UINT m_allocationsLength;

    UINT m_FenceId;
    // DXGK_SUBMITCOMMANDFLAGS.NullRendering: the runtime is timing
    // the submission path itself (profiling) and wants the fence to
    // complete without executing the DMA body.
    BOOLEAN m_NullRendering;

    // Outstanding async submissions where `this` is the complete_ctx.
    // Tracked so the dtor can assert no callback is still pending.
    volatile LONG m_pendingCallbacks;

    void AddPending();
    // Returns the post-decrement count so callers can re-queue the command
    // exactly once -- when the last outstanding async submission completes.
    LONG DropPending();

    char *m_pDmaBuffer;
    char *m_pCommand;
    char *m_pEnd;
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

    _IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql, Irql) _IRQL_raises_(DISPATCH_LEVEL) void LockQueue(
                                                                                                        KIRQL *Irql);
    _IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void UnlockQueue(KIRQL Irql);

    VioGpuCommand *DequeueRunning();
    void QueueRunning(VioGpuCommand *cmd);

    VioGpuCommand *DequeueSubmitted();
    void QueueSubmitted(VioGpuCommand *cmd);

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
};

#pragma once

#include "handle.h"

#define VIOGPU_MAX_RUNNING 1

class VioGpuAdapter;
class VioGpuDevice;
class VioGpuContext;
class VioGpuAllocation;
class VioGpuCommander;

class VioGpuCommand final : public HandleBase<"VIOGCOMM"_M, VioGpuCommand>
{
  public:
    VioGpuCommand(VioGpuAdapter *adapter);

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

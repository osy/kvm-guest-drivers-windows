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

#pragma once

#include "handle.h"
#include "viogpu_allocation.h"
#include "viogpu_queue.h"
#include "viogpu_command.h"
#include "viogpu_vidpn.h"

// GpuMmu page-table geometry: the x86-64 shape -- 48-bit VA split into a
// 12-bit page offset and 4 levels of 9 index bits, each level a 4KB table of
// 512 8-byte PTEs in system memory.  Anything unusual here is a shape VidMm
// never sees from real drivers, so it is the safest configuration to claim
// for page tables that exist only as bookkeeping: the rendering-bypass design
// runs real GPU work over virtio rings and never dereferences a GPU VA.
#define VIOGPU_WDDM2_VA_BIT_COUNT  48
#define VIOGPU_WDDM2_PT_LEVELS     4
#define VIOGPU_WDDM2_PTE_SIZE      8
#define VIOGPU_WDDM2_PT_INDEX_BITS 9
#define VIOGPU_WDDM2_PT_SIZE       4096

#pragma pack(push)
#pragma pack(1)
typedef struct
{
    UINT DriverStarted : 1;
    UINT HardwareInit : 1;
    UINT PointerEnabled : 1;
    UINT VgaDevice : 1;
    UINT FlexResolution : 1;
    UINT UsePhysicalMemory : 1;
    UINT Unused : 26;
} DRIVER_STATUS_FLAG;

#pragma pack(pop)

struct CAPSET_INFO
{
    ULONG max_version;
    ULONG max_size;
    ULONG id;
};

virtio_gpu_formats ColorFormat(UINT format);

class VioGpuAdapter final : public HandleBase<"VIOGADAP"_M, VioGpuAdapter>, IVioGpuPCI
{
  public:
    VioGpuCommander commander;
    VioGpuVidPN vidpn;
    VioGpuIdr resourceIdr;
    VioGpuIdr ctxIdr;
    CtrlQueue ctrlQueue;

    VioGpuMemSegment frameSegment;

    UINT64 m_u64HostFeatures;
    UINT64 m_u64GuestFeatures;
    UINT32 m_u32NumCapsets;
    UINT32 m_u32NumScanouts;
    UINT64 m_supportedCapsetIDs;

    // Per-scanout last-reported connect state, indexed by ChildUid.
    // UpdateChildStatus dedupes against this so DXGK only sees real
    // connected <-> disconnected transitions.
    BOOLEAN m_scanoutConnected[MAX_CHILDREN] = {FALSE};

    ULONGLONG GetShmemPA() {
        if (!m_VioDev.shmem.available) {
            return 0;
        }
        return m_PciResources.GetPciBar(m_VioDev.shmem.bar)->GetPA().QuadPart + m_VioDev.shmem.offset;
    }

    static const ULONGLONG SHMEM_GPU_BASE_VA = 0x700000000;
  private:
    DEVICE_OBJECT *m_pPhysicalDevice;
    DXGKRNL_INTERFACE m_DxgkInterface;
    DXGK_DEVICE_INFO m_DeviceInfo;

    DEVICE_POWER_STATE m_MonitorPowerState;
    DEVICE_POWER_STATE m_AdapterPowerState;
    DRIVER_STATUS_FLAG m_Flags;

    DXGKARG_SETPOINTERSHAPE m_PointerShape;

    VirtIODevice m_VioDev;
    CPciResources m_PciResources;

    CrsrQueue m_CursorQueue;
    VioGpuBuf m_GpuBuf;
    volatile ULONG m_PendingWorks;
    KEVENT m_ConfigUpdateEvent;
    PETHREAD m_pWorkThread;
    BOOLEAN m_bStopWorkThread;
    PKEVENT m_ResolutionEvent;
    HANDLE m_ResolutionEventHandle;

    VioGpuObj *m_pCursorBuf;
    VioGpuMemSegment m_CursorSegment;

    ULONG m_PciBus;
    ULONG m_PciDev;
    ULONG m_PciFunc;

    ULONG m_Id;
    CAPSET_INFO m_capsetInfos[VIRTIO_GPU_MAX_CAPSET_ID + 1];

    LUID m_AdapterLuid;
  public:
    VioGpuAdapter(_In_ DEVICE_OBJECT *pPhysicalDeviceObject);
    ~VioGpuAdapter(void);
#pragma code_seg(push)
#pragma code_seg()

    BOOLEAN IsDriverActive() const
    {
        return m_Flags.DriverStarted;
    }
    BOOLEAN IsHardwareInit() const
    {
        return m_Flags.HardwareInit;
    }
    void SetHardwareInit(BOOLEAN init)
    {
        m_Flags.HardwareInit = init;
    }
    BOOLEAN IsPointerEnabled() const
    {
        return m_Flags.PointerEnabled;
    }
    void SetPointerEnabled(BOOLEAN Enabled)
    {
        m_Flags.PointerEnabled = Enabled;
    }
    BOOLEAN IsVgaDevice(void) const
    {
#ifdef RENDER_ONLY
        return FALSE;
#else
        return m_Flags.VgaDevice;
#endif
    }
    void SetVgaDevice(BOOLEAN Vga)
    {
        m_Flags.VgaDevice = Vga;
    }
    BOOLEAN IsFlexResolution(void) const
    {
        return m_Flags.FlexResolution;
    }
    void SetFlexResolution(BOOLEAN FlexRes)
    {
        m_Flags.FlexResolution = FlexRes;
    }
    BOOLEAN IsUsePhysicalMemory() const
    {
        return m_Flags.UsePhysicalMemory;
    }
    void SetUsePhysicalMemory(BOOLEAN enable)
    {
        m_Flags.UsePhysicalMemory = enable;
    }
#pragma code_seg(pop)

    NTSTATUS StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                         _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                         _Out_ ULONG *pNumberOfViews,
                         _Out_ ULONG *pNumberOfChildren);
    NTSTATUS StopDevice(VOID);
    VOID ResetDevice(VOID);
    NTSTATUS DispatchIoRequest(_In_ ULONG VidPnSourceId, _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket);
    NTSTATUS SetPowerState(_In_ ULONG HardwareUid,
                           _In_ DEVICE_POWER_STATE DevicePowerState,
                           _In_ POWER_ACTION ActionType);
    NTSTATUS QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                 _In_ ULONG ChildRelationsSize);
    NTSTATUS QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly);
    NTSTATUS QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor);
    BOOLEAN InterruptRoutine(_In_ ULONG MessageNumber);
    VOID DpcRoutine(VOID);
    NTSTATUS QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo);
    NTSTATUS Escape(_In_ CONST DXGKARG_ESCAPE *pEscape);
    NTSTATUS QueryInterface(_In_ CONST PQUERY_INTERFACE QueryInterface);
    NTSTATUS StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                      _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo);
    PDXGKRNL_INTERFACE GetDxgkInterface(void)
    {
        return &m_DxgkInterface;
    }
    DEVICE_OBJECT *GetPhysicalDevice(void)
    {
        return m_pPhysicalDevice;
    }
    NTSTATUS NotifyInterrupt(DXGKARGCB_NOTIFY_INTERRUPT_DATA *interruptData, BOOL triggerDpc);

    CPciResources *GetPciResources(void)
    {
        return &m_PciResources;
    }
    BOOLEAN IsMSIEnabled()
    {
        return m_PciResources.IsMSIEnabled();
    }

    VioGpuAllocation *AllocationFromHandle(D3DKMT_HANDLE handle);
    VioGpuResource *ResourceFromHandle(D3DKMT_HANDLE handle);

    // ---- allocation resolution under GpuMmu ------------------------------
    //
    // DxgkCbGetHandleData returns NULL for every allocation handle once the
    // driver registers a WDDM2 interface version, so the three maps below are
    // how an open, an escape, or a by-id DMA command finds its allocation.
    // All of them are guarded by m_PendingCreateLock.

    // Create->open pairing: dxgkrnl calls DxgkDdiCreateAllocation and the
    // paired DxgkDdiOpenAllocation back-to-back on the same thread, so creates
    // push here and in-create opens pop their own thread's entry.
    VOID PendingCreatePush(VioGpuAllocation *allocation);
    VioGpuAllocation *PendingCreatePop(VOID);
    VOID PendingCreateRemove(VioGpuAllocation *allocation);

    // D3DKMT handle -> allocation bindings learned at DxgkDdiOpenAllocation,
    // the only place both are visible together.  The UMD's RES_INFO/RES_BUSY
    // escapes resolve through this, as does AllocationByResId.
    VOID KmtMapInsert(D3DKMT_HANDLE handle, VioGpuAllocation *allocation);
    VOID KmtMapRemove(D3DKMT_HANDLE handle, VioGpuAllocation *allocation);
    VOID KmtMapRemoveByAllocation(VioGpuAllocation *allocation);
    VioGpuAllocation *KmtMapLookup(D3DKMT_HANDLE handle);

    // Lookup cookie -> allocation, taken from the allocation private data
    // dxgkrnl replays at every open.  This is the only binding that survives a
    // cross-process open, where the create->open pairing (in-create opens
    // only) and the KMT map (seeded at first open) both miss.
    VOID CookieMapInsert(ULONGLONG cookie, VioGpuAllocation *allocation);
    VOID CookieMapRemoveByAllocation(VioGpuAllocation *allocation);
    VioGpuAllocation *CookieMapLookup(ULONGLONG cookie);

    VioGpuAllocation *AllocationByResId(UINT resId);

    // KMD-owned shmem window suballocation, page granularity.  VidMm never
    // commits host-backed blobs into the shmem segment under GpuMmu, so the
    // KMD hands out offsets itself and maps BAR+offset into the UMD process
    // during the RES_INFO escape.
    ULONGLONG ShmemAlloc(SIZE_T size);
    VOID ShmemFree(ULONGLONG offset, SIZE_T size);

    // One node per map; `key` is the D3DKMT handle or the lookup cookie.
    struct ALLOC_MAP_ENTRY
    {
        LIST_ENTRY entry;
        ULONGLONG key;
        VioGpuAllocation *allocation;
    };
    KSPIN_LOCK m_PendingCreateLock;
    LIST_ENTRY m_PendingCreateList;
    LIST_ENTRY m_KmtMapList;
    LIST_ENTRY m_CookieMapList;

    // Shmem suballocator, lazy-initialized on first ShmemAlloc.
    RTL_BITMAP m_ShmemBitmap;
    PULONG m_ShmemBitmapBuffer = NULL;
    ULONG m_ShmemPageCount = 0;
    // Where every ShmemAlloc search starts.  Deliberately a FIXED base,
    // not a rolling bump pointer -- see ShmemAlloc.
    ULONG m_ShmemSearchBase = 0;

    PHYSICAL_ADDRESS GetFrameBufferPA(void)
    {
        return m_PciResources.GetPciBar(0)->GetPA();
    }

    volatile LONG m_LastCompletedFenceId;
    volatile LONG m_LastSubmittedFenceId;

    // Fences dxgkrnl re-owned at a preempt ack or an engine reset: it
    // resubmits (or aborts) those packets under NEWER ids, so a stray
    // completion carrying an original id must NOT raise DMA_COMPLETED --
    // reporting an id past the acknowledged watermark is an invalid fence
    // report and bugchecks 0x119 arg1=1.
    volatile LONG m_PreemptSkipThroughFenceId;

    // Pending preempt request (interrupt-lock domain).  The engine cannot
    // preempt a packet the commander has already started -- its body has
    // been handed to the host and will execute -- so a preempt is acked only
    // once the completion watermark reaches PreemptTargetFenceId, the last
    // id dequeued for execution when the request arrived.  Packets behind
    // it never start (the commander is held) and are the ones dxgkrnl
    // resubmits.  Acking earlier declares executed packets preempted, and
    // dxgkrnl then submits their bodies a second time (a CREATE_RING twice
    // for one blob kills the host context; a MAP/UNMAP twice, a present
    // twice, ...).
    BOOLEAN m_PreemptPending;
    UINT m_PreemptFenceId;
    UINT m_PreemptNode;
    UINT m_PreemptEngine;
    UINT m_PreemptTargetFenceId;

    // {advance m_LastCompletedFenceId + raise DMA_COMPLETED} and {read
    // watermark + set skip-window + raise DMA_PREEMPTED} must be mutually
    // atomic: a preempt ack must never report an id whose completion interrupt
    // is still unraised (0x119 arg1=1 duplicate), and completions in the acked
    // window must be squashed.  A driver spinlock held across
    // DxgkCbSynchronizeExecution deadlocks, so the atomicity rides the
    // interrupt lock instead -- both operations run inside a
    // SynchronizeExecution routine.
    BOOLEAN ReportDmaCompleted(UINT fenceId, UINT node, UINT engine);
    // Record the request; acks immediately when nothing is executing, else
    // the ack rides the completion that reaches the target.
    void ReportDmaPreempted(UINT preemptFenceId, UINT node, UINT engine);

    // Retire a packet's fence without executing it.  The submission DDIs
    // cannot report failure -- an error return from them is a defined 0x119
    // bugcheck -- and cannot silently swallow the packet either, because
    // dxgkrnl would then wait forever on an id nothing completes and
    // ResetFenceStateFromTimeout only syncs up to m_LastSubmittedFenceId.
    // The contiguity window holds the id back until its predecessors land, so
    // this cannot regress the watermark.
    void CompleteFenceWithoutWork(UINT fenceId, UINT node, UINT engine);

    // TDR recovery.  DxgkDdiResetFromTimeout must leave the adapter in a
    // state where the scheduler can resume: everything it submitted has to
    // read as completed (it re-owns those packets and resubmits under fresh
    // ids), or DxgkDdiQueryCurrentFence keeps reporting a watermark below
    // m_LastSubmittedFenceId forever and the node never restarts.  Runs the
    // sync-up under the interrupt lock for the same atomicity reason as the
    // preempt ack above.  Returns the fence id everything was synced to.
    UINT ResetFenceStateFromTimeout(void);

    // ---- present-fence tokens (render -> flip ordering) ------------------
    //
    // Each VIOGPU_SUBMIT_PRESENT_FENCE escape gets a monotonic token.  The
    // host defers that fence's used-ring response until the frame's real GPU
    // completion, so the token retiring means "everything submitted before
    // the escape has finished on the host GPU".  VioGpuVidPN holds a flip
    // until the token its contents depend on has retired.  See
    // viogpu_vidpn.h.
    //
    // The association is per THREAD: the UMD arms the fence
    // (VIOGPU_SUBMIT_PRESENT_FENCE) and then calls pfnPresentCb on the SAME
    // thread, and DxgkDdiPresent runs synchronously in that thread -- so the
    // escape stamps its token under the current thread id and the flip
    // consumes its own thread's stamp.  Exact pairing, cross-process safe,
    // and a flip whose fence had already completed (the UMD arms only when
    // GetCompletedValue < v) finds no stamp and correctly runs un-gated.
    //
    // It must NOT be the peeked newest token: a pipelined app latches flip
    // N+1 (with a newer, unretired token) before token N retires, so a
    // newest-token dependency is unretired at EVERY promote check and each
    // new latch also resets the fallback deadline -- the scanout livelocks
    // parked and the display freezes while rendering continues at full
    // speed.
    // And it can NOT be keyed on the device: the escape arrives on the npt
    // transport's private D3DKMT device while the flip arrives on the
    // runtime's device, so a per-device stamp is never read back.
    //
    // Retirement is tracked per token, not as a maximum.
    //
    // The token counter is adapter-wide, but the completions that retire those
    // tokens are not one stream: each arm rides its own device's host context
    // and is round-robined across the event rings, each drained by its own
    // host worker.  Token order therefore says nothing about completion order
    // across devices -- the compositor's 1 ms composite and a fullscreen
    // workload's 200 ms frame draw adjacent tokens from the same counter and
    // retire in whatever order they finish.
    //
    // A maximum watermark would turn that into corruption: one early retire
    // marks every older in-flight token as done, and the flips depending on
    // them scan out buffers the GPU is still writing -- a partial frame,
    // correct below the line the writer had reached and stale above it.
    //
    // So "has token T retired" is answered for T exactly.  Tokens are dense
    // and monotonic, so a circular bitmap indexed by T answers it in O(1),
    // and m_PresentTokenDone is kept as the contiguous watermark (every token
    // <= it has retired) which bounds the bitmap from below.
// Circular retired-token bitmap.  Must comfortably exceed the number of arms
// that can be in flight adapter-wide; at ~100 arms/s a lap is minutes.
#define VIOGPU_PRESENT_TOKEN_RING 4096

    ULONGLONG PresentTokenSubmit(void)
    {
        ULONGLONG token = (ULONGLONG)InterlockedIncrement64(&m_PresentTokenNext);
        // Clear this token's ring slot before handing it out.  Bits are
        // normally cleared as the contiguous watermark absorbs them, but a
        // completion that never arrives (adapter reset drops in-flight
        // responses) stalls that watermark, and the slot would still hold the
        // bit set by token-RING when the ring laps -- reading the new token as
        // already retired.  Clearing on issue makes the ring self-healing.
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_PresentTokenLock, &oldIrql);
        m_PresentTokenRetiredBits[(token % VIOGPU_PRESENT_TOKEN_RING) / 64] &=
            ~(1ULL << (token % 64));
        KeReleaseSpinLock(&m_PresentTokenLock, oldIrql);
        return token;
    }

    // ---- per-thread arm->flip pairing (see the block comment above) ------
    //
    // Small open-addressed table keyed on the arming thread id.  Linear
    // probe of 4; on a full neighbourhood the home slot is overwritten --
    // the cost of a lost stamp is one un-gated flip (a possible transient
    // tear), never a stall.  Take() always clears the slot, so a stamp that
    // never met a flip (windowed presents whose frames go to DWM instead)
    // is displaced by the thread's next arm rather than accumulating.
#define VIOGPU_THREAD_TOKEN_SLOTS 64
#define VIOGPU_THREAD_TOKEN_PROBE 4
    struct THREAD_TOKEN_SLOT
    {
        HANDLE Tid;
        ULONGLONG Token;
        // VIOGPU_PRESENT_GATE_HINT: the flip consuming this stamp parks its
        // own DMA packet on the token (see PresentWaitConsume).
        BOOLEAN PacketGate;
    };

    void StampThreadToken(HANDLE tid, ULONGLONG token)
    {
        ULONG home = VioGpuThreadTokenHash(tid);
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_ThreadTokenLock, &oldIrql);
        ULONG victim = home;
        BOOLEAN displaced = TRUE;
        for (ULONG i = 0; i < VIOGPU_THREAD_TOKEN_PROBE; i++)
        {
            ULONG idx = (home + i) & (VIOGPU_THREAD_TOKEN_SLOTS - 1);
            if (m_ThreadTokens[idx].Tid == tid || m_ThreadTokens[idx].Tid == NULL)
            {
                victim = idx;
                displaced = FALSE;
                break;
            }
        }
        m_ThreadTokens[victim].Tid = tid;
        m_ThreadTokens[victim].Token = token;
        m_ThreadTokens[victim].PacketGate = FALSE;
        KeReleaseSpinLock(&m_ThreadTokenLock, oldIrql);
        if (displaced)
        {
            // Another thread's live stamp was overwritten: that thread's
            // next flip gates on token 0 (always retired) and can scan out
            // one frame early -- a one-frame flash, never a stall.  Counted
            // and logged so the artifact is diagnosable.
            LONG n = InterlockedIncrement(&m_ThreadTokenDisplaced);
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s: thread-token slot displaced (#%ld, tid=%p) -- "
                      "victim's next flip runs un-gated\n",
                      __FUNCTION__, n, tid));
        }
    }

    // VIOGPU_PRESENT_GATE_HINT: flag the caller's live stamp.  FALSE = no
    // stamp to flag (fast-path present whose fence already completed, or the
    // stamp was displaced) -- the escape reports it so the UMD can fall back
    // to its CPU wait for that frame.
    BOOLEAN MarkThreadTokenPacketGate(HANDLE tid)
    {
        ULONG home = VioGpuThreadTokenHash(tid);
        BOOLEAN marked = FALSE;
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_ThreadTokenLock, &oldIrql);
        for (ULONG i = 0; i < VIOGPU_THREAD_TOKEN_PROBE; i++)
        {
            ULONG idx = (home + i) & (VIOGPU_THREAD_TOKEN_SLOTS - 1);
            if (m_ThreadTokens[idx].Tid == tid)
            {
                marked = m_ThreadTokens[idx].Token != 0;
                m_ThreadTokens[idx].PacketGate = marked;
                break;
            }
        }
        KeReleaseSpinLock(&m_ThreadTokenLock, oldIrql);
        return marked;
    }

    ULONGLONG TakeThreadToken(HANDLE tid, BOOLEAN *pPacketGate = NULL)
    {
        ULONG home = VioGpuThreadTokenHash(tid);
        ULONGLONG token = 0;
        BOOLEAN packetGate = FALSE;
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_ThreadTokenLock, &oldIrql);
        for (ULONG i = 0; i < VIOGPU_THREAD_TOKEN_PROBE; i++)
        {
            ULONG idx = (home + i) & (VIOGPU_THREAD_TOKEN_SLOTS - 1);
            if (m_ThreadTokens[idx].Tid == tid)
            {
                token = m_ThreadTokens[idx].Token;
                packetGate = m_ThreadTokens[idx].PacketGate;
                m_ThreadTokens[idx].Tid = NULL;
                m_ThreadTokens[idx].Token = 0;
                m_ThreadTokens[idx].PacketGate = FALSE;
                break;
            }
        }
        KeReleaseSpinLock(&m_ThreadTokenLock, oldIrql);
        if (pPacketGate != NULL)
        {
            *pPacketGate = packetGate;
        }
        return token;
    }

    // Contiguous watermark: every token <= this has retired.  Diagnostic
    // only -- readiness is the exact per-token query below.
    ULONGLONG PresentTokenDone(void)
    {
        return (ULONGLONG)InterlockedCompareExchange64(&m_PresentTokenDone, 0, 0);
    }

    // Has THIS token retired?  Token 0 means "flip carries no render
    // dependency" (the UMD arms only when the fence had not already completed)
    // and is always ready.
    BOOLEAN PresentTokenRetired(ULONGLONG token)
    {
        if (token == 0)
        {
            return TRUE;
        }
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_PresentTokenLock, &oldIrql);
        BOOLEAN retired = PresentTokenRetiredLocked(token);
        KeReleaseSpinLock(&m_PresentTokenLock, oldIrql);
        return retired;
    }

    void PresentTokenRetire(ULONGLONG token)
    {
        if (token == 0)
        {
            return;
        }
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_PresentTokenLock, &oldIrql);
        if ((LONG64)token > m_PresentTokenDone)
        {
            m_PresentTokenRetiredBits[(token % VIOGPU_PRESENT_TOKEN_RING) / 64] |=
                (1ULL << (token % 64));
            // Advance the contiguous watermark over the run this completed,
            // clearing each bit as it is absorbed so the ring stays reusable.
            for (;;)
            {
                ULONGLONG next = (ULONGLONG)m_PresentTokenDone + 1;
                ULONGLONG *word = &m_PresentTokenRetiredBits[(next % VIOGPU_PRESENT_TOKEN_RING) / 64];
                ULONGLONG bit = 1ULL << (next % 64);
                if ((*word & bit) == 0)
                {
                    break;
                }
                *word &= ~bit;
                m_PresentTokenDone = (LONG64)next;
            }
        }
        KeReleaseSpinLock(&m_PresentTokenLock, oldIrql);
    }

    // ---- packet-gated presents (VIOGPU_PRESENT_GATE_HINT) ----------------
    //
    // A windowed flip present's empty DMA packet completes on submission
    // order -- CPU speed -- and that completion is what dxgkrnl, and the
    // compositor behind it, read as "frame ready".  On real hardware packet
    // completion IS GPU completion; here the GPU work runs host-side, so an
    // un-parked packet reports the frame ready while the host GPU is still
    // rendering it and the compositor samples a half-written backbuffer.
    // Parking the packet on the frame's present token restores that
    // equivalence without a CPU wait in the present DDI, so CPU frame N+1
    // overlaps GPU frame N.  Same deadline discipline as the monitored-fence
    // gates (GateExpiryDpcRoutine) so a dead host worker degrades instead of
    // TDR-bugchecking.
    struct PRESENT_WAIT_CONSUMER
    {
        LIST_ENTRY Entry;
        ULONGLONG Token;
        void (*Cb)(void *, void *, void *); // VioGpuCommand::QueueRunningCb
        void *Ctx;
        ULONGLONG ParkTime; // KeQueryInterruptTime at park
    };
    // Park `cb` until `token` retires (immediate when it already has).
    void PresentWaitConsume(ULONGLONG token, void (*cb)(void *, void *, void *), void *ctx, UINT fenceId);
    // Fire every parked consumer whose token has retired.  Called after each
    // PresentTokenRetire (and from the expiry DPC for deadline sweeps).
    void PresentWaitSweep(void);
    LIST_ENTRY m_PresentWaitList; // guarded by m_PresentTokenLock

    // Per-escape completion context, carried through SubmitCommand to
    // PresentFenceCb.  Allocated from a nonpaged lookaside list: fixed-size,
    // DISPATCH-safe free from the response DPC, per-processor cached -- and
    // unlike a fixed ring it cannot wrap onto a live entry no matter how
    // many arms are in flight (fence-event arms are per
    // ID3D11Fence::SetEventOnCompletion, adapter-wide, so the in-flight
    // count is unbounded by design).
    struct PRESENT_FENCE_CTX
    {
        VioGpuAdapter *pAdapter;
        ULONGLONG Token;
        PKEVENT pEvent;    // optional user-mode wake; NULL for kernel-gated flips
        PKEVENT pAppEvent; // optional direct app wake (SetEventOnCompletion
                           // hEvent), signalled from the DPC before pEvent so
                           // the app never waits out the UMD waiter-thread hop
    };
    NPAGED_LOOKASIDE_LIST m_PresentFenceLookaside;

    // ---- monitored-fence gates (VIOGPU_ARM_GATE / VIOGPU_CMD_GATE) -------
    //
    // The D3D12 runtime never calls the queue fence DDIs on this driver; it
    // services app fences (queue::Signal -> SetEventOnCompletion /
    // GetCompletedValue) through dxgkrnl's monitored-fence packets on the
    // queue's kernel context.  Those packets complete when the context's
    // prior DMA completes -- and without gates a queue context has NO DMA,
    // so every fence completes at CPU speed while the GPU work is still
    // running on the host, and the app reads back a frame the GPU has
    // not produced.
    //
    // A gate bridges the gap: the UMD arms an event-ring fence that the
    // host retires at true GPU completion of the queue's work
    // (VIOGPU_ARM_GATE -> GateFenceCb), and submits a small DMA packet on
    // the queue's kernel context (VIOGPU_CMD_GATE{token}) whose completion
    // is parked until the token fires.  dxgkrnl then releases the
    // monitored-fence packets only after the GPU truly drained.
    struct GATE_CONSUMER
    {
        LIST_ENTRY Entry;
        void (*Cb)(void *, void *, void *); // VioGpuCommand::QueueRunningCb
        void *Ctx;
        UINT FenceId; // parked packet's submission fence (gate-hold key)
    };
    struct GATE_TOKEN_CTX
    {
        LIST_ENTRY Entry;
        VioGpuAdapter *pAdapter;
        ULONGLONG Token;
        BOOLEAN Fired;
        BOOLEAN Consumed;
        BOOLEAN Expired;      // deadline completed the parked consumers early
        ULONGLONG ParkTime;   // KeQueryInterruptTime at first consumer park; 0 = none
        LIST_ENTRY Consumers; // GATE_CONSUMER
    };
    NTSTATUS GateArm(class VioGpuDevice *pDevice, ULONG ringIdx, ULONGLONG *outToken);
    void GateConsume(ULONGLONG token, void (*cb)(void *, void *, void *), void *ctx, UINT fenceId);
    static void GateFenceCb(void *ctx, void *, void *);

    // Parked-gate deadline.  A gate token fires only if its host render
    // worker survives to retire the event-ring fence; a dead worker means
    // the parked VIOGPU_CMD_GATE packet never completes, and dxgkrnl's
    // scheduler escalates that into a node timeout -> bugcheck 0x116
    // VIDEO_TDR_FAILURE (a TDR is fatal on this driver).  The flip path
    // already bounds its token waits; this is the same discipline for the
    // gate DMA: a periodic DPC completes any consumer parked longer than
    // the deadline, logging loudly.  Expiring early merely runs that
    // batch's monitored fences at pre-gate (CPU) speed -- transiently
    // degraded, recoverable -- instead of bugchecking the OS.  The
    // deadline + scan period must stay comfortably inside dxgkrnl's
    // TdrDelay (2 s); healthy gate holds measure in the 10s-100s of ms.
    static const ULONGLONG VIOGPU_GATE_PARK_DEADLINE_100NS = 12000000ULL; // 1.2 s
    static const LONG VIOGPU_GATE_EXPIRY_PERIOD_MS = 400;
    static VOID GateExpiryDpcRoutine(_In_ struct _KDPC *Dpc,
                                     _In_opt_ PVOID DeferredContext,
                                     _In_opt_ PVOID SystemArgument1,
                                     _In_opt_ PVOID SystemArgument2);
    KTIMER m_GateExpiryTimer;
    KDPC m_GateExpiryDpc;

    // Deferred monitored-fence value write (SIGNAL_MONITORED_FENCE paging
    // op).  The value must become visible exactly when dxgkrnl learns the
    // signal's packet completed -- the DMA_COMPLETED watermark crossing
    // fenceId -- and not before, so GetCompletedValue pollers can't observe
    // it ahead of the GPU work it covers.  The record carries a KVA pinned
    // at build time (PASSIVE): the completion sync routine that advances
    // the watermark runs at DIRQL, where it may do nothing but a bare
    // store through it.  The packet itself is never parked; it completes
    // on its normal schedule.
    struct MFENCE_DEFER
    {
        LIST_ENTRY Entry;
        UINT FenceId;
        volatile UINT64 *Kva; // pinned mapping of the fence value cell
        ULONGLONG Phys;       // pin bookkeeping (MFenceMapUnpin)
        UINT64 Value;
    };
    void MFenceDeferOrWrite(UINT fenceId, volatile UINT64 *kva, ULONGLONG phys, UINT64 value);
    void MFenceWrite(ULONGLONG phys, UINT64 value);
    LIST_ENTRY m_MFenceDeferList; // interrupt-lock domain (sync routines only)

    // Private-data area of the paging buffer currently carrying a deferred
    // monitored-fence command, NULL when none is outstanding.  VidMm batches
    // several paging operations into one buffer and the driver hands the
    // private data back unadvanced, so every operation sees the same slot:
    // without this, the entry-time clear that rejects a recycled buffer's
    // stale command pointer would also erase a stamp an earlier operation in
    // the same batch had just placed.  Written at PASSIVE from
    // DxgkDdiBuildPagingBuffer, cleared from the submit DDIs at DISPATCH.
    void *volatile m_PagingStampPriv = NULL;

    // Kernel mappings of the monitored-fence storage pages, keyed on the
    // physical page and direct-mapped.  Every signal writes 8 bytes into
    // one of a handful of pinned pages, so the mapping is established once
    // and reused: a map/unmap round trip per signal costs a system-PTE
    // reservation plus the TLB shootdown IPI that MmUnmapIoSpace broadcasts
    // to every CPU, on the packet-completion path the scheduler times.
    // Slots are recycled by eviction and released at adapter teardown.
    struct MFENCE_MAP_SLOT
    {
        ULONGLONG PhysPage;   // page-aligned physical address, 0 = free
        volatile UINT64 *Kva; // MmMapIoSpaceEx mapping of PhysPage
        LONG Pins;            // outstanding MFENCE_DEFER records using Kva
    };
    static const ULONG MFENCE_MAP_SLOTS = 64;
    MFENCE_MAP_SLOT m_MFenceMap[MFENCE_MAP_SLOTS];
    KSPIN_LOCK m_MFenceMapLock;
    void MFenceMapRelease(void);
    // Pin the cached mapping of the fence cell at phys (PASSIVE -- may
    // establish the mapping).  NULL = unmappable / slot contention; the
    // caller falls back to the immediate write.  A pinned slot is never
    // evicted, so the returned KVA stays valid until MFenceMapUnpin.
    volatile UINT64 *MFenceMapPin(ULONGLONG phys);
    void MFenceMapUnpin(ULONGLONG phys); // <= DISPATCH

    // Completion-contiguity window.  DMA_COMPLETED is a watermark that
    // acknowledges everything <= N, but gates make the node's completion
    // sequence SPARSE: a parked gate packet withholds its report while later
    // submissions keep completing.  Raising any later id would implicitly
    // complete the gate early, releasing parked monitored-fence writes ahead
    // of the work they cover.  So completions are recorded per-id and the
    // watermark raised only to the highest CONTIGUOUSLY-completed one.  Byte i
    // of m_Win means (m_WinBase + 1 + i) is completed but unraised.  Lives in
    // the interrupt-lock domain (SynchronizeExecution routines only).
    static const UINT VIOGPU_FENCE_WINDOW = 256;
    UINT m_WinBase = 0; // == (UINT)m_LastCompletedFenceId, or skip-through after preempt
    UCHAR m_Win[VIOGPU_FENCE_WINDOW] = {};

    LIST_ENTRY m_GateList;
    KSPIN_LOCK m_GateLock;
    volatile LONG64 m_GateTokenNext = 0;

    // GPU-VA -> physical shadow for monitored-fence signal writes (WDDM2
    // GpuMmu).  Leaf UPDATE_PAGE_TABLE ops covering system memory are
    // recorded; SIGNAL_MONITORED_FENCE resolves its GpuVa here and CPU-writes
    // the fence value, which is what satisfies dxgkrnl's GPU-side signal path
    // and the CPU waiters behind it.
    //
    // GpuMmu virtual addresses are per address space, so the key is
    // (hProcess, vaPage): DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE::hProcess
    // names the process whose page tables an update belongs to, and two
    // processes routinely hold the same GPU VA over different pages.  The
    // hash mixes only the VA so that every process holding one VA shares a
    // probe chain -- SIGNAL_MONITORED_FENCE carries no process handle, and
    // walking that chain is what lets the lookup recognise an ambiguous VA
    // rather than resolve it into the wrong address space.  Open-addressed
    // with linear probing; slot states: vaPage==0 => empty, vaPage!=0 &&
    // phys==0 => tombstone (keeps the chain intact), else occupied.
    struct VaShadowEntry
    {
        HANDLE hProcess;  // owning address space (UpdatePageTable.hProcess)
        ULONGLONG vaPage; // page-aligned GPU VA, 0 = empty
        ULONGLONG phys;   // page-aligned system physical address, 0 = tombstone
    };
    static const ULONG VA_SHADOW_SIZE = 8192;
    VaShadowEntry m_vaShadow[VA_SHADOW_SIZE];

    void VaShadowInsert(HANDLE hProcess, ULONGLONG vaPage, ULONGLONG physPage);
    void VaShadowRemove(HANDLE hProcess, ULONGLONG vaPage);
    BOOLEAN VaShadowLookup(ULONGLONG va, ULONGLONG *physOut);
  private:
    static ULONG VioGpuThreadTokenHash(HANDLE tid)
    {
        // Thread ids are multiples of 4; fold the useful bits.
        ULONG_PTR v = (ULONG_PTR)tid >> 2;
        return (ULONG)(v ^ (v >> 6)) & (VIOGPU_THREAD_TOKEN_SLOTS - 1);
    }

    THREAD_TOKEN_SLOT m_ThreadTokens[VIOGPU_THREAD_TOKEN_SLOTS] = {};
    KSPIN_LOCK m_ThreadTokenLock;
    // Diagnostic: live foreign stamps overwritten by StampThreadToken (each
    // one is a potential single-frame early scanout on the victim thread).
    volatile LONG m_ThreadTokenDisplaced = 0;

    // Caller holds m_PresentTokenLock.
    BOOLEAN PresentTokenRetiredLocked(ULONGLONG token)
    {
        if ((LONG64)token <= m_PresentTokenDone)
        {
            return TRUE;
        }
        // A token far enough behind the newest arm cannot still be pending:
        // the ring has recycled its bit.  Read it as retired so a completion
        // that is genuinely lost parks its flip only until the ring laps it
        // rather than forever (the flip deadline fires long before that).
        ULONGLONG next = (ULONGLONG)InterlockedCompareExchange64(&m_PresentTokenNext, 0, 0);
        if (next > token + (VIOGPU_PRESENT_TOKEN_RING / 2))
        {
            return TRUE;
        }
        return (m_PresentTokenRetiredBits[(token % VIOGPU_PRESENT_TOKEN_RING) / 64] &
                (1ULL << (token % 64))) != 0;
    }

    volatile LONG64 m_PresentTokenNext = 0;
    volatile LONG64 m_PresentTokenDone = 0;   // contiguous watermark
    KSPIN_LOCK m_PresentTokenLock;
    ULONGLONG m_PresentTokenRetiredBits[VIOGPU_PRESENT_TOKEN_RING / 64] = {};

    BOOLEAN CheckHardware();
    NTSTATUS WriteRegistryString(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PCSTR pszValue);
    NTSTATUS WriteRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PDWORD pdwValue);
    NTSTATUS ReadRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _Inout_ PDWORD pdwValue);
    NTSTATUS SetRegisterInfo(_In_ ULONG Id, _In_ DWORD MemSize);
    NTSTATUS GetRegisterInfo(void);
    NTSTATUS GetPCIInfo(void);

    NTSTATUS HWInit(PCM_RESOURCE_LIST pResList);
    NTSTATUS HWClose(void);

    ULONG GetInstanceId(void)
    {
        return m_Id;
    }

    NTSTATUS VioGpuAdapterInit();
    void VioGpuAdapterClose(void);
    NTSTATUS VirtIoDeviceInit(void);
    BOOLEAN AckFeature(UINT64 Feature);

    void static ThreadWork(_In_ PVOID Context);
    void ThreadWorkRoutine(void);

    void ConfigChanged(void);

    VOID CreateResolutionEvent(VOID);
    VOID NotifyResolutionEvent(VOID);
    VOID CloseResolutionEvent(VOID);

    NTSTATUS UpdateChildStatus(BOOLEAN connect);

    NTSTATUS SetPowerState(DXGK_DEVICE_INFO *pDeviceInfo,
                           DEVICE_POWER_STATE DevicePowerState,
                           CURRENT_MODE *pCurrentMode);
    BOOLEAN InterruptRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface, _In_ ULONG MessageNumber);
    VOID DpcRoutine(_In_ PDXGKRNL_INTERFACE pDxgkInterface);

    UINT64 RequestParameter(ULONG parmeter);
};

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

    PHYSICAL_ADDRESS GetFrameBufferPA(void)
    {
        return m_PciResources.GetPciBar(0)->GetPA();
    }

    volatile LONG m_LastCompletedFenceId;
    volatile LONG m_LastSubmittedFenceId;

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
    };

    void StampThreadToken(HANDLE tid, ULONGLONG token)
    {
        ULONG home = VioGpuThreadTokenHash(tid);
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_ThreadTokenLock, &oldIrql);
        ULONG victim = home;
        for (ULONG i = 0; i < VIOGPU_THREAD_TOKEN_PROBE; i++)
        {
            ULONG idx = (home + i) & (VIOGPU_THREAD_TOKEN_SLOTS - 1);
            if (m_ThreadTokens[idx].Tid == tid || m_ThreadTokens[idx].Tid == NULL)
            {
                victim = idx;
                break;
            }
        }
        m_ThreadTokens[victim].Tid = tid;
        m_ThreadTokens[victim].Token = token;
        KeReleaseSpinLock(&m_ThreadTokenLock, oldIrql);
    }

    ULONGLONG TakeThreadToken(HANDLE tid)
    {
        ULONG home = VioGpuThreadTokenHash(tid);
        ULONGLONG token = 0;
        KIRQL oldIrql;
        KeAcquireSpinLock(&m_ThreadTokenLock, &oldIrql);
        for (ULONG i = 0; i < VIOGPU_THREAD_TOKEN_PROBE; i++)
        {
            ULONG idx = (home + i) & (VIOGPU_THREAD_TOKEN_SLOTS - 1);
            if (m_ThreadTokens[idx].Tid == tid)
            {
                token = m_ThreadTokens[idx].Token;
                m_ThreadTokens[idx].Tid = NULL;
                m_ThreadTokens[idx].Token = 0;
                break;
            }
        }
        KeReleaseSpinLock(&m_ThreadTokenLock, oldIrql);
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
        PKEVENT pEvent; // optional user-mode wake; NULL for kernel-gated flips
    };
    NPAGED_LOOKASIDE_LIST m_PresentFenceLookaside;

  private:
    static ULONG VioGpuThreadTokenHash(HANDLE tid)
    {
        // Thread ids are multiples of 4; fold the useful bits.
        ULONG_PTR v = (ULONG_PTR)tid >> 2;
        return (ULONG)(v ^ (v >> 6)) & (VIOGPU_THREAD_TOKEN_SLOTS - 1);
    }

    THREAD_TOKEN_SLOT m_ThreadTokens[VIOGPU_THREAD_TOKEN_SLOTS] = {};
    KSPIN_LOCK m_ThreadTokenLock;

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

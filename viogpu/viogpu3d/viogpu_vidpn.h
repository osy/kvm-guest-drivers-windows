#pragma once

#include "helper.h"
#include "viogpu.h"

class VioGpuAdapter;
class VioGpuAllocation;
class VioGpuObj;

typedef struct _CURRENT_MODE
{
    DXGK_DISPLAY_INFORMATION DispInfo;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
    D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling;
    UINT SrcModeWidth;
    UINT SrcModeHeight;
    struct _CURRENT_MODE_FLAGS
    {
        UINT SourceNotVisible : 1;
        UINT FullscreenPresent : 1;
        UINT FrameBufferIsActive : 1;
        UINT DoNotMapOrUnmap : 1;
        UINT IsInternal : 1;
        UINT Unused : 27;
    } Flags;

    PHYSICAL_ADDRESS ZeroedOutStart;
    PHYSICAL_ADDRESS ZeroedOutEnd;

    union {
        VOID *Ptr;
        ULONG64 Force8Bytes;
    } FrameBuffer;
} CURRENT_MODE;

class VioGpuVidPN
{
  public:
    VioGpuVidPN(VioGpuAdapter *adapter);
    ~VioGpuVidPN();

    NTSTATUS Start(ULONG *pNumberOfViews, ULONG *pNumberOfChildren);
    NTSTATUS AcquirePostDisplayOwnership();
    void ReleasePostDisplayOwnership(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, DXGK_DISPLAY_INFORMATION *pDisplayInfo);
    void Powerdown();

    NTSTATUS IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const;
    NTSTATUS IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const;

    NTSTATUS CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn);
    NTSTATUS
    UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath);

    NTSTATUS SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode);
    ULONG GetModeCount(void)
    {
        return m_ModeCount;
    }
    VOID BlackOutScreen(CURRENT_MODE *pCurrentMod);

    NTSTATUS GetModeList(DXGK_DISPLAY_INFORMATION *pDispInfo);

    void CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode);
    void DestroyFrameBufferObj(BOOLEAN bReset);

    BOOLEAN GpuObjectAttach(UINT res_id, VioGpuObj *obj);
    PBYTE GetEdidData(UINT Idx);

    PBYTE GetCTA861Data(void);
    void SetVideoModeInfo(UINT Idx, PVIOGPU_DISP_MODE pModeInfo);
    BOOLEAN GetDisplayInfo(void);
    int ProcessEdid(void);
    void FixEdid(void);
    BOOLEAN GetEdids(void);
    int AddEdidModes(void);
    BOOLEAN UpdateModes(USHORT xres, USHORT yres, int &cnt);
    void SetCustomDisplay(_In_ USHORT xres, _In_ USHORT yres);

    NTSTATUS EscapeCustomResoulution(VIOGPU_DISP_MODE *resolution);

    NTSTATUS IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn);
    NTSTATUS RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn);
    NTSTATUS RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology);
    NTSTATUS RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality);
    NTSTATUS SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility);
    NTSTATUS QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps);

    NTSTATUS SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                 _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                 _Out_ UINT *pWidth,
                                 _Out_ UINT *pHeight,
                                 _Out_ D3DDDIFORMAT *pColorFormat);
    VOID SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                            _In_ UINT SourceWidth,
                            _In_ UINT SourceHeight,
                            _In_ UINT SourceStride,
                            _In_ INT PositionX,
                            _In_ INT PositionY);

    void Flip();
    static void FlipThread(void *ctx);

    // How long an armed flip may wait for its render dependency before it is
    // scanned out regardless (100ns units, 1 second).  Far beyond any
    // legitimate frame, so it only ever fires on a LOST completion -- and far
    // below the TDR budget, so a genuinely wedged GPU still surfaces as a TDR
    // instead of being hidden behind a stuttering display.
#define VIOGPU_FLIP_TOKEN_DEADLINE_100NS (10ull * 1000ull * 1000ull)

    // Scan out an armed flip if its render dependency has retired.  Called
    // from the vsync tick and, so a ready flip does not wait a whole refresh
    // period, from the present-fence wake.  Returns TRUE if it scanned out.
    BOOLEAN TryPromoteFlip();

    // Present-fence completion (DPC): a token retired, so re-evaluate an
    // armed flip.  Only wakes the flip thread; does no work at DISPATCH.
    void OnPresentTokenRetired();

    NTSTATUS SetVidPnSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress);

    // Override the source-0 scanout to an arbitrary allocation (a standing
    // dmabuf primary the UMD blits the composited frame into), independent of
    // dxgkrnl's flip. The vsync Flip thread then scans it out continuously.
    // Used by the blt-present path where dxgkrnl issues no SetVidPnSourceAddress
    // for the windowed present source.
    // Latch \p res as the scanout source.  \p addr is the allocation's
    // segment address when the caller knows it (DdiPresent's allocation
    // list); the vsync interrupt echoes it so dxgkrnl sees the display
    // progressing across flips.  Callers without an address (creation-
    // time promotion) pass {0}, which leaves the reported address alone.
    // \p requiredToken is the present-fence token whose retirement means the
    // contents of \p res are complete on the host GPU; 0 means "no dependency"
    // (boot / GDI primaries, which no present fence covers).
    void SetScanoutSource(VioGpuAllocation *res, PHYSICAL_ADDRESS addr, ULONGLONG requiredToken = 0);
    void RearmFlipIfScanout(VioGpuAllocation *res);
    inline void SetScanoutSource(VioGpuAllocation *res)
    {
        PHYSICAL_ADDRESS zero = {};
        SetScanoutSource(res, zero, 0);
    }

    // Currently-committed refresh rate, or {0,0} if no source mode is
    // pinned. Caller is responsible for choosing a default.
    D3DDDI_RATIONAL GetActiveRefreshRate() const;

  private:
    NTSTATUS SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                  CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath);
    NTSTATUS AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS AddSingleSourceMode(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                 D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    NTSTATUS AddSingleTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                 D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                 _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    D3DDDI_VIDEO_PRESENT_SOURCE_ID FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, BOOLEAN DefaultToZero);
    VOID BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo);

    // m_sourceLock is taken from PASSIVE/DISPATCH (mode set, FlipThread) and
    // also from DIRQL: with FlipCaps.FlipOnVSyncMmIo set, dxgkrnl runs
    // DdiSetVidPnSourceAddress inside dxgmms1!VidSchiExecuteMmIoFlipAtISR via
    // KeSynchronizeExecution. KeAcquireSpinLock raises to DISPATCH_LEVEL and
    // must not be called above it -- at DIRQL it hangs the CPU in
    // nt!KeAcquireSpinLockRaiseToDpc. Go through these helpers so the caller's
    // IRQL is honoured wherever the lock is taken.
    __forceinline KIRQL AcquireSourceLock()
    {
        KIRQL irql = KeGetCurrentIrql();
        if (irql >= DISPATCH_LEVEL)
        {
            KeAcquireSpinLockAtDpcLevel(&m_sourceLock);
            return irql;
        }
        KeAcquireSpinLock(&m_sourceLock, &irql);
        return irql;
    }

    __forceinline void ReleaseSourceLock(KIRQL irql)
    {
        if (irql >= DISPATCH_LEVEL)
        {
            KeReleaseSpinLockFromDpcLevel(&m_sourceLock);
        }
        else
        {
            KeReleaseSpinLock(&m_sourceLock, irql);
        }
    }

    VioGpuAdapter *m_pAdapter;
    DXGKRNL_INTERFACE *m_pDxgkInterface;

    CURRENT_MODE m_CurrentModes[MAX_VIEWS];

    PVIDEO_MODE_INFORMATION m_ModeInfo;
    ULONG m_ModeCount;
    PUSHORT m_ModeNumbers;
    USHORT m_CurrentModeIndex;
    USHORT m_CustomModeIndex;
    BYTE m_EDIDs[MAX_CHILDREN][EDID_RAW_BLOCK_SIZE];
    BOOLEAN m_bEDID;

    DXGK_DISPLAY_INFORMATION m_SystemDisplayInfo;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID m_SystemDisplaySourceId;

    VioGpuObj *m_pFrameBuf;

    PHYSICAL_ADDRESS m_sourceAddress = {0};
    VioGpuAllocation *m_sourceRes = NULL;
    KSPIN_LOCK m_sourceLock;
    volatile LONG m_shouldFlip = 0;

    // Render->flip ordering.  The UMD renders through its OWN kernel context
    // (D3DKMTCreateContext in the neptune transport) while the flip arrives on
    // the runtime's context, so dxgkrnl cannot order the flip behind the
    // render -- it never saw the render.  The UMD therefore submits a
    // GPU-completion fence (VIOGPU_SUBMIT_PRESENT_FENCE) per present, the
    // adapter stamps it with a monotonic token paired to the flip by arming
    // thread (viogpu_adapter.h), and the flip may not scan out until its
    // token retires.
    //
    // Pending flips form a small QUEUE, not a single coalescing latch.  A
    // pipelined app (DXGI MaximumFrameLatency is 3) presents flip N+1 --
    // whose render is still in flight -- BEFORE flip N's token retires, so a
    // single latch always holds a not-yet-retired dependency and the scanout
    // livelocks parked while the app renders at full speed.  With a queue the
    // promote scans out the NEWEST entry whose token HAS retired and drops
    // the older ones: standard sync-interval-0 frame-dropping, display shows
    // the latest completed frame while newer ones render.  Each entry keeps
    // the arm time of ITS OWN flip, so the lost-completion deadline
    // genuinely accrues instead of being reset by every new present.
    //
    // All under m_sourceLock.  Entries hold a reference to Res.
    struct PENDING_FLIP
    {
        VioGpuAllocation *Res;
        ULONGLONG Token;
        ULONGLONG ArmedAt;
    };
#define VIOGPU_FLIP_QUEUE_DEPTH 8
    PENDING_FLIP m_flipQueue[VIOGPU_FLIP_QUEUE_DEPTH] = {};
    ULONG m_flipQHead = 0;  // index of the oldest entry
    ULONG m_flipQCount = 0;

    // Drop every queued entry (deferred release -- callers may hold the lock
    // at DIRQL).  Used when a no-dependency latch (boot/GDI primary)
    // supersedes the queued flips and at teardown.
    void FlipQueueClearLocked();

    // What is ACTUALLY on screen, as opposed to what has been latched.
    // dxgkrnl retires a queued flip -- and frees the primary it displaced --
    // when a vsync reports that flip's address, so reporting a latched but
    // not-yet-scanned-out address would hand a buffer back while it is still
    // the one being displayed.  Only Flip() promotes latched -> displayed,
    // and only after the scanout has been emitted.
    PHYSICAL_ADDRESS m_displayedAddress = {0};

    // Signalled from the present-fence completion DPC so a flip whose token
    // just retired scans out immediately instead of waiting for the next
    // vsync tick (which would add up to a full refresh period of latency).
    KEVENT m_flipReadyEvent;

    // Diagnostics for the bounded fallback: a flip promoted without its token
    // means a completion was lost.  Must stay zero in healthy operation.
    volatile LONG m_flipTokenTimeouts = 0;

    PETHREAD m_pFlipThread;
    BOOL m_shouldFlipStop = false;
};

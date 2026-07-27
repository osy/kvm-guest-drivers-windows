#include "viogpu_vidpn.h"
#include "viogpu_adapter.h"
#include "bitops.h"
#include "baseobj.h"
#include "edid.h"
#include "trace.h"

static const LONGLONG kVsyncPeriodFallback100ns = 166666LL; // 60 Hz

// Convert a D3DDDI_RATIONAL refresh rate into a 100ns timer period.
// Falls back to 60 Hz for missing or out-of-range inputs so callers
// always receive a usable value.
static LONGLONG VsyncPeriodFromRefresh(D3DDDI_RATIONAL refresh)
{
    if (refresh.Denominator == 0 || refresh.Numerator == 0)
    {
        return kVsyncPeriodFallback100ns;
    }
    // 10,000,000 100ns ticks per second; period = denom / numer.
    LONGLONG num = 10000000LL * (LONGLONG)refresh.Denominator;
    LONGLONG den = (LONGLONG)refresh.Numerator;
    LONGLONG period = (num + den / 2) / den;
    if (period < 10000LL || period > 1000000LL) // clamp 1000Hz .. 10Hz
    {
        return kVsyncPeriodFallback100ns;
    }
    return period;
}

PAGED_CODE_SEG_BEGIN

VioGpuVidPN::VioGpuVidPN(VioGpuAdapter *adapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    m_pAdapter = adapter;
    m_pDxgkInterface = adapter->GetDxgkInterface();

    RtlZeroMemory(m_CurrentModes, sizeof(m_CurrentModes));
    RtlZeroMemory(&m_SystemDisplayInfo, sizeof(m_SystemDisplayInfo));
    m_ModeInfo = NULL;
    m_ModeCount = 0;
    m_ModeNumbers = NULL;
    m_CurrentModeIndex = 0;
    m_CustomModeIndex = 0;
    m_pFrameBuf = NULL;

    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;
    KeInitializeSpinLock(&m_sourceLock);
    // Auto-reset: each retiring present fence is one promotion opportunity,
    // and TryPromoteFlip re-checks the latch anyway, so a coalesced signal
    // can never strand an armed flip.
    KeInitializeEvent(&m_flipReadyEvent, SynchronizationEvent, FALSE);
    // Auto-reset periodic tick; armed by the flip thread (see FlipThread for
    // why the vsync cadence must not come from a wait timeout).
    KeInitializeTimerEx(&m_vsyncTimer, SynchronizationTimer);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
}

VioGpuVidPN::~VioGpuVidPN()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    DestroyFrameBufferObj(TRUE);

    if (m_sourceRes)
    {
        m_sourceRes->Release();
        m_sourceRes = NULL;
    }

    delete[] m_ModeInfo;
    delete[] m_ModeNumbers;

    m_CurrentModeIndex = 0;
    m_CustomModeIndex = 0;
    m_ModeCount = 0;

    m_ModeInfo = NULL;
    m_ModeNumbers = NULL;

    m_shouldFlipStop = true;
    KeSetEvent(&m_flipReadyEvent, IO_NO_INCREMENT, FALSE);

    KeWaitForSingleObject(m_pFlipThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(m_pFlipThread);

    KIRQL oldIrql = AcquireSourceLock();
    FlipQueueClearLocked();
    ReleaseSourceLock(oldIrql);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
}

NTSTATUS VioGpuVidPN::Start(ULONG *pNumberOfViews, ULONG *pNumberOfChildren)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    RtlZeroMemory(m_CurrentModes, sizeof(m_CurrentModes));
    m_CurrentModes[0].DispInfo.TargetId = D3DDDI_ID_UNINITIALIZED;

    NTSTATUS Status = GetModeList(&m_CurrentModes[0].DispInfo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s GetModeList failed with %x\n", __FUNCTION__, Status));
        VioGpuDbgBreak();
    }

    if (m_pAdapter->IsVgaDevice())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s a VGA device\n", __FUNCTION__));
        Status = AcquirePostDisplayOwnership();
        if (!NT_SUCCESS(Status))
        {
            return STATUS_UNSUCCESSFUL;
        }
    } else {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s NOT a VGA device\n", __FUNCTION__));
    }

    DbgPrint(TRACE_LEVEL_FATAL,
             ("%s DxgkCbAcquirePostDisplayOwnership Width = %d Height = %d Pitch = %d ColorFormat = %d\n",
              __FUNCTION__,
              m_SystemDisplayInfo.Width,
              m_SystemDisplayInfo.Height,
              m_SystemDisplayInfo.Pitch,
              m_SystemDisplayInfo.ColorFormat));

    if (m_SystemDisplayInfo.Width == 0)
    {
        m_SystemDisplayInfo.Width = NOM_WIDTH_SIZE;
        m_SystemDisplayInfo.Height = NOM_HEIGHT_SIZE;
        m_SystemDisplayInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
        m_SystemDisplayInfo.Pitch = (BPPFromPixelFormat(m_SystemDisplayInfo.ColorFormat) / BITS_PER_BYTE) *
                                    m_SystemDisplayInfo.Width;
        m_SystemDisplayInfo.TargetId = 0;
        if (m_SystemDisplayInfo.PhysicAddress.QuadPart == 0LL)
        {
            m_SystemDisplayInfo.PhysicAddress = m_pAdapter->GetFrameBufferPA();
        }
    }

    m_CurrentModes[0].DispInfo.Width = max(MIN_WIDTH_SIZE, m_SystemDisplayInfo.Width);
    m_CurrentModes[0].DispInfo.Height = max(MIN_HEIGHT_SIZE, m_SystemDisplayInfo.Height);
    m_CurrentModes[0].DispInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
    m_CurrentModes[0].DispInfo.Pitch = (BPPFromPixelFormat(m_CurrentModes[0].DispInfo.ColorFormat) / BITS_PER_BYTE) *
                                       m_CurrentModes[0].DispInfo.Width;
    m_CurrentModes[0].DispInfo.TargetId = 0;
    if (m_CurrentModes[0].DispInfo.PhysicAddress.QuadPart == 0LL && m_SystemDisplayInfo.PhysicAddress.QuadPart != 0LL)
    {
        m_CurrentModes[0].DispInfo.PhysicAddress = m_SystemDisplayInfo.PhysicAddress;
    }

#if 1
    *pNumberOfViews = MAX_VIEWS;
    *pNumberOfChildren = MAX_CHILDREN;
#else
    *pNumberOfViews = 0;
    *pNumberOfChildren = 0;
#endif

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s ColorFormat = %d\n", __FUNCTION__, m_CurrentModes[0].DispInfo.ColorFormat));

    HANDLE threadHandle = 0;
    m_shouldFlipStop = false;
    Status = PsCreateSystemThread(&threadHandle, (ACCESS_MASK)0, NULL, (HANDLE)0, NULL, VioGpuVidPN::FlipThread, this);

    ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)(&m_pFlipThread), NULL);

    ZwClose(threadHandle);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return Status;
}

NTSTATUS VioGpuVidPN::AcquirePostDisplayOwnership()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    NTSTATUS Status = m_pDxgkInterface->DxgkCbAcquirePostDisplayOwnership(m_pDxgkInterface->DeviceHandle,
                                                                          &m_SystemDisplayInfo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s DxgkCbAcquirePostDisplayOwnership failed with status 0x%X Width = %d\n",
                  __FUNCTION__,
                  Status,
                  m_SystemDisplayInfo.Width));
        VioGpuDbgBreak();
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    return Status;
}

void VioGpuVidPN::ReleasePostDisplayOwnership(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                              DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = FindSourceForTarget(TargetId, TRUE);
    m_sourceAddress.QuadPart = 0;

    LARGE_INTEGER timeout = {0};
    timeout.QuadPart = Int32x32To64(1000, -10000);

    m_shouldFlipStop = TRUE;
    KeSetEvent(&m_flipReadyEvent, IO_NO_INCREMENT, FALSE);

    if (KeWaitForSingleObject(m_pFlipThread, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("---> Failed to exit the flip thread\n"));
        VioGpuDbgBreak();
    }

    ObDereferenceObject(m_pFlipThread);

    BlackOutScreen(&m_CurrentModes[SourceId]);
    DestroyFrameBufferObj(TRUE);

    DbgPrint(TRACE_LEVEL_FATAL,
             ("%s StopDeviceAndReleasePostDisplayOwnership Width = %d Height = %d Pitch = %d ColorFormat = %dn",
              __FUNCTION__,
              m_SystemDisplayInfo.Width,
              m_SystemDisplayInfo.Height,
              m_SystemDisplayInfo.Pitch,
              m_SystemDisplayInfo.ColorFormat));

    *pDisplayInfo = m_SystemDisplayInfo;
    pDisplayInfo->TargetId = TargetId;
    pDisplayInfo->AcpiId = m_CurrentModes[0].DispInfo.AcpiId;
}

void VioGpuVidPN::Powerdown()
{
    DestroyFrameBufferObj(TRUE);
    m_CurrentModes[0].Flags.FrameBufferIsActive = FALSE;
    m_CurrentModes[0].FrameBuffer.Ptr = NULL;
    m_shouldFlipStop = true;
}

NTSTATUS VioGpuVidPN::CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pCommitVidPn != NULL);
    VIOGPU_ASSERT(pCommitVidPn->AffectedVidPnSourceId < MAX_VIEWS);

    NTSTATUS Status;
    SIZE_T NumPaths = 0;
    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE *pPinnedVidPnSourceModeInfo = NULL;

    if (pCommitVidPn->Flags.PathPoweredOff)
    {
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = m_pDxgkInterface->DxgkCbQueryVidPnInterface(pCommitVidPn->hFunctionalVidPn,
                                                         DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                         &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
        goto CommitVidPnExit;
    }

    Status = pVidPnInterface->pfnGetTopology(pCommitVidPn->hFunctionalVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
        goto CommitVidPnExit;
    }

    Status = pVidPnTopologyInterface->pfnGetNumPaths(hVidPnTopology, &NumPaths);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetNumPaths failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        goto CommitVidPnExit;
    }

    if (NumPaths != 0)
    {
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pCommitVidPn->hFunctionalVidPn,
                                                          pCommitVidPn->AffectedVidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireSourceModeSet failed with Status = 0x%X, hFunctionalVidPn = 0x%llu, SourceId = "
                      "0x%I64x\n",
                      Status,
                      LONG_PTR(pCommitVidPn->hFunctionalVidPn),
                      pCommitVidPn->AffectedVidPnSourceId));
            goto CommitVidPnExit;
        }

        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                      Status,
                      LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
            goto CommitVidPnExit;
        }
    }
    else
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s no vidpn paths found", __FUNCTION__));
        pPinnedVidPnSourceModeInfo = NULL;
    }

    if (pPinnedVidPnSourceModeInfo == NULL)
    {
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = IsVidPnSourceModeFieldsValid(pPinnedVidPnSourceModeInfo);
    if (!NT_SUCCESS(Status))
    {
        goto CommitVidPnExit;
    }

    SIZE_T NumPathsFromSource = 0;
    Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology,
                                                               pCommitVidPn->AffectedVidPnSourceId,
                                                               &NumPathsFromSource);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetNumPathsFromSource failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        goto CommitVidPnExit;
    }

    for (SIZE_T PathIndex = 0; PathIndex < NumPathsFromSource; ++PathIndex)
    {
        D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId = D3DDDI_ID_UNINITIALIZED;
        Status = pVidPnTopologyInterface->pfnEnumPathTargetsFromSource(hVidPnTopology,
                                                                       pCommitVidPn->AffectedVidPnSourceId,
                                                                       PathIndex,
                                                                       &TargetId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnEnumPathTargetsFromSource failed with Status = 0x%X, hVidPnTopology = 0x%llu, SourceId = "
                      "0x%I64x, PathIndex = 0x%I64x\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pCommitVidPn->AffectedVidPnSourceId,
                      PathIndex));
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnAcquirePathInfo(hVidPnTopology,
                                                             pCommitVidPn->AffectedVidPnSourceId,
                                                             TargetId,
                                                             &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, SourceId = 0x%I64x, "
                      "TargetId = 0x%I64x\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pCommitVidPn->AffectedVidPnSourceId,
                      TargetId));
            goto CommitVidPnExit;
        }

        Status = IsVidPnPathFieldsValid(pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = SetSourceModeAndPath(pPinnedVidPnSourceModeInfo, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnReleasePathInfo failed with Status = 0x%X, hVidPnTopoogy = 0x%llu, pVidPnPresentPath = %p\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPath));
            goto CommitVidPnExit;
        }
        pVidPnPresentPath = NULL;
    }

CommitVidPnExit:

    NTSTATUS TempStatus = STATUS_SUCCESS;

    if ((pVidPnSourceModeSetInterface != NULL) && (hVidPnSourceModeSet != 0) && (pPinnedVidPnSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pPinnedVidPnSourceModeInfo);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnInterface != NULL) && (pCommitVidPn->hFunctionalVidPn != 0) && (hVidPnSourceModeSet != 0))
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pCommitVidPn->hFunctionalVidPn, hVidPnSourceModeSet);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTopologyInterface != NULL) && (hVidPnTopology != 0) && (pVidPnPresentPath != NULL))
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return Status;
}

NTSTATUS VioGpuVidPN::SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                           CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;

    CURRENT_MODE *pCurrentMode = &m_CurrentModes[pPath->VidPnSourceId];
    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s (%dx%d)\n",
              __FUNCTION__,
              pSourceMode->Format.Graphics.VisibleRegionSize.cx,
              pSourceMode->Format.Graphics.VisibleRegionSize.cy));
    pCurrentMode->Scaling = pPath->ContentTransformation.Scaling;
    pCurrentMode->SrcModeWidth = pSourceMode->Format.Graphics.VisibleRegionSize.cx;
    pCurrentMode->SrcModeHeight = pSourceMode->Format.Graphics.VisibleRegionSize.cy;
    pCurrentMode->Rotation = pPath->ContentTransformation.Rotation;

    pCurrentMode->DispInfo.Width = pSourceMode->Format.Graphics.PrimSurfSize.cx;
    pCurrentMode->DispInfo.Height = pSourceMode->Format.Graphics.PrimSurfSize.cy;
    pCurrentMode->DispInfo.Pitch = pSourceMode->Format.Graphics.PrimSurfSize.cx *
                                   BPPFromPixelFormat(pCurrentMode->DispInfo.ColorFormat) / BITS_PER_BYTE;

    if (NT_SUCCESS(Status))
    {
        pCurrentMode->Flags.FullscreenPresent = TRUE;
        for (USHORT ModeIndex = 0; ModeIndex < GetModeCount(); ++ModeIndex)
        {
            PVIDEO_MODE_INFORMATION pModeInfo = &m_ModeInfo[ModeIndex];
            if (pCurrentMode->DispInfo.Width == pModeInfo->VisScreenWidth &&
                pCurrentMode->DispInfo.Height == pModeInfo->VisScreenHeight)
            {
                Status = SetCurrentMode(m_ModeNumbers[ModeIndex], pCurrentMode);
                if (NT_SUCCESS(Status))
                {
                    m_CurrentModeIndex = ModeIndex;
                }
                break;
            }
        }
    }

    return Status;
}

NTSTATUS VioGpuVidPN::IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pPath->VidPnSourceId >= MAX_VIEWS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("VidPnSourceId is 0x%I64x is too high (MAX_VIEWS is 0x%I64x)", pPath->VidPnSourceId, MAX_VIEWS));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    else if (pPath->VidPnTargetId >= MAX_CHILDREN)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("VidPnTargetId is 0x%I64x is too high (MAX_CHILDREN is 0x%I64x)",
                  pPath->VidPnTargetId,
                  MAX_CHILDREN));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }
    else if (pPath->GammaRamp.Type != D3DDDI_GAMMARAMP_DEFAULT)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pPath contains a gamma ramp (0x%I64x)", pPath->GammaRamp.Type));
        return STATUS_GRAPHICS_GAMMA_RAMP_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_IDENTITY) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_CENTERED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPath contains a non-identity scaling (0x%I64x)", pPath->ContentTransformation.Scaling));
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_ROTATE90) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPath contains a not-supported rotation (0x%I64x)", pPath->ContentTransformation.Rotation));
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->VidPnTargetColorBasis != D3DKMDT_CB_SCRGB) &&
             (pPath->VidPnTargetColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pPath has a non-linear RGB color basis (0x%I64x)", pPath->VidPnTargetColorBasis));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pSourceMode->Type != D3DKMDT_RMT_GRAPHICS)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pSourceMode is a non-graphics mode (0x%I64x)", pSourceMode->Type));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if ((pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_SCRGB) &&
             (pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pSourceMode has a non-linear RGB color basis (0x%I64x)", pSourceMode->Format.Graphics.ColorBasis));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if (pSourceMode->Format.Graphics.PixelValueAccessMode != D3DKMDT_PVAM_DIRECT)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pSourceMode has a palettized access mode (0x%I64x)",
                  pSourceMode->Format.Graphics.PixelValueAccessMode));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else
    {
        // Source modes are offered in both channel orders so DXGI mode
        // enumeration works for R8G8B8A8 (28/29) swapchains as well as
        // B8G8R8A8 (87/91); both scan out identically from the host.
        if (pSourceMode->Format.Graphics.PixelFormat == D3DDDIFMT_A8R8G8B8 ||
            pSourceMode->Format.Graphics.PixelFormat == D3DDDIFMT_A8B8G8R8)
        {
            return STATUS_SUCCESS;
        }
    }

    DbgPrint(TRACE_LEVEL_ERROR,
             ("pSourceMode has an unknown pixel format (0x%I64x)", pSourceMode->Format.Graphics.PixelFormat));

    return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
}

NTSTATUS
VioGpuVidPN::UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pUpdateActiveVidPnPresentPath != NULL);

    NTSTATUS Status = IsVidPnPathFieldsValid(&(pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    m_CurrentModes[pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId].Flags.FullscreenPresent = TRUE;

    m_CurrentModes[pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId].Rotation = pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.ContentTransformation.Rotation;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::GetModeList(DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    NTSTATUS Status = STATUS_SUCCESS;
    UINT ModeCount = 0;
    delete[] m_ModeInfo;
    delete[] m_ModeNumbers;
    m_ModeInfo = NULL;
    m_ModeNumbers = NULL;

    ModeCount = ProcessEdid();

    ModeCount++;

    m_ModeInfo = new (PagedPool) VIDEO_MODE_INFORMATION[ModeCount];
    if (!m_ModeInfo)
    {
        Status = STATUS_NO_MEMORY;
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpuAdapter::GetModeList failed to allocate m_ModeInfo memory\n"));
        return Status;
    }
    RtlZeroMemory(m_ModeInfo, sizeof(VIDEO_MODE_INFORMATION) * ModeCount);

    m_ModeNumbers = new (PagedPool) USHORT[ModeCount];
    if (!m_ModeNumbers)
    {
        Status = STATUS_NO_MEMORY;
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpuAdapter::GetModeList failed to allocate m_ModeNumbers memory\n"));
        return Status;
    }
    RtlZeroMemory(m_ModeNumbers, sizeof(USHORT) * ModeCount);

    m_CurrentModeIndex = 0;
    DbgPrint(TRACE_LEVEL_INFORMATION, ("m_ModeInfo = 0x%p, m_ModeNumbers = 0x%p\n", m_ModeInfo, m_ModeNumbers));

    pDispInfo->Height = max(pDispInfo->Height, MIN_HEIGHT_SIZE);
    pDispInfo->Width = max(pDispInfo->Width, MIN_WIDTH_SIZE);
    pDispInfo->ColorFormat = D3DDDIFMT_X8R8G8B8;
    pDispInfo->Pitch = (BPPFromPixelFormat(pDispInfo->ColorFormat) / BITS_PER_BYTE) * pDispInfo->Width;

    for (USHORT indx = 0; indx < ModeCount - 1; indx++)
    {

        PVIOGPU_DISP_MODE pModeInfo = &gpu_disp_modes[indx];

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s: modes[%d] x_res = %d, y_res = %d\n",
                  __FUNCTION__,
                  indx,
                  pModeInfo->XResolution,
                  pModeInfo->YResolution));

        m_ModeNumbers[indx] = indx;
        SetVideoModeInfo(indx, pModeInfo);
        if (pModeInfo->XResolution == NOM_WIDTH_SIZE && pModeInfo->YResolution == NOM_HEIGHT_SIZE)
        {
            m_CurrentModeIndex = indx;
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s: modes[%d] x_res = %d, y_res = %d\n",
                      __FUNCTION__,
                      m_CurrentModeIndex,
                      pModeInfo->XResolution,
                      pModeInfo->YResolution));
        }
    }

    m_CustomModeIndex = (USHORT)(ModeCount - 1);

    m_ModeNumbers[m_CustomModeIndex] = m_CustomModeIndex;
    memcpy(&m_ModeInfo[m_CustomModeIndex], &m_ModeInfo[m_CurrentModeIndex], sizeof(VIDEO_MODE_INFORMATION));

    m_ModeCount = ModeCount;
    DbgPrint(TRACE_LEVEL_INFORMATION, ("ModeCount filtered %d\n", m_ModeCount));

    GetDisplayInfo();

    for (UINT idx = 0; idx < ModeCount; idx++)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("type %d, XRes = %d, YRes = %d\n",
                  m_ModeNumbers[idx],
                  m_ModeInfo[idx].VisScreenWidth,
                  m_ModeInfo[idx].VisScreenHeight));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuVidPN::SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s: Mode = %d\n", __FUNCTION__, Mode));
    for (ULONG idx = 0; idx < GetModeCount(); idx++)
    {
        if (Mode == m_ModeNumbers[idx])
        {
            if (pCurrentMode->Flags.FrameBufferIsActive)
            {
                DestroyFrameBufferObj(FALSE);
                pCurrentMode->Flags.FrameBufferIsActive = FALSE;
            }
            CreateFrameBufferObj(&m_ModeInfo[idx], pCurrentMode);
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s device: setting current mode %d (%d x %d)\n",
                      __FUNCTION__,
                      Mode,
                      m_ModeInfo[idx].VisScreenWidth,
                      m_ModeInfo[idx].VisScreenHeight));
            return STATUS_SUCCESS;
        }
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed\n", __FUNCTION__));
    return STATUS_UNSUCCESSFUL;
}

void VioGpuVidPN::CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode)
{
    UINT resid, format, size;
    VioGpuObj *obj;
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s : (%d x %d)\n", __FUNCTION__, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight));
    ASSERT(m_pFrameBuf == NULL);
    size = pModeInfo->ScreenStride * pModeInfo->VisScreenHeight;
    format = ColorFormat(pCurrentMode->DispInfo.ColorFormat);
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - (%d -> %d)\n", __FUNCTION__, pCurrentMode->DispInfo.ColorFormat, format));
    resid = m_pAdapter->resourceIdr.GetId();
    m_pAdapter->ctrlQueue.CreateResource(resid, format, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight);
    obj = new (NonPagedPoolNx) VioGpuObj();
    if (!obj->Init(size, &m_pAdapter->frameSegment))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to init obj size = %d\n", __FUNCTION__, size));
        delete obj;
        return;
    }

    GpuObjectAttach(resid, obj);
    // long* pvAddr = (long*)m_FrameSegment.GetVirtualAddress();
    // for (int i = 0; i < 0x8000 / 4; i += 1) {
    //     pvAddr[i] = 0x00ff8800;
    // };
    resid = 1;

    m_pFrameBuf = obj;
    pCurrentMode->FrameBuffer.Ptr = obj->GetVirtualAddress();
    pCurrentMode->Flags.FrameBufferIsActive = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void VioGpuVidPN::DestroyFrameBufferObj(BOOLEAN bReset)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UINT resid = 0;

    if (m_pFrameBuf != NULL)
    {
        resid = (UINT)m_pFrameBuf->GetId();
        m_pAdapter->ctrlQueue.DetachBacking(resid);
        m_pAdapter->ctrlQueue.DestroyResource(resid, NotifyResourceDestroyed, &m_pAdapter->resourceIdr);
        if (bReset == TRUE)
        {
            m_pAdapter->ctrlQueue.SetScanout(0, 0, 0, 0, 0, 0);
        }
        delete m_pFrameBuf;
        m_pFrameBuf = NULL;
        // m_pAdapter->resourceIdr.PutId(resid);
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuVidPN::GpuObjectAttach(UINT res_id, VioGpuObj *obj)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_MEM_ENTRY ents = NULL;
    PSCATTER_GATHER_LIST sgl = NULL;
    UINT size = 0;
    sgl = obj->GetSGList();
    size = sizeof(GPU_MEM_ENTRY) * sgl->NumberOfElements;
    ents = reinterpret_cast<PGPU_MEM_ENTRY>(new (NonPagedPoolNx) BYTE[size]);

    if (!ents)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("<--- %s cannot allocate memory %x bytes numberofentries = %d\n",
                  __FUNCTION__,
                  size,
                  sgl->NumberOfElements));
        return FALSE;
    }
    // FIXME
    RtlZeroMemory(ents, size);

    for (UINT i = 0; i < sgl->NumberOfElements; i++)
    {
        ents[i].addr = sgl->Elements[i].Address.QuadPart;
        ents[i].length = sgl->Elements[i].Length;
        ents[i].padding = 0;
    }

    m_pAdapter->ctrlQueue.AttachBacking(res_id, ents, sgl->NumberOfElements);
    obj->SetId(res_id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

NTSTATUS VioGpuVidPN::EscapeCustomResoulution(VIOGPU_DISP_MODE *resolution)
{
    PAGED_CODE();

    resolution->XResolution = (USHORT)m_ModeInfo[m_CustomModeIndex].VisScreenWidth;
    resolution->YResolution = (USHORT)m_ModeInfo[m_CustomModeIndex].VisScreenHeight;

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pVidPnHWCaps != NULL);
    VIOGPU_ASSERT(pVidPnHWCaps->SourceId < MAX_VIEWS);
    VIOGPU_ASSERT(pVidPnHWCaps->TargetId < MAX_CHILDREN);

    pVidPnHWCaps->VidPnHWCaps.DriverRotation = 1;
    pVidPnHWCaps->VidPnHWCaps.DriverScaling = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverCloning = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverColorConvert = 1;
    pVidPnHWCaps->VidPnHWCaps.DriverLinkedAdapaterOutput = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverRemoteDisplay = 0;
    pVidPnHWCaps->VidPnHWCaps.Reserved = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pIsSupportedVidPn != NULL);

    if (pIsSupportedVidPn->hDesiredVidPn == 0)
    {
        pIsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }

    pIsSupportedVidPn->IsVidPnSupported = FALSE;
    // DbgPrint(TRACE_LEVEL_ERROR, ("vidpn %d\n", pIsSupportedVidPn->hDesiredVidPn));
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface;
    NTSTATUS Status = m_pDxgkInterface->DxgkCbQueryVidPnInterface(pIsSupportedVidPn->hDesiredVidPn,
                                                                  DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                                  &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hDesiredVidPn = %llu\n",
                  Status,
                  LONG_PTR(pIsSupportedVidPn->hDesiredVidPn)));
        return Status;
    }

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface;
    Status = pVidPnInterface->pfnGetTopology(pIsSupportedVidPn->hDesiredVidPn,
                                             &hVidPnTopology,
                                             &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hDesiredVidPn = %llu\n",
                  Status,
                  LONG_PTR(pIsSupportedVidPn->hDesiredVidPn)));
        return Status;
    }

    for (D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = 0; SourceId < MAX_VIEWS; ++SourceId)
    {
        SIZE_T NumPathsFromSource = 0;
        Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology, SourceId, &NumPathsFromSource);
        if (Status == STATUS_GRAPHICS_SOURCE_NOT_IN_TOPOLOGY)
        {
            continue;
        }
        else if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnGetNumPathsFromSource failed with Status = 0x%X hVidPnTopology = %llu, SourceId = %llu",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      LONG_PTR(SourceId)));
            return Status;
        }
        else if (NumPathsFromSource > MAX_CHILDREN)
        {
            return STATUS_SUCCESS;
        }
    }

    pIsSupportedVidPn->IsVidPnSupported = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpuVidPN::RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pRecommendFunctionalVidPn == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS VioGpuVidPN::RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pRecommendVidPnTopology == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS VioGpuVidPN::RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return AddSingleMonitorMode(pRecommendMonitorModes);
}

NTSTATUS VioGpuVidPN::AddSingleSourceMode(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                          D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                          D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(SourceId);

    for (ULONG idx = 0; idx < GetModeCount(); ++idx)
    {
        D3DKMDT_VIDPN_SOURCE_MODE *pVidPnSourceModeInfo = NULL;
        PVIDEO_MODE_INFORMATION pModeInfo = &m_ModeInfo[idx];
        NTSTATUS Status = pVidPnSourceModeSetInterface->pfnCreateNewModeInfo(hVidPnSourceModeSet,
                                                                             &pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = %llu",
                      Status,
                      LONG_PTR(hVidPnSourceModeSet)));
            return Status;
        }

        pVidPnSourceModeInfo->Type = D3DKMDT_RMT_GRAPHICS;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx = pModeInfo->VisScreenWidth;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy = pModeInfo->VisScreenHeight;
        pVidPnSourceModeInfo->Format.Graphics.VisibleRegionSize = pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize;
        pVidPnSourceModeInfo->Format.Graphics.Stride = pModeInfo->ScreenStride;
        pVidPnSourceModeInfo->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
        pVidPnSourceModeInfo->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        pVidPnSourceModeInfo->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

        Status = pVidPnSourceModeSetInterface->pfnAddMode(hVidPnSourceModeSet, pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            NTSTATUS TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet,
                                                                                   pVidPnSourceModeInfo);
            UNREFERENCED_PARAMETER(TempStatus);
            NT_ASSERT(NT_SUCCESS(TempStatus));

            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hVidPnSourceModeSet = %llu, pVidPnSourceModeInfo = %p",
                          Status,
                          LONG_PTR(hVidPnSourceModeSet),
                          pVidPnSourceModeInfo));
                return Status;
            }
        }

        // Second entry per resolution with the opposite channel order so
        // DXGI GetDisplayModeList(R8G8B8A8[_SRGB]) is non-empty; 3DMark's
        // backbuffer format is 29 (R8G8B8A8_UNORM_SRGB) and it aborts with
        // "103 Display mode list not found" on an empty list. One
        // A8B8G8R8 source mode yields both the UNORM(28) and SRGB(29) DXGI
        // entries, exactly as A8R8G8B8 yields 87 and 91.
        Status = pVidPnSourceModeSetInterface->pfnCreateNewModeInfo(hVidPnSourceModeSet, &pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo (ABGR) failed with Status = 0x%X, hVidPnSourceModeSet = %llu",
                      Status, LONG_PTR(hVidPnSourceModeSet)));
            return Status;
        }
        pVidPnSourceModeInfo->Type = D3DKMDT_RMT_GRAPHICS;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx = pModeInfo->VisScreenWidth;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy = pModeInfo->VisScreenHeight;
        pVidPnSourceModeInfo->Format.Graphics.VisibleRegionSize = pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize;
        pVidPnSourceModeInfo->Format.Graphics.Stride = pModeInfo->ScreenStride;
        pVidPnSourceModeInfo->Format.Graphics.PixelFormat = D3DDDIFMT_A8B8G8R8;
        pVidPnSourceModeInfo->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        pVidPnSourceModeInfo->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

        Status = pVidPnSourceModeSetInterface->pfnAddMode(hVidPnSourceModeSet, pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            NTSTATUS TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet,
                                                                                   pVidPnSourceModeInfo);
            UNREFERENCED_PARAMETER(TempStatus);
            NT_ASSERT(NT_SUCCESS(TempStatus));

            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode (ABGR) failed with Status = 0x%X, hVidPnSourceModeSet = %llu, pVidPnSourceModeInfo = %p",
                          Status, LONG_PTR(hVidPnSourceModeSet), pVidPnSourceModeInfo));
                return Status;
            }
        }

    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

VOID VioGpuVidPN::BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo)
{
    PAGED_CODE();

    pVideoSignalInfo->VideoStandard = D3DKMDT_VSS_OTHER;
    pVideoSignalInfo->TotalSize.cx = pModeInfo->VisScreenWidth;
    pVideoSignalInfo->TotalSize.cy = pModeInfo->VisScreenHeight;

#if 1
    pVideoSignalInfo->VSyncFreq.Numerator = 148500000;
    pVideoSignalInfo->VSyncFreq.Denominator = 2475000;
    pVideoSignalInfo->HSyncFreq.Numerator = 67500;
    pVideoSignalInfo->HSyncFreq.Denominator = 1;
    pVideoSignalInfo->PixelRate = 148500000;
#else

    pVideoSignalInfo->VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
#endif
    pVideoSignalInfo->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

NTSTATUS VioGpuVidPN::AddSingleTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                          D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                          _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                          D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(SourceId);

    D3DKMDT_VIDPN_TARGET_MODE *pVidPnTargetModeInfo = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    // Cofunctionality: when dxgkrnl has already PINNED a source mode, this set
    // must contain only target modes that can actually drive it.  This adapter
    // exposes no scaler (QueryVidPnHWCapability: DriverScaling = 0) and its
    // paths carry identity/centered scaling, so exactly ONE target mode is
    // cofunctional -- the one whose raster equals the pinned source's visible
    // region.  Build it from the pinned mode rather than from m_ModeInfo, so a
    // source mode that is not in the table (the custom/agent-driven slot is
    // rewritten under us) still gets a matching target.
    VIDEO_MODE_INFORMATION PinnedMode;
    const BOOLEAN HavePinnedSource = (pVidPnPinnedSourceModeInfo != NULL);
    UINT ModeCount = (UINT)GetModeCount();

    if (HavePinnedSource)
    {
        RtlZeroMemory(&PinnedMode, sizeof(PinnedMode));
        PinnedMode.Length = sizeof(PinnedMode);
        PinnedMode.VisScreenWidth = pVidPnPinnedSourceModeInfo->Format.Graphics.VisibleRegionSize.cx;
        PinnedMode.VisScreenHeight = pVidPnPinnedSourceModeInfo->Format.Graphics.VisibleRegionSize.cy;
        PinnedMode.ScreenStride = (PinnedMode.VisScreenWidth * 4 + 3) & ~0x3;
        ModeCount = 1;
    }

    for (UINT ModeIndex = 0; ModeIndex < ModeCount; ++ModeIndex)
    {
        PVIDEO_MODE_INFORMATION pModeInfo = HavePinnedSource ? &PinnedMode : &m_ModeInfo[ModeIndex];
        pVidPnTargetModeInfo = NULL;
        Status = pVidPnTargetModeSetInterface->pfnCreateNewModeInfo(hVidPnTargetModeSet, &pVidPnTargetModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = %llu",
                      Status,
                      LONG_PTR(hVidPnTargetModeSet)));
            return Status;
        }
        // A freshly created mode carries D3DKMDT_DIMENSION_NOTSPECIFIED, so the
        // raster has to be built before ActiveSize can be taken from TotalSize.
        BuildVideoSignalInfo(&pVidPnTargetModeInfo->VideoSignalInfo, pModeInfo);
        pVidPnTargetModeInfo->VideoSignalInfo.ActiveSize = pVidPnTargetModeInfo->VideoSignalInfo.TotalSize;

        pVidPnTargetModeInfo->Preference = D3DKMDT_MP_NOTPREFERRED; // TODO: another logic for prefferred mode. Maybe
                                                                    // the pinned source mode

        Status = pVidPnTargetModeSetInterface->pfnAddMode(hVidPnTargetModeSet, pVidPnTargetModeInfo);
        if (!NT_SUCCESS(Status))
        {
            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu, pVidPnTargetModeInfo = "
                          "%p\n",
                          Status,
                          LONG_PTR(hVidPnTargetModeSet),
                          pVidPnTargetModeInfo));
            }

            Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnTargetModeInfo);
            NT_ASSERT(NT_SUCCESS(Status));
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    D3DKMDT_MONITOR_SOURCE_MODE *pMonitorSourceMode = NULL;
    PVIDEO_MODE_INFORMATION pVbeModeInfo = NULL;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                          &pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnCreateNewModeInfo failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu\n",
                  Status,
                  LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet)));
        return Status;
    }

    pVbeModeInfo = &m_ModeInfo[m_CurrentModeIndex];

    BuildVideoSignalInfo(&pMonitorSourceMode->VideoSignalInfo, pVbeModeInfo);

    pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
    pMonitorSourceMode->Preference = D3DKMDT_MP_PREFERRED;
    pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAddMode failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu, pMonitorSourceMode = "
                      "0x%p\n",
                      Status,
                      LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet),
                      pMonitorSourceMode));
        }
        else
        {
            Status = STATUS_SUCCESS;
        }

        NTSTATUS TempStatus = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                                         pMonitorSourceMode);
        UNREFERENCED_PARAMETER(TempStatus);
        NT_ASSERT(NT_SUCCESS(TempStatus));
        return Status;
    }

    for (UINT Idx = 0; Idx < GetModeCount(); ++Idx)
    {
        pVbeModeInfo = &m_ModeInfo[Idx];

        Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                              &pMonitorSourceMode);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu\n",
                      Status,
                      LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet)));
            return Status;
        }

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s: add pref mode, dimensions %ux%u, taken from DxgkCbAcquirePostDisplayOwnership at StartDevice\n",
                  __FUNCTION__,
                  pVbeModeInfo->VisScreenWidth,
                  pVbeModeInfo->VisScreenHeight));

        BuildVideoSignalInfo(&pMonitorSourceMode->VideoSignalInfo, pVbeModeInfo);

        pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
        pMonitorSourceMode->Preference = D3DKMDT_MP_NOTPREFERRED;
        pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
        pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;

        Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                    pMonitorSourceMode);
        if (!NT_SUCCESS(Status))
        {
            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu, pMonitorSourceMode = "
                          "0x%p\n",
                          Status,
                          LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet),
                          pMonitorSourceMode));
            }

            Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                                pMonitorSourceMode);
            NT_ASSERT(NT_SUCCESS(Status));
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuVidPN::EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality)
{
    PAGED_CODE();

    // DbgBreakPoint();

    VIOGPU_ASSERT(pEnumCofuncModality != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet = 0;
    D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface = NULL;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPathTemp = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo = NULL;
    CONST D3DKMDT_VIDPN_TARGET_MODE *pVidPnPinnedTargetModeInfo = NULL;

    NTSTATUS Status = m_pDxgkInterface->DxgkCbQueryVidPnInterface(pEnumCofuncModality->hConstrainingVidPn,
                                                                  DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                                  &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
        return Status;
    }

    Status = pVidPnInterface->pfnGetTopology(pEnumCofuncModality->hConstrainingVidPn,
                                             &hVidPnTopology,
                                             &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
        return Status;
    }

    Status = pVidPnTopologyInterface->pfnAcquireFirstPathInfo(hVidPnTopology, &pVidPnPresentPath);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnAcquireFirstPathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        return Status;
    }

    while (Status != STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                          pVidPnPresentPath->VidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, SourceId = "
                      "0x%llu\n",
                      Status,
                      LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                      LONG_PTR(pVidPnPresentPath->VidPnSourceId)));
            break;
        }

        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pVidPnPinnedSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = 0x%llu\n",
                      Status,
                      LONG_PTR(hVidPnSourceModeSet)));
            break;
        }

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId)))
        {
            if (pVidPnPinnedSourceModeInfo == NULL)
            {
                Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnSourceModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnSourceModeSet)));
                    break;
                }
                hVidPnSourceModeSet = 0;

                Status = pVidPnInterface->pfnCreateNewSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnSourceId,
                                                                    &hVidPnSourceModeSet,
                                                                    &pVidPnSourceModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnCreateNewSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "SourceId = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnSourceId)));
                    break;
                }

                {
                    Status = AddSingleSourceMode(pVidPnSourceModeSetInterface,
                                                 hVidPnSourceModeSet,
                                                 pVidPnPresentPath->VidPnSourceId);
                }

                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("AddSingleSourceMode failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
                    break;
                }

                Status = pVidPnInterface->pfnAssignSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                 pVidPnPresentPath->VidPnSourceId,
                                                                 hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAssignSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, SourceId "
                              "= 0x%llu, hVidPnSourceModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnSourceId),
                              LONG_PTR(hVidPnSourceModeSet)));
                    break;
                }
                hVidPnSourceModeSet = 0;
            }
        }

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            Status = pVidPnInterface->pfnAcquireTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              pVidPnPresentPath->VidPnTargetId,
                                                              &hVidPnTargetModeSet,
                                                              &pVidPnTargetModeSetInterface);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAcquireTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, TargetId = "
                          "0x%llu\n",
                          Status,
                          LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                          LONG_PTR(pVidPnPresentPath->VidPnTargetId)));
                break;
            }

            Status = pVidPnTargetModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnTargetModeSet,
                                                                            &pVidPnPinnedTargetModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu\n",
                          Status,
                          LONG_PTR(hVidPnTargetModeSet)));
                break;
            }

            if (pVidPnPinnedTargetModeInfo == NULL)
            {
                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;

                Status = pVidPnInterface->pfnCreateNewTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnTargetId,
                                                                    &hVidPnTargetModeSet,
                                                                    &pVidPnTargetModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnCreateNewTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "TargetId = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnTargetId)));
                    break;
                }

                Status = AddSingleTargetMode(pVidPnTargetModeSetInterface,
                                             hVidPnTargetModeSet,
                                             pVidPnPinnedSourceModeInfo,
                                             pVidPnPresentPath->VidPnSourceId);

                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("AddSingleTargetMode failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
                    break;
                }

                Status = pVidPnInterface->pfnAssignTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                 pVidPnPresentPath->VidPnTargetId,
                                                                 hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAssignTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, TargetId "
                              "= 0x%llu, hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnTargetId),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;
            }
            else
            {
                Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet,
                                                                          pVidPnPinnedTargetModeInfo);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu, "
                              "pVidPnPinnedTargetModeInfo = %p\n",
                              Status,
                              LONG_PTR(hVidPnTargetModeSet),
                              pVidPnPinnedTargetModeInfo));
                    break;
                }
                pVidPnPinnedTargetModeInfo = NULL;

                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;
            }
        }

        if (pVidPnPinnedSourceModeInfo != NULL)
        {
            Status = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnReleaseModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = 0x%llu, "
                          "pVidPnPinnedSourceModeInfo = %p\n",
                          Status,
                          LONG_PTR(hVidPnSourceModeSet),
                          pVidPnPinnedSourceModeInfo));
                break;
            }
            pVidPnPinnedSourceModeInfo = NULL;
        }

        if (hVidPnSourceModeSet != 0)
        {
            Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnSourceModeSet);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnReleaseSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                          "hVidPnSourceModeSet = 0x%llu\n",
                          Status,
                          LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                          LONG_PTR(hVidPnSourceModeSet)));
                break;
            }
            hVidPnSourceModeSet = 0;
        }

        D3DKMDT_VIDPN_PRESENT_PATH LocalVidPnPresentPath = *pVidPnPresentPath;
        BOOLEAN SupportFieldsModified = FALSE;

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_SCALING) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            if (pVidPnPresentPath->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED)
            {
                RtlZeroMemory(&(LocalVidPnPresentPath.ContentTransformation.ScalingSupport),
                              sizeof(D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT));
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Centered = 1;
                SupportFieldsModified = TRUE;
            }
        }

        if (!((pEnumCofuncModality->EnumPivotType != D3DKMDT_EPT_ROTATION) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            if (pVidPnPresentPath->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)
            {
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate90 = 1;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate180 = 0;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate270 = 0;
                SupportFieldsModified = TRUE;
            }
        }

        if (SupportFieldsModified)
        {
            Status = pVidPnTopologyInterface->pfnUpdatePathSupportInfo(hVidPnTopology, &LocalVidPnPresentPath);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnUpdatePathSupportInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                          Status,
                          LONG_PTR(hVidPnTopology)));
                break;
            }
        }

        pVidPnPresentPathTemp = pVidPnPresentPath;
        Status = pVidPnTopologyInterface->pfnAcquireNextPathInfo(hVidPnTopology,
                                                                 pVidPnPresentPathTemp,
                                                                 &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireNextPathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, "
                      "pVidPnPresentPathTemp = %p\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPathTemp));
            break;
        }

        NTSTATUS TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        if (!NT_SUCCESS(TempStatus))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnReleasePathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, pVidPnPresentPathTemp = "
                      "%p\n",
                      TempStatus,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPathTemp));
            Status = TempStatus;
            break;
        }
        pVidPnPresentPathTemp = NULL;
    }

    if (Status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = STATUS_SUCCESS;
    }

    NTSTATUS TempStatus = STATUS_NOT_FOUND;

    if ((pVidPnSourceModeSetInterface != NULL) && (pVidPnPinnedSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTargetModeSetInterface != NULL) && (pVidPnPinnedTargetModeInfo != NULL))
    {
        TempStatus = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPath != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPathTemp != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnSourceModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnSourceModeSet);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnTargetModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnTargetModeSet);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    VIOGPU_ASSERT_CHK(TempStatus == STATUS_NOT_FOUND || Status != STATUS_SUCCESS);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuVidPN::SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pSetVidPnSourceVisibility != NULL);
    VIOGPU_ASSERT((pSetVidPnSourceVisibility->VidPnSourceId < MAX_VIEWS) ||
                  (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL));

    UINT StartVidPnSourceId = (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL) ? 0
                                                                                          : pSetVidPnSourceVisibility->VidPnSourceId;
    UINT MaxVidPnSourceId = (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL) ? MAX_VIEWS
                                                                                        : pSetVidPnSourceVisibility->VidPnSourceId + 1;

    for (UINT SourceId = StartVidPnSourceId; SourceId < MaxVidPnSourceId; ++SourceId)
    {
        if (pSetVidPnSourceVisibility->Visible)
        {
            m_CurrentModes[SourceId].Flags.FullscreenPresent = TRUE;
        }
        else
        {
            BlackOutScreen(&m_CurrentModes[SourceId]);
        }

        m_CurrentModes[SourceId].Flags.SourceNotVisible = !(pSetVidPnSourceVisibility->Visible);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

VOID VioGpuVidPN::BlackOutScreen(CURRENT_MODE *pCurrentMod)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));

    if (pCurrentMod->Flags.FrameBufferIsActive)
    {
        UINT ScreenHeight = pCurrentMod->DispInfo.Height;
        UINT ScreenPitch = pCurrentMod->DispInfo.Pitch;
        BYTE *pDst = (BYTE *)pCurrentMod->FrameBuffer.Ptr;

        UINT resid = 0;

        if (pDst)
        {
            RtlZeroMemory(pDst, (ULONGLONG)ScreenHeight * ScreenPitch);
        }

        // FIXME!!! rotation

        resid = m_pFrameBuf->GetId();

        // m_pAdapter->ctrlQueue.TransferToHost2D(resid, 0UL, pCurrentMod->DispInfo.Width, pCurrentMod->DispInfo.Height, 0, 0);
        m_pAdapter->ctrlQueue.ResFlush(resid, pCurrentMod->DispInfo.Width, pCurrentMod->DispInfo.Height, 0, 0);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PBYTE VioGpuVidPN::GetEdidData(UINT Id)
{
    PAGED_CODE();

    if (Id >= MAX_CHILDREN)
    {
        return NULL;
    }
    return m_bEDID ? m_EDIDs[Id] : (PBYTE)(g_gpu_edid);
}

BOOLEAN VioGpuVidPN::GetDisplayInfo(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf = NULL;
    ULONG xres = 0;
    ULONG yres = 0;

    for (UINT32 i = 0; i < m_pAdapter->m_u32NumScanouts; i++)
    {
        if (m_pAdapter->ctrlQueue.AskDisplayInfo(&vbuf))
        {
            m_pAdapter->ctrlQueue.GetDisplayInfo(vbuf, i, &xres, &yres);
            m_pAdapter->ctrlQueue.ReleaseBuffer(vbuf);
            if (xres && yres)
            {
                DbgPrint(TRACE_LEVEL_FATAL, ("---> %s (%dx%d)\n", __FUNCTION__, xres, yres));
                SetCustomDisplay((USHORT)xres, (USHORT)yres);
            }
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

int VioGpuVidPN::ProcessEdid(void)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_pAdapter->m_u64HostFeatures, VIRTIO_GPU_F_EDID))
    {
        GetEdids();
    }
    else
    {
        FixEdid();
    }
    return AddEdidModes();
}

BOOLEAN VioGpuVidPN::UpdateModes(USHORT xres, USHORT yres, int &cnt)
{
    int idx = 0;

    if ((xres < MIN_WIDTH_SIZE) || (yres < MIN_HEIGHT_SIZE))
    {
        return FALSE;
    }

    for (; idx < cnt; idx++)
    {
        if ((gpu_disp_modes[idx].XResolution == xres) && (gpu_disp_modes[idx].YResolution == yres))
        {
            return FALSE;
        }
    }
    gpu_disp_modes[idx].XResolution = xres;
    gpu_disp_modes[idx].YResolution = yres;
    cnt++;
    return TRUE;
}

void VioGpuVidPN::FixEdid(void)
{
    PAGED_CODE();

    UCHAR Sum = 0;
    PUCHAR buf = GetEdidData(0);
    ;
    PEDID_DATA_V1 pdata = (PEDID_DATA_V1)buf;
    pdata->MaximumHorizontalImageSize[0] = 0;
    pdata->MaximumVerticallImageSize[0] = 0;
    pdata->ExtensionFlag[0] = 0;
    pdata->Checksum[0] = 0;
    for (ULONG i = 0; i < EDID_V1_BLOCK_SIZE; i++)
    {
        Sum += buf[i];
    }
    pdata->Checksum[0] = -Sum;
}

PBYTE VioGpuVidPN::GetCTA861Data(void)
{
    PAGED_CODE();
    if (m_bEDID)
    {
        PEDID_DATA_V1 edid_data = (PEDID_DATA_V1)m_EDIDs;
        if (edid_data->ExtensionFlag)
        {
            // m_EDIDs is BYTE[MAX_CHILDREN][EDID_RAW_BLOCK_SIZE]; "m_EDIDs +
            // EDID_V1_BLOCK_SIZE" advances 128 ROWS (128*256 bytes past the
            // array), not 128 bytes -- wild OOB read, bugcheck 0x50 in
            // GetCTA861Data when the stray page is unmapped. The CTA-861
            // extension block sits 128 bytes into scanout 0's raw EDID.
            PEDID_CTA_861 cta_data = (PEDID_CTA_861)(m_EDIDs[0] + EDID_V1_BLOCK_SIZE);
            if (cta_data->ExtentionTag[0] >= 2 && cta_data->Revision[0] >= 3)
            {
                return (PBYTE)cta_data;
            }
        }
    }
    return NULL;
}

BOOLEAN VioGpuVidPN::GetEdids(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf = NULL;

    for (UINT32 i = 0; i < m_pAdapter->m_u32NumScanouts; i++)
    {
        if (m_pAdapter->ctrlQueue.AskEdidInfo(&vbuf, i) && m_pAdapter->ctrlQueue.GetEdidInfo(vbuf, i, m_EDIDs[i]))
        {
            m_bEDID = TRUE;
        }
        m_pAdapter->ctrlQueue.ReleaseBuffer(vbuf);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

int VioGpuVidPN::AddEdidModes(void)
{
    PAGED_CODE();
    PEDID_DATA_V1 edid_data = (PEDID_DATA_V1)(GetEdidData(0));
    ESTABLISHED_TIMINGS_1_2 est_timing_1_2 = edid_data->EstablishedTimings;
    MANUFACTURER_TIMINGS manufact_timing = edid_data->ManufacturerTimings;
    int modecount = 0;
    UpdateModes(MIN_WIDTH_SIZE, MIN_HEIGHT_SIZE, modecount);
    UpdateModes(NOM_WIDTH_SIZE, NOM_HEIGHT_SIZE, modecount);
    if (est_timing_1_2.Timing_800x600_60 || est_timing_1_2.Timing_800x600_56 || est_timing_1_2.Timing_800x600_75 ||
        est_timing_1_2.Timing_800x600_72)
    {
        UpdateModes(800, 600, modecount);
    }
    if (est_timing_1_2.Timing_720x400_88 || est_timing_1_2.Timing_720x400_70)
    {
        UpdateModes(720, 400, modecount);
    }
    if (est_timing_1_2.Timing_832x624_75)
    {
        UpdateModes(832, 624, modecount);
    }
    if (est_timing_1_2.Timing_1280x1024_75)
    {
        UpdateModes(1280, 1024, modecount);
    }
    if (manufact_timing.Timing_1152x870_75)
    {
        UpdateModes(1152, 870, modecount);
    }
    PSTANDARD_TIMING_DESCRIPTOR standard_timing = edid_data->StandardTimings;
    for (int i = 0; i < 8; i++, standard_timing++)
    {
        VIOGPU_DISP_MODE mode{0};
        if (GetStandardTimingResolution(standard_timing, &mode))
        {
            UpdateModes(mode.XResolution, mode.YResolution, modecount);
        }
    }
    if (edid_data->Revision[0] == 4)
    {
        PEDID_DETAILED_DESCRIPTOR detailed_desc = edid_data->EDIDDetailedTimings;
        for (int i = 0; i < 4; i++, detailed_desc++)
        {
            if (detailed_desc->PixelClock == 0)
            {
                PEDID_DISPLAY_DESCRIPTOR disp = (PEDID_DISPLAY_DESCRIPTOR)detailed_desc;
                if (disp->Tag[3] == 0xF7 && disp->Revision == 0xA)
                {
                    PESTABLISHED_TIMINGS_3 est_timing_3 = (PESTABLISHED_TIMINGS_3)disp->Data;
                    if (est_timing_3->Timing_640x350_85)
                    {
                        UpdateModes(640, 350, modecount);
                    }
                    if (est_timing_3->Timing_640x400_85)
                    {
                        UpdateModes(640, 400, modecount);
                    }
                    if (est_timing_3->Timing_640x480_85)
                    {
                        UpdateModes(640, 480, modecount);
                    }
                    if (est_timing_3->Timing_720x400_85)
                    {
                        UpdateModes(720, 400, modecount);
                    }
                    if (est_timing_3->Timing_800x600_85)
                    {
                        UpdateModes(800, 600, modecount);
                    }
                    if (est_timing_3->Timing_848x480_60)
                    {
                        UpdateModes(848, 480, modecount);
                    }
                    if (est_timing_3->Timing_1024x768_85)
                    {
                        UpdateModes(1024, 768, modecount);
                    }
                    if (est_timing_3->Timing_1152x864_75)
                    {
                        UpdateModes(1152, 864, modecount);
                    }
                    if (est_timing_3->Timing_1280x768_60 || est_timing_3->Timing_1280x768_60_RB ||
                        est_timing_3->Timing_1280x768_75 || est_timing_3->Timing_1280x768_85)
                    {
                        UpdateModes(1280, 768, modecount);
                    }
                    if (est_timing_3->Timing_1280x960_60 || est_timing_3->Timing_1280x960_85)
                    {
                        UpdateModes(1280, 960, modecount);
                    }
                    if (est_timing_3->Timing_1280x1024_60 || est_timing_3->Timing_1280x1024_85)
                    {
                        UpdateModes(1280, 1024, modecount);
                    }
                    if (est_timing_3->Timing_1360x768_60)
                    {
                        UpdateModes(1360, 768, modecount);
                    }
                    if (est_timing_3->Timing_1400x1050_60 || est_timing_3->Timing_1400x1050_60_RB ||
                        est_timing_3->Timing_1400x1050_75 || est_timing_3->Timing_1400x1050_85)
                    {
                        UpdateModes(1400, 1050, modecount);
                    }
                    if (est_timing_3->Timing_1440x900_60 || est_timing_3->Timing_1440x900_60_RB ||
                        est_timing_3->Timing_1440x900_75 || est_timing_3->Timing_1440x900_85)
                    {
                        UpdateModes(1440, 900, modecount);
                    }
                    if (est_timing_3->Timing_1600x1200_60 || est_timing_3->Timing_1600x1200_65 ||
                        est_timing_3->Timing_1600x1200_70 || est_timing_3->Timing_1600x1200_75 ||
                        est_timing_3->Timing_1600x1200_85)
                    {
                        UpdateModes(1600, 1200, modecount);
                    }
                    if (est_timing_3->Timing_1680x1050_60 || est_timing_3->Timing_1680x1050_60_RB ||
                        est_timing_3->Timing_1680x1050_75 || est_timing_3->Timing_1680x1050_85)
                    {
                        UpdateModes(1680, 1050, modecount);
                    }
                    if (est_timing_3->Timing_1792x1344_60 || est_timing_3->Timing_1792x1344_75)
                    {
                        UpdateModes(1792, 1344, modecount);
                    }
                    if (est_timing_3->Timing_1856x1392_60 || est_timing_3->Timing_1856x1392_75)
                    {
                        UpdateModes(1856, 1392, modecount);
                    }
                    if (est_timing_3->Timing_1920x1200_60 || est_timing_3->Timing_1920x1200_60_RB ||
                        est_timing_3->Timing_1920x1200_75 || est_timing_3->Timing_1920x1200_85)
                    {
                        UpdateModes(1920, 1200, modecount);
                    }
                    if (est_timing_3->Timing_1920x1440_60 || est_timing_3->Timing_1920x1440_75)
                    {
                        UpdateModes(1920, 1440, modecount);
                    }
                }
            }
        }
    }
    PEDID_CTA_861 cta_data = (PEDID_CTA_861)GetCTA861Data();
    if (cta_data && cta_data->DTDBegin[0] > 4)
    {
        int vics = (cta_data->DTDBegin[0] - 1) - 4;
        for (int idx = 0; idx < vics; idx++)
        {
            VIOGPU_DISP_MODE mode{0};
            USHORT vic_num = cta_data->Data[idx];
            if (GetVICResolution(vic_num, &mode))
            {
                UpdateModes(mode.XResolution, mode.YResolution, modecount);
            }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return modecount;
}

void VioGpuVidPN::SetVideoModeInfo(UINT Idx, PVIOGPU_DISP_MODE pModeInfo)
{
    PAGED_CODE();

    PVIDEO_MODE_INFORMATION pMode = NULL;

    pMode = &m_ModeInfo[Idx];
    pMode->Length = sizeof(VIDEO_MODE_INFORMATION);
    pMode->ModeIndex = Idx;
    pMode->VisScreenWidth = pModeInfo->XResolution;
    pMode->VisScreenHeight = pModeInfo->YResolution;
    pMode->ScreenStride = (pModeInfo->XResolution * 4 + 3) & ~0x3;
}

void VioGpuVidPN::SetCustomDisplay(_In_ USHORT xres, _In_ USHORT yres)
{
    PAGED_CODE();

    VIOGPU_DISP_MODE tmpModeInfo = {0};

    if (xres < MIN_WIDTH_SIZE || yres < MIN_HEIGHT_SIZE)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s: (%dx%d) less than (%dx%d)\n", __FUNCTION__, xres, yres, MIN_WIDTH_SIZE, MIN_HEIGHT_SIZE));
    }
    tmpModeInfo.XResolution = m_pAdapter->IsFlexResolution() ? xres : max(MIN_WIDTH_SIZE, xres);
    tmpModeInfo.YResolution = m_pAdapter->IsFlexResolution() ? yres : max(MIN_HEIGHT_SIZE, yres);

    m_CustomModeIndex = (USHORT)(m_ModeCount - 1);

    DbgPrint(TRACE_LEVEL_FATAL,
             ("%s - %d (%dx%d)\n", __FUNCTION__, m_CustomModeIndex, tmpModeInfo.XResolution, tmpModeInfo.YResolution));

    SetVideoModeInfo(m_CustomModeIndex, &tmpModeInfo);
}

// Scan out an armed flip once the render it depends on has retired on the host
// GPU.  Returns TRUE if a scanout was emitted (or explicitly cleared).
//
// Until the dependency retires the latch stays armed and NOTHING is published:
// in particular m_displayedAddress is left alone, so the vsync interrupt keeps
// reporting the frame that is really on screen and dxgkrnl neither retires the
// queued flip nor frees the primary it would displace.  That is what makes it
// safe for the UMD's present to return immediately.
BOOLEAN VioGpuVidPN::TryPromoteFlip()
{
    PAGED_CODE();

    // Consume the arm FIRST, then read the latch.  Any producer that latches
    // after this exchange re-arms the flag, so its flip is picked up by a
    // later promote against its own latch state.  Consuming only after the
    // token check would swallow the arm of a producer that latched in the
    // window: that flip would never scan out, its address would never be
    // reported, and dxgkrnl would wait on it forever.
    if (!InterlockedExchange(&m_shouldFlip, 0))
    {
        return FALSE;
    }

    VioGpuAllocation *res = NULL;
    PHYSICAL_ADDRESS address;
    ULONGLONG required = 0;
    BOOLEAN timedOut = FALSE;

    const ULONGLONG done = m_pAdapter->PresentTokenDone();
    const ULONGLONG now = KeQueryInterruptTime();

    KIRQL oldIrql = AcquireSourceLock();
    address = m_sourceAddress;
    if (m_flipQCount != 0)
    {
        // Scan out the NEWEST queued flip whose render has retired; every
        // older entry is superseded and dropped (sync-interval-0 frame
        // dropping).  If nothing has retired, the oldest entry's own age
        // bounds the wait: past the deadline a completion was lost, so the
        // newest frame is shown anyway rather than freezing the display
        // (and a genuinely wedged GPU still surfaces as a TDR elsewhere).
        LONG pick = -1;
        for (LONG i = (LONG)m_flipQCount - 1; i >= 0; i--)
        {
            const ULONGLONG tok = m_flipQueue[(m_flipQHead + i) % VIOGPU_FLIP_QUEUE_DEPTH].Token;
            // Per-token, not a max-of-retired watermark: tokens come from one
            // adapter-wide counter but retire on per-device, per-event-ring
            // streams, so an unrelated device's later token can retire first
            // and would mark this frame's render done while the GPU is still
            // writing the buffer.
            if (m_pAdapter->PresentTokenRetired(tok))
            {
                pick = i;
                break;
            }
        }
        if (pick < 0)
        {
            if (now - m_flipQueue[m_flipQHead].ArmedAt < VIOGPU_FLIP_TOKEN_DEADLINE_100NS)
            {
                // Not ready: put the arm back.  A producer may have re-armed
                // already; OR keeps it set either way, and whichever promote
                // eventually consumes it re-evaluates the queue, so nothing
                // is lost.
                ReleaseSourceLock(oldIrql);
                InterlockedOr(&m_shouldFlip, 1);
                InterlockedIncrement(&m_flipParked);
                return FALSE;
            }
            timedOut = TRUE;
            pick = (LONG)m_flipQCount - 1;
        }
        // Steal the picked entry's reference; drop everything older.
        for (LONG i = 0; i <= pick; i++)
        {
            PENDING_FLIP *p = &m_flipQueue[(m_flipQHead + i) % VIOGPU_FLIP_QUEUE_DEPTH];
            if (i == pick)
            {
                res = p->Res;
                required = p->Token;
            }
            else
            {
                if (p->Res)
                {
                    p->Res->ReleaseDeferred();
                }
            }
            p->Res = NULL;
        }
        m_flipQHead = (m_flipQHead + (ULONG)pick + 1) % VIOGPU_FLIP_QUEUE_DEPTH;
        m_flipQCount -= (ULONG)pick + 1;
        if (m_flipQCount != 0)
        {
            // Newer flips remain pending; keep the promote loop running.
            InterlockedOr(&m_shouldFlip, 1);
        }
    }
    else
    {
        // No fenced flips pending: legacy direct latch (boot / GDI / blt
        // primaries and SetVidPnSourceAddress-only transitions).
        res = m_sourceRes;
        if (res)
        {
            res->AddRef();
        }
    }
    ReleaseSourceLock(oldIrql);

    if (timedOut)
    {
        LONG n = InterlockedIncrement(&m_flipTokenTimeouts);
        // Rate-limited: an unconditional print on the flip path is a known
        // TDR heisenbug.
        if (n <= 16 || (n & 63) == 0)
        {
            DbgPrintEx(DPFLTR_DEFAULT_ID,
                       DPFLTR_ERROR_LEVEL,
                       "%s: flip token %llu not retired (done=%llu) after deadline #%ld; "
                       "scanning out anyway\n",
                       __FUNCTION__,
                       required,
                       done,
                       n);
        }
    }

    // Blob primaries (the blt-present standing dmabuf set via
    // SetScanoutSource) scan out by res_id through SetScanoutBlob and carry
    // no guest PrimaryAddress, so flush them regardless of address; only 3D
    // primaries are gated on a non-zero MMIO-flip address.
    if (res != NULL && (address.QuadPart != 0 || res->IsBlob()))
    {
        res->FlushToScreen(0);
    }
    else
    {
        m_pAdapter->ctrlQueue.SetScanout(0, 0, 0, 0, 0, 0);
    }

    if (res)
    {
        res->Release();
    }

    // Published only after the scanout was emitted: from here the vsync may
    // tell dxgkrnl this flip retired.
    oldIrql = AcquireSourceLock();
    m_displayedAddress = address;
    ReleaseSourceLock(oldIrql);

    if (required != 0)
    {
        InterlockedIncrement(&m_flipGatedPromotes);
    }
    else
    {
        InterlockedIncrement(&m_flipUngatedPromotes);
    }
    InterlockedIncrement(&m_flipPromotes);

    return TRUE;
}

void VioGpuVidPN::Flip()
{
    PAGED_CODE();

    TryPromoteFlip();

    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
    interrupt.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;

    interrupt.CrtcVsync.VidPnTargetId = 0;
    // Report the address of what is actually displayed: dxgkrnl completes
    // queued flips (and waits to reuse or destroy displaced primaries)
    // based on the address the vsync reports, so it must track the flip
    // latch.  Flip-model primaries get their segment address at Patch;
    // fall back to the last SetVidPnSourceAddress value (MMIO flips,
    // boot primary) when the latch has no patched address yet.
    {
        KIRQL vsyncIrql = AcquireSourceLock();
        // Report the address of the frame that has actually been scanned
        // out.  For MMIO flips (FlipOnVSyncMmIo) this is the PrimaryAddress
        // of the flip and MUST be echoed verbatim for dxgkrnl to confirm it;
        // the allocation's patched SegmentAddress is only a boot-primary
        // fallback.
        //
        // m_displayedAddress rather than the latched m_sourceAddress: a flip
        // whose render has not retired has been latched but NOT scanned out,
        // and reporting it would tell dxgkrnl the flip completed -- freeing
        // the displaced primary for reuse while it is still the one on
        // screen, and letting the app overwrite the buffer we are about to
        // display.  Once TryPromoteFlip emits the scanout the two agree.
        interrupt.CrtcVsync.PhysicalAddress =
            (m_displayedAddress.QuadPart != 0)
                ? m_displayedAddress
                : ((m_sourceRes && m_sourceRes->m_SegmentAddress.QuadPart != 0)
                       ? m_sourceRes->m_SegmentAddress
                       : m_displayedAddress);
        ReleaseSourceLock(vsyncIrql);
    }

    m_pAdapter->NotifyInterrupt(&interrupt, true);
}

D3DDDI_RATIONAL VioGpuVidPN::GetActiveRefreshRate() const
{
    PAGED_CODE();

    D3DDDI_RATIONAL rate = {0, 0};
    // m_ModeInfo/m_CurrentModeIndex point at the active mode. Our
    // builds emit a fixed 60 Hz signal (see BuildVideoSignalInfo);
    // surface that to UMD when a source is pinned, otherwise leave
    // the rate unset so the caller picks a default.
    if (m_ModeInfo && m_CurrentModeIndex < m_ModeCount)
    {
        rate.Numerator = 60;
        rate.Denominator = 1;
    }
    return rate;
}

void VioGpuVidPN::FlipThread(void *ctx)
{
    PAGED_CODE();

    VioGpuVidPN *vidpn = reinterpret_cast<VioGpuVidPN *>(ctx);
    PVOID waitObjects[2] = {&vidpn->m_vsyncTimer, &vidpn->m_flipReadyEvent};

    // This thread IS the display: it emits every scanout and reports every
    // vsync.  At default priority it starves whenever a benchmark saturates
    // the vCPUs.  Real display drivers run this work at DIRQL/DPC; the
    // closest a system thread gets is the realtime band, where it preempts
    // any time-sharing workload thread but still yields to DPCs and other
    // realtime work.
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    // Force the first loop iteration to program the timer (the thread can be
    // restarted after ReleasePostDisplayOwnership, and the previous run
    // cancelled it on exit).
    vidpn->m_vsyncTimerPeriod100ns = 0;

    for (;;)
    {
        // The vsync tick comes from a PERIODIC timer, not from a wait
        // timeout.  A relative timeout restarts the full period on every
        // wake, and m_flipReadyEvent fires on every present-fence completion
        // adapter-wide (one per ID3D11Fence::SetEventOnCompletion, from
        // every process) -- under load their inter-arrival stays below the
        // refresh period, a timeout-based tick then never fires, and per the
        // MMIO-flip contract (DxgkDdiSetVidPnSourceAddress: completion is
        // reported only by the CRTC_VSYNC interrupt's effective scan
        // address) every queued flip stops completing: the screen freezes on
        // one frame while rendering continues.  The periodic timer keeps
        // ticking no matter how often the event fires.
        //
        // Re-program only when a mode change alters the refresh rate.  The
        // ms rounding of KeSetTimerEx's Period is fine: this is a synthetic
        // cadence for dxgkrnl, not a hardware vblank.
        LONGLONG period100ns = VsyncPeriodFromRefresh(vidpn->GetActiveRefreshRate());
        if (period100ns != vidpn->m_vsyncTimerPeriod100ns)
        {
            vidpn->m_vsyncTimerPeriod100ns = period100ns;
            LARGE_INTEGER due;
            due.QuadPart = -period100ns;
            LONG periodMs = (LONG)((period100ns + 5000) / 10000);
            if (periodMs < 1)
            {
                periodMs = 1;
            }
            KeSetTimerEx(&vidpn->m_vsyncTimer, due, periodMs, NULL);
        }

        // Timer tick -> Flip() (vsync interrupt + promote).  Fence-retire
        // wake -> TryPromoteFlip() only: a flip whose render just finished
        // scans out immediately instead of sitting out the rest of the
        // period, but no extra vsync is reported -- dxgkrnl needs a steady
        // refresh cadence, and emitting vsyncs at the fence-completion rate
        // would corrupt its flip accounting.
        NTSTATUS wait = KeWaitForMultipleObjects(2,
                                                 waitObjects,
                                                 WaitAny,
                                                 Executive,
                                                 KernelMode,
                                                 FALSE,
                                                 NULL,
                                                 NULL);
        if (vidpn->m_shouldFlipStop)
        {
            break;
        }
        if (wait == STATUS_WAIT_0)
        {
            vidpn->Flip();
        }
        else
        {
            vidpn->TryPromoteFlip();
        }
    }

    KeCancelTimer(&vidpn->m_vsyncTimer);
}

PAGED_CODE_SEG_END

//
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()

void VioGpuVidPN::OnPresentTokenRetired()
{
    // Runs at DISPATCH_LEVEL from the present-fence completion DPC
    // (PresentFenceCb -> here).  It only wakes the flip thread -- FlushToScreen
    // is PAGED and stays on that thread -- but the FUNCTION ITSELF must live in
    // a NON-PAGED code segment: MmTrimAllSystemPagableMemory can trim its code
    // page out, and executing a paged-out page at DISPATCH bugchecks 0xD1
    // (DRIVER_IRQL_NOT_LESS_OR_EQUAL, Arg1==Arg4 = an execute-from-paged fault).
    KeSetEvent(&m_flipReadyEvent, IO_NO_INCREMENT, FALSE);
}

// NON-PAGED: runs under m_sourceLock at DISPATCH_LEVEL (SetScanoutSource can
// hold the lock with callers at raised IRQL) -- executing paged code there is
// the 0xD1 class OnPresentTokenRetired documents above.  ReleaseDeferred is
// the DIRQL-safe release.
void VioGpuVidPN::FlipQueueClearLocked()
{
    while (m_flipQCount != 0)
    {
        PENDING_FLIP *p = &m_flipQueue[m_flipQHead];
        if (p->Res)
        {
            p->Res->ReleaseDeferred();
        }
        p->Res = NULL;
        m_flipQHead = (m_flipQHead + 1) % VIOGPU_FLIP_QUEUE_DEPTH;
        m_flipQCount--;
    }
}

NTSTATUS VioGpuVidPN::SetVidPnSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress)
{
    VioGpuAllocation *newRes = VioGpuAllocation::FromHandle(pSetVidPnSourceAddress->hAllocation);
    if (newRes)
    {
        newRes->AddRef();
    }

    KIRQL oldIrql = AcquireSourceLock();
    VioGpuAllocation *oldRes = m_sourceRes;
    m_sourceAddress = pSetVidPnSourceAddress->PrimaryAddress;
    m_sourceRes = newRes;
    ReleaseSourceLock(oldIrql);

    if (oldRes)
    {
        // DxgkDdiSetVidPnSourceAddress is called at PASSIVE_LEVEL for
        // mode-switch and at DIRQL for MMIO-based flips
        // (FlipCaps.FlipOnVSyncMmIo = TRUE). The destructor reaches
        // PAGED_CODE() through ~VioGpuDeviceAllocation, so the trailing
        // Release must drop to PASSIVE_LEVEL before running it.
        oldRes->ReleaseDeferred();
    }

    InterlockedOr(&m_shouldFlip, 1);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d isBlob=%d, vidPnSrcId=%d, duration=%lld\n",
                                   __FUNCTION__,
                                   newRes ? newRes->GetId() : 0,
                                   newRes ? newRes->IsBlob() : FALSE,
                                   pSetVidPnSourceAddress->VidPnSourceId,
                                   pSetVidPnSourceAddress->Duration));

    return STATUS_SUCCESS;
};

void VioGpuVidPN::RearmFlipIfScanout(VioGpuAllocation *res)
{
    // GDI/basic present model: blts land in the shared primary that the
    // scanout already points at (SetVidPnSourceAddress at modeset), so no
    // present DDI re-arms the flip latch and the host framebuffer updates
    // are never re-flushed -- the display freezes on the modeset frame.
    // Re-arm the vsync flip when a blt destination IS the current scanout
    // source so FlushToScreen re-emits SET_SCANOUT + RESOURCE_FLUSH.
    KIRQL oldIrql = AcquireSourceLock();
    BOOLEAN isScanout = (m_sourceRes == res);
    ReleaseSourceLock(oldIrql);

    if (isScanout)
    {
        InterlockedOr(&m_shouldFlip, 1);
    }
}
void VioGpuVidPN::SetScanoutSource(VioGpuAllocation *res, PHYSICAL_ADDRESS addr, ULONGLONG requiredToken)
{
    // Only the full-screen desktop primary may become the scanout source.
    // DWM can present its cursor (e.g. 32x32) and individual windows
    // (sub-screen) as their OWN "primary" allocations as well; without this
    // gate the most-recently-created/flipped one clobbers the desktop and
    // the screen scans out a cursor/window surface (black/garbage desktop).
    //
    // The framebuffer BYTE SIZE -- populated on every allocation -- is the
    // discriminator: the desktop primary backs the whole screen
    // (>= mode_w * mode_h * 4), the cursor / per-window primaries are far
    // smaller.  A 0 mode (before CommitVidPn) bypasses the gate so the boot
    // primary still promotes.
    if (res != NULL)
    {
        UINT mw = m_CurrentModes[0].DispInfo.Width;
        UINT mh = m_CurrentModes[0].DispInfo.Height;
        ULONGLONG modeBytes = (ULONGLONG)mw * (ULONGLONG)mh * 4ull;
        ULONGLONG resBytes = res->GetSize();
        // 95% margin tolerates stride / height padding on the desktop dmabuf
        // while still excluding sub-screen surfaces.
        if (mw != 0 && mh != 0 && resBytes * 100ull < modeBytes * 95ull)
        {
            return;
        }
    }

    // Mirror SetVidPnSourceAddress's refcount/swap discipline.  Blob
    // scanout is keyed by res_id, so the flip latch does not need a
    // PrimaryAddress -- but m_sourceAddress must be left ALONE: it is
    // what the vsync interrupt reports back to dxgkrnl, and dxgkrnl
    // completes a queued SetVidPnSourceAddress flip only when a vsync
    // reports that flip's address.  Zeroing it here let a blob latch
    // race a concurrent MMIO flip (e.g. dxgkrnl reverting to the
    // standard shared primary when a device died) and park it forever:
    // display-path TDR with an idle engine (submitted==completed).
    if (res)
    {
        res->AddRef();
    }

    KIRQL oldIrql = AcquireSourceLock();
    VioGpuAllocation *oldRes = m_sourceRes;
    m_sourceRes = res;
    if (addr.QuadPart != 0)
    {
        m_sourceAddress = addr;
    }
    // A flip WITH a render dependency queues; see the PENDING_FLIP block in
    // viogpu_vidpn.h for why this must be a queue and not a coalescing
    // latch.  A latch WITHOUT a dependency (boot/GDI primaries pass 0)
    // supersedes everything queued: its content is current by definition,
    // and chaining it behind a stale token would stall the display on the
    // wrong image.
    if (requiredToken != 0)
    {
        if (m_flipQCount == VIOGPU_FLIP_QUEUE_DEPTH)
        {
            // Full: the oldest frame is dropped, exactly as a sync-interval-0
            // flip queue drops superseded frames.
            PENDING_FLIP *pOld = &m_flipQueue[m_flipQHead];
            if (pOld->Res)
            {
                pOld->Res->ReleaseDeferred();
            }
            pOld->Res = NULL;
            m_flipQHead = (m_flipQHead + 1) % VIOGPU_FLIP_QUEUE_DEPTH;
            m_flipQCount--;
        }
        PENDING_FLIP *pNew = &m_flipQueue[(m_flipQHead + m_flipQCount) % VIOGPU_FLIP_QUEUE_DEPTH];
        pNew->Res = res;
        if (res)
        {
            res->AddRef();
        }
        pNew->Token = requiredToken;
        pNew->ArmedAt = KeQueryInterruptTime();
        m_flipQCount++;
    }
    else
    {
        FlipQueueClearLocked();
    }
    ReleaseSourceLock(oldIrql);

    if (oldRes)
    {
        oldRes->ReleaseDeferred();
    }

    InterlockedOr(&m_shouldFlip, 1);
}

D3DDDI_VIDEO_PRESENT_SOURCE_ID VioGpuVidPN::FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                BOOLEAN DefaultToZero)
{
    UNREFERENCED_PARAMETER(TargetId);
    for (UINT SourceId = 0; SourceId < MAX_VIEWS; ++SourceId)
    {
        if (m_CurrentModes[SourceId].FrameBuffer.Ptr != NULL)
        {
            return SourceId;
        }
    }

    return DefaultToZero ? 0 : D3DDDI_ID_UNINITIALIZED;
}

NTSTATUS VioGpuVidPN::SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                          _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                          _Out_ UINT *pWidth,
                                          _Out_ UINT *pHeight,
                                          _Out_ D3DDDIFORMAT *pColorFormat)
{
    UNREFERENCED_PARAMETER(Flags);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;

    VIOGPU_ASSERT((TargetId < MAX_CHILDREN) || (TargetId == D3DDDI_ID_UNINITIALIZED));

    if (TargetId == D3DDDI_ID_UNINITIALIZED)
    {
        for (UINT SourceIdx = 0; SourceIdx < MAX_VIEWS; ++SourceIdx)
        {
            if (m_CurrentModes[SourceIdx].FrameBuffer.Ptr != NULL)
            {
                m_SystemDisplaySourceId = SourceIdx;
                break;
            }
        }
    }
    else
    {
        m_SystemDisplaySourceId = FindSourceForTarget(TargetId, FALSE);
    }

    if (m_SystemDisplaySourceId == D3DDDI_ID_UNINITIALIZED)
    {
        {
            return STATUS_UNSUCCESSFUL;
        }
    }

    if ((m_CurrentModes[m_SystemDisplaySourceId].Rotation == D3DKMDT_VPPR_ROTATE90) ||
        (m_CurrentModes[m_SystemDisplaySourceId].Rotation == D3DKMDT_VPPR_ROTATE270))
    {
        *pHeight = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
        *pWidth = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;
    }
    else
    {
        *pWidth = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
        *pHeight = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;
    }

    *pColorFormat = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat;
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s ColorFormat = %d\n",
              __FUNCTION__,
              m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat));

    return STATUS_SUCCESS;
}

VOID VioGpuVidPN::SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                                     _In_ UINT SourceWidth,
                                     _In_ UINT SourceHeight,
                                     _In_ UINT SourceStride,
                                     _In_ INT PositionX,
                                     _In_ INT PositionY)
{
    UNREFERENCED_PARAMETER(pSource);
    UNREFERENCED_PARAMETER(SourceStride);

    RECT Rect;
    Rect.left = PositionX;
    Rect.top = PositionY;
    Rect.right = Rect.left + SourceWidth;
    Rect.bottom = Rect.top + SourceHeight;

    BLT_INFO DstBltInfo;
    DstBltInfo.pBits = m_CurrentModes[m_SystemDisplaySourceId].FrameBuffer.Ptr;
    DstBltInfo.Pitch = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Pitch;
    DstBltInfo.BitsPerPel = BPPFromPixelFormat(m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat);
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = m_CurrentModes[m_SystemDisplaySourceId].Rotation;
    DstBltInfo.Width = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
    DstBltInfo.Height = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;

    BLT_INFO SrcBltInfo;
    SrcBltInfo.pBits = pSource;
    SrcBltInfo.Pitch = SourceStride;
    SrcBltInfo.BitsPerPel = 32;

    SrcBltInfo.Offset.x = -PositionX;
    SrcBltInfo.Offset.y = -PositionY;
    SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    SrcBltInfo.Width = SourceWidth;
    SrcBltInfo.Height = SourceHeight;

    BltBits(&DstBltInfo, &SrcBltInfo, &Rect);
}

#pragma code_seg(pop) // End Non-Paged Code

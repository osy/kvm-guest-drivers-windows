#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"
#include "virgl_hw.h"

PAGED_CODE_SEG_BEGIN

VioGpuContext::VioGpuContext(VioGpuAdapter *pAdapter) {
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));
    m_Capset = 0;
    m_pAdapter = pAdapter;
    m_id = m_pAdapter->ctxIdr.GetId();
    m_empty = TRUE;
}

void VioGpuContext::Init(VIOGPU_CTX_INIT_REQ *pOptions) {
    PAGED_CODE();

    m_Capset = pOptions->CapsetID;

    if (!m_empty)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("<--> %s UNREACHABLE: do not create context twice! (ctx_id=%d new capset %d, old capset %d, name %s)\n", __FUNCTION__, m_id, pOptions->CapsetID, m_Capset, pOptions->DebugName));
        m_pAdapter->ctrlQueue.DestroyCtx(m_id, NULL, NULL);
    }
    else
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("<--> %s (ctx_id=%d capset=%d name=%s)\n", __FUNCTION__, m_id, pOptions->CapsetID, pOptions->DebugName));
    }

    m_pAdapter->ctrlQueue.CreateCtx(m_id, pOptions->CapsetID, pOptions->DebugName);

    m_empty = FALSE;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()

static void NotifyContextDestroyed(void *ctx, void *cmd, void *)
{
    VioGpuIdr *ctxIdr = reinterpret_cast<VioGpuIdr *>(ctx);
    PGPU_CTRL_HDR cmd_hdr = reinterpret_cast<PGPU_CTRL_HDR>(cmd);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s freeing ctx %lu\n", __FUNCTION__, cmd_hdr->ctx_id));
    ctxIdr->PutId(cmd_hdr->ctx_id);
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

VioGpuContext::~VioGpuContext() {
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    m_pAdapter->ctrlQueue.DestroyCtx(m_id, NotifyContextDestroyed, &m_pAdapter->ctxIdr);
    // m_pAdapter->ctxIdr.PutId(m_id);
}

VioGpuDevice::VioGpuDevice(VioGpuAdapter *pAdapter) : m_Context(pAdapter), m_Virgl(pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    m_hUM = NULL;
    m_hKM = NULL;
    m_pBlit = NULL;

    m_pAdapter = pAdapter;
}

VioGpuDevice::~VioGpuDevice()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s", __FUNCTION__));

    if (m_hUM) ObDereferenceObject(m_hUM);
    if (m_hKM) ObDereferenceObject(m_hKM);
}

NTSTATUS VioGpuDevice::GenerateBltPresent(DXGKARG_PRESENT *pPresent, VioGpuDeviceAllocation *srcDev, VioGpuDeviceAllocation *dstDev)
{
    VioGpuAllocation *src = srcDev->GetAllocation();
    VioGpuAllocation *dst = dstDev->GetAllocation();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    DbgPrint(TRACE_LEVEL_WARNING, ("<--> %s srcCoherent=%d dstCoherent=%d\n", __FUNCTION__, src->IsCoherent(), dst->IsCoherent()));

    UCHAR *dmaBuf = (UCHAR *)pPresent->pDmaBuffer;

    // The virgl shadow context is normally created by the UMD's
    // VIOGPU_CTX_INIT escape (VioGpu3DEscape). The GDI/System device that
    // carries the basic-model shadow->primary blt presents never runs that
    // escape, so m_Virgl holds only a guest-minted id with NO host context
    // behind it -- every CtxResource attach and SUBMIT below is silently
    // dropped by the host and the primary never receives the blt (frozen
    // black desktop). Create the host context lazily on first use.
    if (m_Virgl.IsEmpty())
    {
        bool has_virgl  = !!(m_pAdapter->m_supportedCapsetIDs & (1llu << VIRTIO_GPU_CAPSET_VIRGL));
        bool has_virgl2 = !!(m_pAdapter->m_supportedCapsetIDs & (1llu << VIRTIO_GPU_CAPSET_VIRGL2));
        if (has_virgl || has_virgl2)
        {
            VIOGPU_CTX_INIT_REQ VirglCtx;
            memset(&VirglCtx, 0, sizeof(VirglCtx));
            VirglCtx.CapsetID = has_virgl2 ? VIRTIO_GPU_CAPSET_VIRGL2 : VIRTIO_GPU_CAPSET_VIRGL;
            VirglCtx.NumRings = 64;
            memcpy(VirglCtx.DebugName, "virgl-gdi-blt", sizeof("virgl-gdi-blt") - 1);
            m_Virgl.Init(&VirglCtx);
        }
        else
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("%s no virgl capset for blt present\n", __FUNCTION__));
            return STATUS_UNSUCCESSFUL;
        }
    }

    // Calculate rect covering all SubRectx
    RECT coverRect = pPresent->pDstSubRects[0];
    for (UINT i = 1; i < pPresent->SubRectCnt; i++)
    {
        coverRect.top = min(coverRect.top, pPresent->pDstSubRects[i].top);
        coverRect.left = min(coverRect.left, pPresent->pDstSubRects[i].left);
        coverRect.right = max(coverRect.right, pPresent->pDstSubRects[i].right);
        coverRect.bottom = max(coverRect.bottom, pPresent->pDstSubRects[i].bottom);
    }

    INT dx = pPresent->SrcRect.left - pPresent->DstRect.left;
    INT dy = pPresent->SrcRect.top - pPresent->DstRect.top;

    // If source requires coherency (staging or shadow surface) then emit transfer
    if (src->IsCoherent())
    {
        ULONG transferStride;
        ULONGLONG transferOffset;
        LONG transferX = coverRect.left + dx;
        LONG transferY = coverRect.top + dy;
        if (!src->GetTransferLayout(transferX, transferY, &transferStride, &transferOffset))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--> %s invalid source transfer layout res_id=%d x=%ld y=%ld\n",
                      __FUNCTION__, src->GetId(), transferX, transferY));
            return STATUS_INVALID_PARAMETER;
        }

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_TO_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        // Route through the virgl shadow ctx like every other command in
        // this packet: m_Context is uninitialized for the GDI/System
        // device, and a TRANSFER_TO_HOST_3D on a nonexistent ctx is
        // rejected by the host (EINVAL) -- the shadow surface pixels never
        // reach the host texture and the desktop blits stay black.
        if (!m_Context.IsVirgl())
        {
            cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
        }

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

        cmdBody->res_id = src->GetId();

        cmdBody->box.x = coverRect.left + dx;
        cmdBody->box.y = coverRect.top + dy;
        cmdBody->box.z = 0;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;

        cmdBody->layer_stride = 0;
        cmdBody->stride = transferStride;
        cmdBody->level = 0;
        cmdBody->offset = transferOffset;
    }

    if (!srcDev->m_AttachedToVirgl)
    {
        GetCtrlQueue()->CtxResource(true, m_Virgl.GetId(), src->GetId());
        srcDev->m_AttachedToVirgl = true;

        if (src->IsBlob())
        {
            UINT sizeOfSetType = 4 * (VIRGL_PIPE_RES_SET_TYPE_SIZE(1) + 1);

            VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
            cmd_hdr->type = VIOGPU_CMD_SUBMIT;
            cmd_hdr->size = sizeOfSetType;
            cmd_hdr->flags = 0;
            cmd_hdr->ring_idx = 0;
            dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

            if (!m_Context.IsVirgl())
            {
                cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
            }

            UINT *cmdBody = (UINT *)dmaBuf;
            dmaBuf += sizeOfSetType;

            cmdBody[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE, 0, VIRGL_PIPE_RES_SET_TYPE_SIZE(1));
            cmdBody[1] = src->GetId(),
            cmdBody[2] = src->m_Blob.Info.format;
            cmdBody[3] = VIRGL_BIND_RENDER_TARGET | /*VIRGL_BIND_LINEAR |*/ VIRGL_BIND_SHARED;
            cmdBody[4] = src->m_Blob.Info.width;
            cmdBody[5] = src->m_Blob.Info.height;
            cmdBody[6] = 0; // usage seems to be ignored
            cmdBody[7] = (UINT)(src->m_BlobModifier & 0xFFFFFFFFull);
            cmdBody[8] = (UINT)(src->m_BlobModifier >> 32);
            cmdBody[9] = src->m_Blob.Info.strides[0];
            cmdBody[10] = src->m_Blob.Info.offsets[0];
        }
    }

    if (!dstDev->m_AttachedToVirgl)
    {
        GetCtrlQueue()->CtxResource(true, m_Virgl.GetId(), dst->GetId());
        dstDev->m_AttachedToVirgl = true;
        if (dst->IsBlob())
        {
            UINT sizeOfSetType = 4 * (VIRGL_PIPE_RES_SET_TYPE_SIZE(1) + 1);

            VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
            cmd_hdr->type = VIOGPU_CMD_SUBMIT;
            cmd_hdr->size = sizeOfSetType;
            cmd_hdr->flags = 0;
            cmd_hdr->ring_idx = 0;
            dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

            if (!m_Context.IsVirgl())
            {
                cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
            }

            UINT *cmdBody = (UINT *)dmaBuf;
            dmaBuf += sizeOfSetType;

            cmdBody[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE, 0, VIRGL_PIPE_RES_SET_TYPE_SIZE(1));
            cmdBody[1] = dst->GetId(),
            cmdBody[2] = dst->m_Blob.Info.format;
            cmdBody[3] = VIRGL_BIND_RENDER_TARGET | /*VIRGL_BIND_LINEAR |*/ VIRGL_BIND_SHARED;
            cmdBody[4] = dst->m_Blob.Info.width;
            cmdBody[5] = dst->m_Blob.Info.height;
            cmdBody[6] = 0; // usage seems to be ignored
            cmdBody[7] = (UINT)(dst->m_BlobModifier & 0xFFFFFFFFull);
            cmdBody[8] = (UINT)(dst->m_BlobModifier >> 32);
            cmdBody[9] = dst->m_Blob.Info.strides[0];
            cmdBody[10] = dst->m_Blob.Info.offsets[0];
        }
    }

    {
        UINT sizeOfOneRect = 4 * (VIRGL_CMD_RESOURCE_COPY_REGION_SIZE + 1);

        // TODO: Support MultiPassOffset
        UINT rectCnt = min(pPresent->SubRectCnt, (pPresent->DmaSize - 0x100) / sizeOfOneRect);

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_SUBMIT;
        cmd_hdr->size = rectCnt * sizeOfOneRect;
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        if (!m_Context.IsVirgl()) {
            cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
        }

        for (UINT i = 0; i < rectCnt; i++)
        {
            UINT *cmdBody = (UINT *)dmaBuf;
            dmaBuf += sizeOfOneRect;

            RECT rect = pPresent->pDstSubRects[i];

            cmdBody[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
            cmdBody[1] = dst->GetId();
            cmdBody[2] = 0;
            cmdBody[3] = rect.left;
            cmdBody[4] = rect.top;
            cmdBody[5] = 0;

            cmdBody[6] = src->GetId();
            cmdBody[7] = 0;
            cmdBody[8] = rect.left + dx;
            cmdBody[9] = rect.top + dy;
            cmdBody[10] = 0;
            cmdBody[11] = rect.right - rect.left;
            cmdBody[12] = rect.bottom - rect.top;
            cmdBody[13] = 1;
        }
    }

    if (dst->IsCoherent())
    {
        ULONG transferStride;
        ULONGLONG transferOffset;
        if (!dst->GetTransferLayout(coverRect.left, coverRect.top, &transferStride, &transferOffset))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--> %s invalid destination transfer layout res_id=%d x=%ld y=%ld\n",
                      __FUNCTION__, dst->GetId(), coverRect.left, coverRect.top));
            return STATUS_INVALID_PARAMETER;
        }

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_FROM_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        if (!m_Context.IsVirgl()) {
            cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
        }

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

        cmdBody->res_id = dst->GetId();

        cmdBody->box.x = coverRect.left;
        cmdBody->box.y = coverRect.top;
        cmdBody->box.z = 0;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;

        cmdBody->layer_stride = 0;
        cmdBody->stride = transferStride;
        cmdBody->level = 0;
        cmdBody->offset = transferOffset;
    }

    pPresent->pDmaBuffer = dmaBuf;

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::GenerateBltPresentUM(DXGKARG_PRESENT *pPresent, VioGpuAllocation *src, VioGpuAllocation *dst)
{
    UNREFERENCED_PARAMETER(src);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    if (!CanBlit()) {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s Invoke VIOGPU_BLIT_INIT escape first\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_BLIT_PRESENT blit;

    __try
    {
        ProbeForRead(m_pBlit, sizeof(blit), sizeof(ULONG));
        RtlCopyMemory(&blit, m_pBlit, sizeof(blit));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("---> %s: Failed to copy from user\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    // Calculate rect covering all SubRectx
    RECT coverRect = pPresent->pDstSubRects[0];
    for (UINT i = 1; i < pPresent->SubRectCnt; i++)
    {
        coverRect.top = min(coverRect.top, pPresent->pDstSubRects[i].top);
        coverRect.left = min(coverRect.left, pPresent->pDstSubRects[i].left);
        coverRect.right = max(coverRect.right, pPresent->pDstSubRects[i].right);
        coverRect.bottom = max(coverRect.bottom, pPresent->pDstSubRects[i].bottom);
    }

    if (dst->IsBlob()) {
        blit.dst.alloc.Type = VIOGPU_RESOURCE_TYPE_BLOB;
        blit.dst.alloc.OptionsBlob = dst->m_Blob.Options;
        blit.dst.alloc.Size = dst->m_Size;
    } else {
        blit.dst.alloc.Type = VIOGPU_RESOURCE_TYPE_3D;
        blit.dst.alloc.Options3D = dst->m_3dOptions;
        blit.dst.alloc.Size = dst->m_Size;
    }
    // Descriptor only: the blit reads the host resource directly, so this path
    // must not establish a BAR mapping the UMD never asked for.
    dst->EscapeResourceInfo(&blit.dst.res_info, NULL);

    INT dx = pPresent->SrcRect.left - pPresent->DstRect.left;
    INT dy = pPresent->SrcRect.top - pPresent->DstRect.top;

    for (UINT i = 0; i < pPresent->SubRectCnt; i++)
    {
        KeClearEvent(m_hKM);
        KeClearEvent(m_hUM);

        RECT rect = pPresent->pDstSubRects[i];

        blit.src.rect.left = rect.left + dx;
        blit.src.rect.right = rect.right + dx;
        blit.src.rect.top = rect.top + dy;
        blit.src.rect.bottom = rect.bottom + dy;
        blit.dst.rect = rect;

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("---> %s: DstRect = {.left = %ld, .top = %ld, .right = %ld, .bottom = %ld}\n",
                  __FUNCTION__,
                  blit.dst.rect.left,
                  blit.dst.rect.top,
                  blit.dst.rect.right,
                  blit.dst.rect.bottom));

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("---> %s: SrcRect = {.left = %ld, .top = %ld, .right = %ld, .bottom = %ld}, dx = %d, dy = %d\n",
                  __FUNCTION__,
                  blit.src.rect.left,
                  blit.src.rect.top,
                  blit.src.rect.right,
                  blit.src.rect.bottom,
                  dx, dy));

        __try
        {
            ProbeForWrite(m_pBlit, sizeof(blit), sizeof(ULONG));
            RtlCopyMemory(m_pBlit, &blit, sizeof(blit));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s: Failed to copy to user\n", __FUNCTION__));
            return STATUS_INVALID_PARAMETER;
        }

        /* Blit information set up complete */
        KeSetEvent(m_hUM, IO_NO_INCREMENT, FALSE);
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s: waiting for blit from userspace %p / %p\n", __FUNCTION__, m_hUM, m_hKM));

        LARGE_INTEGER timeout = {0};
        timeout.QuadPart = Int32x32To64(10000, -10000);
        /* Waiting for userspace to perform blit */
        if (!NT_SUCCESS(KeWaitForSingleObject(m_hKM, Executive, KernelMode, FALSE, &timeout))) {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s: TIMEOUT waiting for blit from userspace\n", __FUNCTION__));
            break;
        }
        /* Blit done */
    }

    KeClearEvent(m_hUM);
    KeClearEvent(m_hKM);

    if (dst->IsCoherent())
    {
        ULONG transferStride;
        ULONGLONG transferOffset;
        if (!dst->GetTransferLayout(coverRect.left, coverRect.top, &transferStride, &transferOffset))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--> %s invalid destination transfer layout res_id=%d x=%ld y=%ld\n",
                      __FUNCTION__, dst->GetId(), coverRect.left, coverRect.top));
            return STATUS_INVALID_PARAMETER;
        }

        UCHAR *dmaBuf = (UCHAR *)pPresent->pDmaBuffer;

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_FROM_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

        cmdBody->res_id = dst->GetId();

        cmdBody->box.x = coverRect.left;
        cmdBody->box.y = coverRect.top;
        cmdBody->box.z = 0;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;

        cmdBody->layer_stride = 0;
        cmdBody->stride = transferStride;
        cmdBody->level = 0;
        cmdBody->offset = transferOffset;

        pPresent->pDmaBuffer = dmaBuf;
    }
    else
    {
        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)pPresent->pDmaBuffer;
        cmd_hdr->type = VIOGPU_CMD_NOP;
        cmd_hdr->size = 0;
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        pPresent->pDmaBuffer = (char *)pPresent->pDmaBuffer + sizeof(VIOGPU_COMMAND_HDR);
    }

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::Present(_Inout_ DXGKARG_PRESENT *pPresent)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    // DMA buffers (and their private-data area) are RECYCLED by dxgkrnl.
    // Every path below that returns without storing a VioGpuCommand*
    // (windowed flips with no present bytes, error exits) would leave a
    // STALE pointer from the buffer's previous user; SubmitCommand then
    // resurrects that command and PrepareSubmit clobbers its fence id —
    // fences complete wrongly or never (intermittent TDR at first
    // windowed flip). NULL it up front; real writers below overwrite.
    // Stamp the packet as ours so the submit phase recovers the command it
    // carries and never mistakes a paging packet for one.
    VioGpuDmaPrivClaim(pPresent->pDmaBufferPrivateData, pPresent->DmaBufferPrivateDataSize);

    if (pPresent->Flags.Flip)
    {
        // Flip-model present: the runtime advances the swapchain by making
        // src the new active primary.  Latch m_sourceRes so the next vsync
        // Flip scans out the back buffer the runtime just made current.
        VioGpuAllocation *srcAlloc = NULL;
        ULONGLONG flipToken = 0;
        BOOLEAN packetGate = FALSE;
        DXGK_PRESENTALLOCATIONINFO *dxgk_src = &pPresent->pAllocationInfo[DXGK_PRESENT_SOURCE_INDEX];
        if (dxgk_src->hDeviceSpecificAllocation)
        {
            VioGpuDeviceAllocation *srcDev =
                VioGpuDeviceAllocation::FromHandle(dxgk_src->hDeviceSpecificAllocation);
            srcAlloc = srcDev ? srcDev->GetAllocation() : NULL;
            // Latch only the RESOURCE here. With FlipOnVSyncMmIo the
            // flip's PrimaryAddress is programmed by
            // DxgkDdiSetVidPnSourceAddress, and THAT address must be
            // what the vsync reports; letting the (earlier) Present
            // latch overwrite m_sourceAddress raced the queued flip's
            // address match and dxgkrnl TDR'd an idle engine
            // (0x117 LiveKernelEvent, submitted==completed).
            //
            // The render dependency is THIS frame's own present-fence token,
            // stamped by the escape that armed it on this same thread (see
            // viogpu_adapter.h).  0 when the frame's fence had already
            // completed at present time -- the UMD then never armed, and the
            // flip correctly runs un-gated.  Consumed unconditionally so a
            // stamp never outlives its present.
            flipToken = m_pAdapter->TakeThreadToken(PsGetCurrentThreadId(), &packetGate);
            PHYSICAL_ADDRESS zeroAddr = {};
            // Latch blob sources too, not just PRIMARY-flagged ones: dxgkrnl
            // flip-promotes a fullscreen-sized borderless window, and those
            // flips carry the app's NON-primary backbuffers as src.  Without
            // the latch the vsync keeps reporting the desktop address, so
            // dxgkrnl never retires the flips and the app blocks forever on
            // its frame-latency wait.  Backbuffers are share-exported blobs,
            // so they scan out like blob primaries.
            if (srcAlloc && (srcAlloc->IsPrimary() || srcAlloc->IsBlob()))
                m_pAdapter->vidpn.SetScanoutSource(srcAlloc, zeroAddr, flipToken);
        }

        // Packet-gated present (VIOGPU_PRESENT_GATE_HINT): carry a
        // PRESENT_WAIT{token} body so the packet's DMA completion -- the
        // signal dxgkrnl and the compositor read as "frame ready" -- is
        // parked until the token retires at real host-GPU completion.  On any
        // allocation failure the flip falls back to completing on submission
        // order (one frame's ordering hole, never a stall).
        if (packetGate && flipToken != 0 && pPresent->pDmaBufferPrivateData &&
            pPresent->DmaBufferPrivateDataSize >= sizeof(VIOGPU_DMA_PRIVATE))
        {
            VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
            if (cmd)
            {
                UCHAR body[sizeof(VIOGPU_COMMAND_HDR) + sizeof(ULONGLONG)];
                VIOGPU_COMMAND_HDR *hdr = (VIOGPU_COMMAND_HDR *)body;
                hdr->type = VIOGPU_CMD_PRESENT_WAIT;
                hdr->size = sizeof(ULONGLONG);
                hdr->flags = 0;
                hdr->ring_idx = 0;
                RtlCopyMemory(body + sizeof(VIOGPU_COMMAND_HDR), &flipToken, sizeof(ULONGLONG));

                void **privateData = (void **)pPresent->pDmaBufferPrivateData;
                *privateData = cmd->ToHandle();
                cmd->MirrorBody(pPresent->pDmaBufferPrivateData,
                                pPresent->DmaBufferPrivateDataSize,
                                body,
                                sizeof(body));
            }
            else
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s OOM building PRESENT_WAIT; flip completes ungated\n", __FUNCTION__));
            }
        }

        // Without a packet gate no host command is needed for a flip: the
        // primary IS the blob the KMD scans out, and FlushToScreen re-emits
        // SET_SCANOUT_BLOB at vsync.  The flip's (empty) DMA packet then
        // completes on submission order like any other packet.
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s Flags=(%s %s %s %s %s %s %s %s)\n",
              __FUNCTION__,
              pPresent->Flags.Blt ? "Blt" : "",
              pPresent->Flags.ColorFill ? "ColorFill" : "",
              pPresent->Flags.Flip ? "Flip" : "",
              pPresent->Flags.FlipWithNoWait ? "FlipWithNoWait" : "",
              pPresent->Flags.SrcColorKey ? "SrcColorKey" : "",
              pPresent->Flags.DstColorKey ? "DstColorKey" : "",
              pPresent->Flags.LinearToSrgb ? "LinearToSrgb" : "",
              pPresent->Flags.Rotate ? "Rotate" : ""));

    VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
    if (!cmd)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s VioGpuCommand allocation failed\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }
    if (pPresent->pDmaBuffer)
    {
        void **privateData = (void **)pPresent->pDmaBufferPrivateData;
        *privateData = cmd->ToHandle();
    }

    cmd->SetDmaBuf((char *)pPresent->pDmaBuffer);

    DXGK_PRESENTALLOCATIONINFO *dxgk_src = &pPresent->pAllocationInfo[DXGK_PRESENT_SOURCE_INDEX];
    DXGK_PRESENTALLOCATIONINFO *dxgk_dst = &pPresent->pAllocationInfo[DXGK_PRESENT_DESTINATION_INDEX];

    VioGpuDeviceAllocation *src = NULL;
    VioGpuDeviceAllocation *dst = NULL;
    UINT cPatchOut = 0;
    const UINT cPatchOutMax = pPresent->PatchLocationListOutSize;

    if (dxgk_src->hDeviceSpecificAllocation != NULL)
    {
        src = VioGpuDeviceAllocation::FromHandle(dxgk_src->hDeviceSpecificAllocation);
        if (src && pPresent->pDmaBuffer && cPatchOut < cPatchOutMax)
        {
            D3DDDI_PATCHLOCATIONLIST *out = pPresent->pPatchLocationListOut;
            RtlZeroMemory(out, sizeof(*out));
            out->AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
            out->DriverId = 1;
            out->SlotId = 1;
            pPresent->pPatchLocationListOut += 1;
            cPatchOut++;
        }
    }

    if (dxgk_dst != NULL)
    {
        dst = VioGpuDeviceAllocation::FromHandle(dxgk_dst->hDeviceSpecificAllocation);
        if (dst && pPresent->pDmaBuffer && cPatchOut < cPatchOutMax)
        {
            D3DDDI_PATCHLOCATIONLIST *out = pPresent->pPatchLocationListOut;
            RtlZeroMemory(out, sizeof(*out));
            out->AllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
            out->DriverId = 2;
            out->SlotId = 2;
            pPresent->pPatchLocationListOut += 1;
            cPatchOut++;
        }
    }

    // Register the source/destination in the driver's in-flight set
    // (m_busy via MarkBusy) so EscapeResourceBusy from DxgkDdiDestroyAllocation
    // sees Present-attached work, matching Render. Present's allocation view
    // is a fixed-size array with index 0 reserved and source/destination at
    // DXGK_PRESENT_SOURCE_INDEX (1) / DXGK_PRESENT_DESTINATION_INDEX (2);
    // there is no separate length field, so the count is
    // DXGK_PRESENT_MAX_INDEX + 1.
    NTSTATUS attachStatus = cmd->AttachAllocations(pPresent->pAllocationInfo,
                                                   DXGK_PRESENT_MAX_INDEX + 1);
    if (!NT_SUCCESS(attachStatus))
    {
        if (pPresent->pDmaBufferPrivateData)
        {
            VioGpuCommand **privateData = (VioGpuCommand **)pPresent->pDmaBufferPrivateData;
            if (*privateData == cmd)
            {
                *privateData = NULL;
            }
        }
        delete cmd;
        return attachStatus;
    }


    if (pPresent->Flags.Blt)
    {
        // The blt-present src is the back buffer DWM just rendered into.
        // When it is a flip primary, latch it as the active VidPnSource so
        // the vsync FlushToScreen scans out the resource directly -- no
        // out-of-band signalling.
        if (src)
        {
            VioGpuAllocation *srcAlloc = src->GetAllocation();
            // See the Flip branch: never let a present-path latch
            // overwrite the MMIO-flip address the vsync must report.
            //
            // No token dependency: blt presents (GDI / redirection model)
            // carry their pixels in the DMA buffer's transfer commands, not
            // in a GPU render the present fence tracks, and the UMD does not
            // arm a present fence for them.  Peeking here would chain the
            // desktop's GDI updates behind an unrelated 3D client's frame.
            PHYSICAL_ADDRESS zeroBltAddr = {};
            if (srcAlloc && srcAlloc->IsPrimary())
                m_pAdapter->vidpn.SetScanoutSource(srcAlloc, zeroBltAddr, 0);
        }
        // Re-flush the scanout when the blt writes into the resource being
        // scanned out (GDI shared-primary model; see RearmFlipIfScanout).
        if (dst)
        {
            VioGpuAllocation *dstBltAlloc = dst->GetAllocation();
            if (dstBltAlloc)
            {
                m_pAdapter->vidpn.RearmFlipIfScanout(dstBltAlloc);
            }
        }
        if (pPresent->pDmaBuffer && dst && src)
        {
            char *bodyStart = (char *)pPresent->pDmaBuffer;
            if (true /*m_Context.IsVirgl()*/) {
                GenerateBltPresent(pPresent, src, dst);
            } else {
                GenerateBltPresentUM(pPresent, src->GetAllocation(), dst->GetAllocation());
            }
            // Carry the just-built body to SubmitCommandVirtual by
            // value (the submit DDI only sees a GPU VA in the virtual model).
            cmd->MirrorBody(pPresent->pDmaBufferPrivateData,
                            pPresent->DmaBufferPrivateDataSize,
                            bodyStart,
                            (char *)pPresent->pDmaBuffer - bodyStart);
        }
        return STATUS_SUCCESS;
    }

    // Only Blt and Flip are implemented in Present. Returning
    // NOT_SUPPORTED for ColorFill / SrcColorKey / DstColorKey /
    // LinearToSrgb / Rotate / FlipWithNoWait lets the UMD fall back
    // to a Render-based path; a silent NOP + SUCCESS would not.
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("%s unsupported Present flags=0x%x\n", __FUNCTION__,
              pPresent->Flags.Value));
    if (pPresent->pDmaBufferPrivateData)
    {
        VioGpuCommand **privateData = (VioGpuCommand **)pPresent->pDmaBufferPrivateData;
        if (*privateData == cmd)
        {
            *privateData = NULL;
        }
    }
    delete cmd;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS VioGpuDevice::Render(DXGKARG_RENDER *pRender)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    // See Present: recycled DMA private data must never carry a stale
    // command pointer into SubmitCommand (multipass/error exits below
    // return before the real write).
    // Stamp as ours (see Present).
    VioGpuDmaPrivClaim(pRender->pDmaBufferPrivateData, pRender->DmaBufferPrivateDataSize);

    char *pDmaBufStart = (char *)pRender->pDmaBuffer;

    __try
    {
        // Bound on PatchLocationListOutSize (the OUT capacity) so a UMD
        // supplying more IN entries than OUT slots cannot overrun the
        // kernel buffer. The count of populated entries is signalled to
        // DxgK by advancing pPatchLocationListOut, per the DDI.
        UINT cPatch = min(pRender->PatchLocationListInSize, pRender->PatchLocationListOutSize);
        for (UINT i = 0; i < cPatch; i++)
        {
            D3DDDI_PATCHLOCATIONLIST *out = &pRender->pPatchLocationListOut[0];
            RtlZeroMemory(out, sizeof(*out));
            out->AllocationIndex = pRender->pPatchLocationListIn[i].AllocationIndex;
            out->SlotId = i;
            pRender->pPatchLocationListOut++;
        }

        unsigned char *dmaBuf = (unsigned char *)pRender->pDmaBuffer;
        unsigned char *endDmaBuf = dmaBuf + pRender->DmaSize;
        unsigned char *cmdBuf = (unsigned char *)pRender->pCommand;
        unsigned char *endBuf = cmdBuf + pRender->CommandLength;
        while (cmdBuf < endBuf)
        {
            if (cmdBuf + sizeof(VIOGPU_COMMAND_HDR) > endBuf)
            {
                return STATUS_INVALID_USER_BUFFER;
            }
            if (dmaBuf + sizeof(VIOGPU_COMMAND_HDR) > endDmaBuf)
            {
                pRender->MultipassOffset = (UINT)(cmdBuf - (unsigned char *)pRender->pCommand);
                return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
            }

            memcpy(dmaBuf, cmdBuf, sizeof(VIOGPU_COMMAND_HDR));
            VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
            cmdBuf += sizeof(VIOGPU_COMMAND_HDR);
            dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

            if (cmdBuf + cmdHdr->size > endBuf)
            {
                return STATUS_INVALID_USER_BUFFER;
            }
            if (dmaBuf + cmdHdr->size > endDmaBuf)
            {
                pRender->MultipassOffset =
                    (UINT)(cmdBuf - sizeof(VIOGPU_COMMAND_HDR) - (unsigned char *)pRender->pCommand);
                return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
            }

            // Copy command body
            memcpy(dmaBuf, cmdBuf, cmdHdr->size);
            dmaBuf += cmdHdr->size;
            cmdBuf += cmdHdr->size;
        }
        pRender->pDmaBuffer = dmaBuf;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("<---> %s Usermode copy exception", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
    if (!cmd)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s VioGpuCommand allocation failed\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }
    if (pRender->pDmaBuffer)
    {
        void **privateData = (void **)pRender->pDmaBufferPrivateData;
        *privateData = cmd->ToHandle();
    }
    cmd->SetDmaBuf(pDmaBufStart);
    cmd->AttachAllocations(pRender->pAllocationList, pRender->AllocationListSize);

    // Mirror the built body for SubmitCommandVirtual.  A render
    // that exceeds the mirror window is captured on the command here (the
    // DMA buffer is still CPU-valid) so PrepareSubmitVirtual can execute
    // it from the heap copy rather than dropping to fence-only.
    cmd->MirrorBody(pRender->pDmaBufferPrivateData,
                    pRender->DmaBufferPrivateDataSize,
                    pDmaBufStart,
                    (char *)pRender->pDmaBuffer - pDmaBufStart);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
};

NTSTATUS VioGpuDevice::OpenAllocation(_In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation)
{
    PAGED_CODE();
    // Flags.Create=1 marks opens that ride the create call.
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("---> %s n=%u flags=0x%x\n", __FUNCTION__,
              pOpenAllocation->NumAllocations, pOpenAllocation->Flags.Value));

    for (UINT i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        DXGK_OPENALLOCATIONINFO *openAllocationInfo = &pOpenAllocation->pOpenAllocation[i];
        VioGpuAllocation *allocation = NULL;
        if (openAllocationInfo->pPrivateDriverData != NULL &&
            openAllocationInfo->PrivateDriverDataSize >= sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE))
        {
            // Cookie resolution comes BEFORE the pending-create FIFO: dxgkrnl
            // replays the create-time private data at every open, so the
            // cookie is authoritative, whereas the per-thread FIFO mispairs
            // when a thread interleaves two creates before their opens and
            // hands two devices in one process each other's blobs.  It also
            // covers cross-process opens, which the FIFO cannot see at all.
            // The cookie is either the trailing EX cookie or, for shared
            // textures, a key derived from (create_ctx_id, blob_id).
            VIOGPU_CREATE_ALLOCATION_EXCHANGE *exchange =
                (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)openAllocationInfo->pPrivateDriverData;
            ULONGLONG cookie = 0;
            if (openAllocationInfo->PrivateDriverDataSize >= sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX))
            {
                cookie = ((VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX *)exchange)->LookupCookie;
            }
            if (cookie == 0 && exchange->Type == VIOGPU_RESOURCE_TYPE_SHARED)
            {
                cookie = VioGpuSharedTexCookie(exchange->OptionsShared.create_ctx_id,
                                               exchange->OptionsShared.blob_id);
            }
            if (cookie != 0)
            {
                allocation = m_pAdapter->CookieMapLookup(cookie);
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<---> %s hAllocation=%x cookie=%llx -> alloc=%p\n",
                          __FUNCTION__, openAllocationInfo->hAllocation, cookie, allocation));
                if (allocation != NULL)
                {
                    // Keep the FIFO consistent for allocations that lack
                    // cookies: this open no longer consumes its entry.
                    m_pAdapter->PendingCreateRemove(allocation);
                }
            }
        }
        // Handle resolution is only a FALLBACK: a stale KMT-map entry with a
        // reused handle value resolves silently to the WRONG allocation,
        // leaving the real one with no paired open and its deferred
        // CreateBlob unrun, so RES_INFO reports the blob as not created.
        if (allocation == NULL)
        {
            allocation = m_pAdapter->AllocationFromHandle(openAllocationInfo->hAllocation);
        }
        if (allocation == NULL && pOpenAllocation->Flags.Create)
        {
            // Last resort for allocations that carry no cookie: pair with the
            // allocation this thread just created.
            allocation = m_pAdapter->PendingCreatePop();
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("<---> %s hAllocation=%x paired via pending-create -> alloc=%p\n",
                      __FUNCTION__, openAllocationInfo->hAllocation, allocation));
        }
        // VidMm-internal allocations (DMA pool buffers) never pass through
        // DxgkDdiCreateAllocation, so there is no driver object behind the
        // handle.  Opening one on a device is legal: hand back a NULL
        // device-specific handle, which every consumer already null-checks,
        // instead of virtual-calling through a NULL this.
        if (allocation == NULL)
        {
            // Only an error for UMD-owned allocations, where the deferred blob
            // create then silently never happens and the UMD device init
            // loops; VidMm-internal opens are routine.
            DbgPrint(openAllocationInfo->PrivateDriverDataSize != 0 ? TRACE_LEVEL_ERROR : TRACE_LEVEL_VERBOSE,
                     ("<---> %s hAllocation=%x privsize=%u priv=%p no driver allocation; NULL device handle\n",
                      __FUNCTION__, openAllocationInfo->hAllocation,
                      openAllocationInfo->PrivateDriverDataSize,
                      openAllocationInfo->pPrivateDriverData));
            openAllocationInfo->hDeviceSpecificAllocation = NULL;
            continue;
        }
        VioGpuDeviceAllocation *devAlloc = allocation->Open(this);
        openAllocationInfo->hDeviceSpecificAllocation = devAlloc->ToHandle();
        // Remember the D3DKMT-handle binding: escapes and later DDIs
        // resolve through it when GetHandleData draws a blank (WDDM2).
        // Opening the same allocation twice on one device returns the same
        // device-allocation under a second handle, so bind whatever handle
        // this open carries rather than only the first: an unbound handle
        // misses KmtMapLookup and the UMD's escape fails with no way to
        // reach the res_id.  KmtMapInsert displaces any stale entry for the
        // value, and the allocation's teardown drops every binding it owns.
        if (openAllocationInfo->hAllocation != 0 &&
            devAlloc->m_hKmtAllocation != openAllocationInfo->hAllocation)
        {
            devAlloc->m_hKmtAllocation = openAllocationInfo->hAllocation;
            m_pAdapter->KmtMapInsert(openAllocationInfo->hAllocation, allocation);
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

CtrlQueue *VioGpuDevice::GetCtrlQueue()
{
    PAGED_CODE();

    return &m_pAdapter->ctrlQueue;
}

VioGpuDeviceAllocation::VioGpuDeviceAllocation(VioGpuDevice *device, VioGpuAllocation *allocation)
{
    PAGED_CODE();

    //auto lock_guard = allocation->LockGuard();

    m_pAllocation = allocation;
    m_pDevice = device;
    m_RefCount = 1;
    m_attached = false;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d ctx=%p\n",
                                   __FUNCTION__,
                                   allocation->GetId(),
                                   device->m_Context.GetId()));

    if (m_pAllocation->IsBlob() && !m_pAllocation->IsCreated())
    {
        // Shared-texture blobs bind on the UMD transport context that
        // staged the pending dmabuf export (m_CreateCtxId); transport
        // shmem blobs (0) bind on the opening device's own context.
        UINT create_ctx = m_pAllocation->m_CreateCtxId
                              ? m_pAllocation->m_CreateCtxId
                              : m_pDevice->m_Context.GetId();
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d ctx_id=%d capset=%d blob_id=%llu creating blob resource\n",
                                       __FUNCTION__,
                                       allocation->GetId(),
                                       create_ctx,
                                       device->m_Context.GetCapset(),
                                       allocation->m_Blob.Options.blob_id));
        bool ok = m_pDevice->GetCtrlQueue()->CreateResourceBlob(m_pAllocation->GetId(), create_ctx, &m_pAllocation->m_Blob.Options, m_pAllocation->m_Size);
        m_pAllocation->m_Blob.Created = ok;
    }

    // Attach the resource to the opening device's virtio context.  For a
    // cross-process open of a shared blob this is what forwards the host
    // dmabuf into the opener's render worker (proxy attach-forwarding).
    // A device that never issued VIOGPU_CTX_INIT has no host context to
    // attach to (the UMD's transport-context import rig covers the open
    // in that case); skip rather than name a nonexistent ctx.
    if (!m_pDevice->m_Context.IsEmpty())
    {
        m_pDevice->GetCtrlQueue()->CtxResource(true, m_pDevice->m_Context.GetId(), m_pAllocation->GetId());
        m_attached = true;
    }
    m_AttachedToVirgl = false;
}

VioGpuDeviceAllocation::~VioGpuDeviceAllocation()
{
    PAGED_CODE();

    if (m_RefCount != 0 || m_pDevice == NULL || m_pAllocation == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s INVALID devalloc: ref=%lld devalloc=%p alloc=%p dev=%p\n", __FUNCTION__, m_RefCount, this, m_pAllocation, m_pDevice));
        // Gated break only: with /debug on and no debugger responding, a raw
        // int3 parks every CPU in KiFreezeTargetExecution polling a dead
        // serial link.  The object is deliberately leaked rather than
        // double-freed; the log above is the record.
        VioGpuDbgBreak();
        return;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d ctx_id=%d\n",
                                   __FUNCTION__,
                                   m_pAllocation->GetId(),
                                   m_pDevice->m_Context.GetId()));


    if (m_pAllocation->IsMapped())
    {
        // This is a driver bug
        DbgPrint(TRACE_LEVEL_WARNING, ("---> %s res_id=%d UNREACHABLE blob is still mapped \n", __FUNCTION__, m_pAllocation->GetId()));
        // FIXME: cannot do this here
        //m_pAllocation->UnmapBlob(m_pDevice->m_Context.GetId(), NULL, NULL);
    }

    if (m_attached)
    {
        m_pDevice->GetCtrlQueue()->CtxResource(false, m_pDevice->m_Context.GetId(), m_pAllocation->GetId());
    }

    if (m_AttachedToVirgl)
    {
        m_pDevice->GetCtrlQueue()->CtxResource(false, m_pDevice->m_Virgl.GetId(), m_pAllocation->GetId());
    }
}

VioGpuAllocation *VioGpuDeviceAllocation::GetAllocation()
{
    PAGED_CODE();

    return m_pAllocation;
}

VioGpuDevice *VioGpuDeviceAllocation::GetDevice()
{
    PAGED_CODE();

    return m_pDevice;
}

PAGED_CODE_SEG_END

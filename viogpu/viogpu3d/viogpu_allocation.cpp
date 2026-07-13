#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "viogpu_allocation.h"
#include "viogpu_adapter.h"
#include "virgl_hw.h"

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_BLOB_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s BLOB\n", __FUNCTION__));

    m_adapter = adapter;
    m_Id = m_adapter->resourceIdr.GetId();
    m_IsImport = FALSE;
    m_IsPrimary = FALSE;
    m_IsShared = FALSE;
    memcpy(&m_Blob.Options, options, sizeof(*options));
    RtlZeroMemory(&m_Blob.Info, sizeof(m_Blob.Info));
    // TODO: find a way to make valid
    // Probably via escape or something
    m_Blob.InfoValid = FALSE;
    m_Blob.Created = FALSE;
    m_Size = size;
    m_IsBlob = TRUE;
    m_Blob.Mapped = FALSE;

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d blob_id=%lld\n", __FUNCTION__, m_Id, m_Blob.Options.blob_id));
}

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_3D_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s 3D\n", __FUNCTION__));

    m_adapter = adapter;
    m_Id = m_adapter->resourceIdr.GetId();
    m_IsImport = FALSE;
    m_IsPrimary = FALSE;
    m_IsShared = FALSE;
    memcpy(&m_3dOptions, options, sizeof(*options));
    m_Size = size;
    m_IsBlob = FALSE;

    // m_adapter->ctrlQueue.CreateResource(m_Id, m_options.format, m_options.width, m_options.height);
    m_adapter->ctrlQueue.CreateResource3D(m_Id, options);

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d 3D\n", __FUNCTION__, m_Id));
}

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_IMPORT_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s IMPORT res_id=%d\n", __FUNCTION__, options->res_id));

    m_adapter = adapter;
    // Adopt the existing host res_id; do NOT mint one. The owning allocation
    // (another process's shared-texture blob) created the resource on its
    // context and owns the id's lifetime.
    m_Id = options->res_id;
    m_IsImport = TRUE;
    m_IsPrimary = FALSE;
    m_IsShared = FALSE;

    // Behave as a host-backed HOST3D blob so DxgkCreateAllocation's segment
    // selection treats it exactly like the dmabuf it aliases.
    RtlZeroMemory(&m_Blob.Options, sizeof(m_Blob.Options));
    m_Blob.Options.blob_mem = VIOGPU_BLOB_MEM_HOST3D;
    RtlZeroMemory(&m_Blob.Info, sizeof(m_Blob.Info));
    m_Blob.MapOffset = 0;
    m_Blob.InfoValid = FALSE;
    // The host resource already exists (created on the owning context), so
    // Open() must not re-issue RESOURCE_CREATE_BLOB.
    m_Blob.Created = TRUE;
    m_Blob.Mapped = FALSE;
    m_Size = size;
    m_IsBlob = TRUE;

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s IMPORT res_id=%d size=%lld\n", __FUNCTION__, m_Id, size));
}

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s SHARED blob_id=0x%llx %dx%d primary=%d modifier=0x%llx\n", __FUNCTION__,
                                   options->blob_id, options->width, options->height,
                                   options->primary, options->modifier));

    m_adapter = adapter;
    // Blob-backed shared texture: mint the res_id now; the host binding
    // (RESOURCE_CREATE_BLOB on the UMD's transport context, which staged
    // the pending dmabuf export under blob_id) happens when the creating
    // device opens the allocation.
    m_Id = m_adapter->resourceIdr.GetId();
    m_IsImport = FALSE;
    m_IsPrimary = !!options->primary;
    m_IsShared = TRUE;
    m_SharedWidth = options->width;
    m_SharedHeight = options->height;
    m_SharedFormat = options->format;
    m_BlobModifier = options->modifier;
    m_CreateCtxId = options->create_ctx_id;
    m_IsBlob = TRUE;
    RtlZeroMemory(&m_Blob.Options, sizeof(m_Blob.Options));
    m_Blob.Options.blob_mem = VIOGPU_BLOB_MEM_HOST3D;
    m_Blob.Options.blob_flags = VIOGPU_BLOB_FLAG_USE_SHAREABLE;
    m_Blob.Options.blob_id = options->blob_id;
    m_Blob.Info = options->ScanoutInfo;
    m_Blob.InfoValid = options->ScanoutInfo.width != 0;
    m_Blob.MapOffset = 0;
    m_Blob.Created = FALSE;
    m_Blob.Mapped = FALSE;
    m_Size = size;

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s SHARED res_id=%d blob_id=0x%llx size=%lld\n", __FUNCTION__,
                                   m_Id, options->blob_id, size));
}

void VioGpuAllocation::AddRef()
{
    InterlockedIncrement(&m_refCount);
}

void VioGpuAllocation::Release()
{
    LONG newCount = InterlockedDecrement(&m_refCount);
    if (newCount < 0)
    {
        // Underflow indicates double-Release somewhere. ASSERT trips
        // in DBG; in retail, leak the object rather than free freed
        // memory.
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s refcount underflow alloc=%p count=%d\n",
                  __FUNCTION__, this, newCount));
        ASSERT(newCount >= 0);
        return;
    }
    if (newCount == 0)
    {
        delete this;
    }
}

void VioGpuAllocation::ReleaseDeferred()
{
    // The destructor tears down LinkedList<VioGpuDeviceAllocation>, whose
    // entries' dtor is PAGED_CODE(). A Release that drops the last ref
    // from a DPC (or higher) would trip that contract. At raised IRQL,
    // hand the Release to the pre-allocated work item so the destructor
    // lands at PASSIVE_LEVEL.
    if (m_deferReleaseItem && KeGetCurrentIrql() >= DISPATCH_LEVEL)
    {
        IoQueueWorkItem(m_deferReleaseItem,
                        VioGpuAllocation::DeferredReleaseWorker,
                        DelayedWorkQueue,
                        this);
    }
    else
    {
        Release();
    }
}

VOID NTAPI VioGpuAllocation::DeferredReleaseWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    VioGpuAllocation *alloc = reinterpret_cast<VioGpuAllocation *>(Context);
    alloc->Release();
}

void NotifyResourceDestroyed(void *ctx, void *cmd, void *)
{
    VioGpuIdr *resIdr = reinterpret_cast<VioGpuIdr *>(ctx);
    PGPU_RES_UNREF unref_cmd = reinterpret_cast<PGPU_RES_UNREF>(cmd);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s freeing res_id=%d\n", __FUNCTION__, unref_cmd->resource_id));
    resIdr->PutId(unref_cmd->resource_id);
}

VioGpuAllocation::~VioGpuAllocation(void)
{
    // m_DeviceAllocations.clear() invokes ~VioGpuDeviceAllocation, which is
    // PAGED_CODE(). Any caller that drops the last ref from IRQL >=
    // DISPATCH_LEVEL must funnel through ReleaseDeferred so the destructor
    // lands here at PASSIVE_LEVEL.
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d alloc=%p size=%zu\n", __FUNCTION__, m_Id, m_IsBlob, this, m_DeviceAllocations.size()));

    if (m_deferReleaseItem)
    {
        IoFreeWorkItem(m_deferReleaseItem);
        m_deferReleaseItem = NULL;
    }

    m_DeviceAllocations.clear();

    if (m_IsBlob && m_Blob.Mapped)
    {
        // Orphaned mapping (process died without the UMD unmap).  Do NOT
        // send UNMAP_BLOB here: back-to-back unmap+destroy re-triggers the
        // QEMU async-unmap/destroy race -- as a main-loop wedge now
        // (cmdq suspended forever on the unmap, observed 17:45 2026-07-04)
        // rather than the historical abort.  The DISCARD_CONTENT paging op
        // (BuildPagingBuffer) performs the ordered host unmap when VidMm
        // actually frees the range; the destroy below is safe because the
        // host keeps blob memory alive until RES_UNREF.
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("---> %s res_id=%d still-mapped blob at destroy (host unmap deferred to DISCARD)\n",
                  __FUNCTION__, m_Id));
        m_Blob.Mapped = FALSE;
    }

    if (m_IsImport)
    {
        // The adopted res_id is owned by another allocation (the transport
        // device's swapchain claim). RES_UNREF + PutId here would free a host
        // resource that is still referenced and double-free the id; the owning
        // allocation's destructor performs the single destroy. m_DeviceAllocations
        // was already cleared above, detaching the id from this device's context.
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s IMPORT res_id=%d: skip DestroyResource (not owner)\n", __FUNCTION__, m_Id));
    }
    else
    {
        m_adapter->ctrlQueue.DestroyResource(m_Id, NotifyResourceDestroyed, &m_adapter->resourceIdr);
        // m_adapter->resourceIdr.PutId(m_Id);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

_IRQL_requires_max_(APC_LEVEL) VOID VioGpuAllocation::Lock()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    ExAcquireFastMutex(&m_Lock);
}

_IRQL_requires_max_(APC_LEVEL) VOID VioGpuAllocation::Unlock()
{
    ExReleaseFastMutex(&m_Lock);
}

void VioGpuAllocation::AttachBacking(MDL *pMDL, size_t pageCount, size_t pageOffset)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d, IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    auto lock_guard = LockGuard();

    m_pMDL = pMDL;
    m_pageCount = pageCount;
    m_pageOffset = pageOffset;

    GPU_MEM_ENTRY *ents = new (NonPagedPoolNx) GPU_MEM_ENTRY[pageCount];

    for (UINT i = 0; i < pageCount; i++)
    {
        ents[i].addr = MmGetMdlPfnArray(pMDL)[pageOffset + i] * PAGE_SIZE;
        ents[i].length = PAGE_SIZE;
        ents[i].padding = 0;
    }

    m_adapter->ctrlQueue.AttachBacking(m_Id, ents, (UINT)pageCount);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void VioGpuAllocation::DetachBacking()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    auto lock_guard = LockGuard();

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;

    m_adapter->ctrlQueue.DetachBacking(m_Id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VOID VioGpuAllocation::CreateBlob(UINT ctx_id)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    auto lock_guard = LockGuard();

    if (IsCreated()) return;

    bool ok = m_adapter->ctrlQueue.CreateResourceBlob(m_Id, ctx_id, &m_Blob.Options, m_Size);
    m_Blob.Created = ok;
}

BOOLEAN VioGpuAllocation::MapBlobLocked(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx)
{
    m_adapter->ctrlQueue.ResourceMapBlob(m_Id, ctx_id, m_Blob.MapOffset, complete_cb, complete_ctx);
    m_Blob.Mapped = TRUE;
    return TRUE;
}

BOOLEAN VioGpuAllocation::UnmapBlobLocked(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx)
{
    // The host treats RESOURCE_UNMAP_BLOB as a per-(res_id) operation rather
    // than per-(res_id, ctx_id): the first UNMAP releases the host mapping,
    // any subsequent UNMAP — whether from a cross-context sharer (TYPE_IMPORT
    // adopting the same res_id) or a duplicate Close path — returns
    // INVALID_RESOURCE_ID. Gating on the local Mapped flag matches the host
    // semantics and makes Close idempotent across cross-context sharers.
    if (!m_Blob.Mapped) return FALSE;
    if (m_Id == 0)
    {
        // Phantom allocations (shared-registry backing) never created a
        // host blob; an UNMAP_BLOB for res_id 0 is the guest-error spam
        // in the QEMU log.  Clear local state and skip the wire op.
        m_Blob.Mapped = FALSE;
        return FALSE;
    }
    m_adapter->ctrlQueue.ResourceUnmapBlob(m_Id, ctx_id, complete_cb, complete_ctx);
    m_Blob.Mapped = FALSE;
    return TRUE;
}

BOOLEAN VioGpuAllocation::MapBlob(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    if (!m_IsBlob) return FALSE;

    // m_Blob.Mapped is mutated under m_Lock by MapBlobLocked /
    // UnmapBlobLocked, so the already-mapped check must run under
    // the same lock to avoid racing a concurrent unmap into a stale
    // skip-the-map decision. Already mapped: no host command issued, so
    // complete_cb will not fire -- report FALSE so the caller's pending
    // count stays balanced.
    auto lock_guard = LockGuard();
    if (m_Blob.Mapped) return FALSE;
    return MapBlobLocked(ctx_id, complete_cb, complete_ctx);
}

BOOLEAN VioGpuAllocation::UnmapBlob(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    if (!m_IsBlob) return FALSE;

    auto lock_guard = LockGuard();
    if (!m_Blob.Mapped) return FALSE;
    return UnmapBlobLocked(ctx_id, complete_cb, complete_ctx);
}

VioGpuAllocationLockGuard::VioGpuAllocationLockGuard(VioGpuAllocation *allocation) : m_Allocation(allocation)
{
    m_Allocation->Lock();
}

VioGpuAllocationLockGuard::~VioGpuAllocationLockGuard()
{
    m_Allocation->Unlock();
}

PAGED_CODE_SEG_BEGIN

void VioGpuAllocation::MarkBusy()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s res_id=%d\n", __FUNCTION__, m_Id));

    // Serialize counter + event mutation under m_busyLock. Without it,
    // an interleaving where UnmarkBusy decrements to 0 and SetEvent's,
    // then MarkBusy increments and ClearEvent's, would leave the
    // counter > 0 with the event signalled -- causing EscapeResourceBusy
    // to busy-spin waking immediately on every check.
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_busyLock, &oldIrql);
    InterlockedIncrement(&m_busy);
    KeClearEvent(&m_busyNotification);
    KeReleaseSpinLock(&m_busyLock, oldIrql);
}

void VioGpuAllocation::UnmarkBusy()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s res_id=%d\n", __FUNCTION__, m_Id));

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_busyLock, &oldIrql);
    LONG remaining = InterlockedDecrement(&m_busy);
    if (remaining < 0)
    {
        // Underflow: more UnmarkBusy than MarkBusy. Clamp back to 0
        // and signal so a waiter doesn't see a permanently-negative
        // counter. DBG trips the assert below.
        InterlockedExchange(&m_busy, 0);
        KeSetEvent(&m_busyNotification, IO_NO_INCREMENT, FALSE);
    }
    else if (remaining == 0)
    {
        KeSetEvent(&m_busyNotification, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&m_busyLock, oldIrql);

    if (remaining < 0)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s busy underflow res_id=%d remaining=%d\n",
                  __FUNCTION__, m_Id, remaining));
    }
    ASSERT(remaining >= 0);
}

D3DDDIFORMAT VioGpuToD3DDDIColorFormat(virtio_gpu_formats format)
{
    PAGED_CODE();

    switch (format)
    {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
            return D3DDDIFMT_A8R8G8B8;
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
            return D3DDDIFMT_X8R8G8B8;
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
            return D3DDDIFMT_A8B8G8R8;
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return D3DDDIFMT_X8B8G8R8;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return D3DDDIFMT_X8B8G8R8;
}

void VioGpuAllocation::FlushToScreen(UINT scan_id)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    if (m_IsBlob) {
        // Snapshot under the allocation lock so a concurrent info update
        // cannot pair a pre-write rect with post-write framebuffer fields
        // in one scanout command.
        VIOGPU_BLOB_INFO info;
        BOOL infoValid;
        {
            auto lock_guard = LockGuard();
            info = m_Blob.Info;
            infoValid = m_Blob.InfoValid;
        }

        DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s scanout blob res_id=%d valid=%d %dx%d\n", __FUNCTION__, m_Id, infoValid, info.width, info.height));

        // A blob whose info has not been published yet has zero
        // dimensions; the host rejects a degenerate scanout rect, so
        // hold off the scanout until the info lands (the next flip
        // retries with the same resource).
        if (!infoValid || info.width < 16 || info.height < 16) {
            DbgPrint(TRACE_LEVEL_INFORMATION,
                     ("---> %s deferring scanout of blob res_id=%d without valid info\n", __FUNCTION__, m_Id));
            return;
        }

        GPU_RECT rect;
        rect.x = 0;
        rect.y = 0;
        rect.width = info.width;
        rect.height = info.height;

        m_adapter->ctrlQueue.SetScanoutBlob(scan_id, m_Id, rect, info);
        // TODO: guard with IsGuest()
        m_adapter->ctrlQueue.ResFlush(m_Id, rect);
    } else {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s scanout 3d res_id=%d %dx%d\n", __FUNCTION__, m_Id, m_3dOptions.width, m_3dOptions.height));
        m_adapter->ctrlQueue.SetScanout(scan_id, m_Id, m_3dOptions.width, m_3dOptions.height, 0, 0);
        m_adapter->ctrlQueue.ResFlush(m_Id, m_3dOptions.width, m_3dOptions.height, 0, 0);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d\n", __FUNCTION__, m_Id));
}

NTSTATUS VioGpuAllocation::GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s type=%d\n", __FUNCTION__, pStandardAllocation->StandardAllocationType));

    if (!pStandardAllocation->pResourcePrivateDriverData || !pStandardAllocation->pAllocationPrivateDriverData)
    {
        pStandardAllocation->ResourcePrivateDriverDataSize = sizeof(VIOGPU_CREATE_RESOURCE_EXCHANGE);
        pStandardAllocation->AllocationPrivateDriverDataSize = sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE);
        return STATUS_SUCCESS;
    }

    VIOGPU_CREATE_ALLOCATION_EXCHANGE *allocationExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)pStandardAllocation->pAllocationPrivateDriverData;

    // TODO: make this work with blob
    allocationExchange->Type = VIOGPU_RESOURCE_TYPE_3D;

    allocationExchange->Options3D.target = 2;
    allocationExchange->Options3D.format = VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    allocationExchange->Options3D.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                                         VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT;

    allocationExchange->Options3D.width = 1024;
    allocationExchange->Options3D.height = 768;
    allocationExchange->Options3D.depth = 1;

    allocationExchange->Options3D.array_size = 1;
    allocationExchange->Options3D.last_level = 0;
    allocationExchange->Options3D.nr_samples = 0;
    allocationExchange->Options3D.flags = 0;

    switch (pStandardAllocation->StandardAllocationType)
    {
        case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
            {
                D3DKMDT_SHAREDPRIMARYSURFACEDATA *surfaceData = pStandardAllocation->pCreateSharedPrimarySurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;
                //[in] D3DDDIFORMAT                   Format;

                allocationExchange->Options3D.width = surfaceData->Width;
                allocationExchange->Options3D.height = surfaceData->Height;
                allocationExchange->Options3D.format = ColorFormat(surfaceData->Format);
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s shared primary surface: width=%d, height=%d, format=%d\n",
                          __FUNCTION__,
                          surfaceData->Width,
                          surfaceData->Height,
                          surfaceData->Format));
                return STATUS_SUCCESS;
            }

        case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
            {
                D3DKMDT_SHADOWSURFACEDATA *surfaceData = pStandardAllocation->pCreateShadowSurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;
                //[in] D3DDDIFORMAT                   Format;

                allocationExchange->Options3D.width = surfaceData->Width;
                allocationExchange->Options3D.height = surfaceData->Height;
                allocationExchange->Options3D.format = ColorFormat(surfaceData->Format);
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                allocationExchange->Options3D.flags |= VIRGL_RESOURCE_FLAG_MAP_COHERENT;

                surfaceData->Pitch = surfaceData->Width * 4;
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s shadow surface: width=%d, height=%d, format=%d\n",
                          __FUNCTION__,
                          surfaceData->Width,
                          surfaceData->Height,
                          surfaceData->Format));
                return STATUS_SUCCESS;
            }

        case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
            {
                D3DKMDT_STAGINGSURFACEDATA *surfaceData = pStandardAllocation->pCreateStagingSurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;
                //[in] D3DDDIFORMAT                   Format;

                allocationExchange->Options3D.width = surfaceData->Width;
                allocationExchange->Options3D.height = surfaceData->Height;
                allocationExchange->Options3D.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                allocationExchange->Options3D.flags |= VIRGL_RESOURCE_FLAG_MAP_COHERENT;

                surfaceData->Pitch = surfaceData->Width * 4;
                DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s staging surface\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_FATAL, ("<--- Unknown standard allocation type \n"));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuAllocation::DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    DXGK_ALLOCATIONINFO *allocationInfo = pCreateAllocation->pAllocationInfo;


    if (max(allocationInfo->PrivateDriverDataSize, pCreateAllocation->PrivateDriverDataSize) <
        sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s private driver data is too small (%d and %d < %zu)\n",
                                     __FUNCTION__,
                                     allocationInfo->PrivateDriverDataSize,
                                     pCreateAllocation->PrivateDriverDataSize,
                                     sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE)));
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_CREATE_ALLOCATION_EXCHANGE *resourceExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)allocationInfo->pPrivateDriverData;

    if (pCreateAllocation->PrivateDriverDataSize > allocationInfo->PrivateDriverDataSize)
    {
        resourceExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)pCreateAllocation->pPrivateDriverData;
    }

    VioGpuAllocation *allocation = NULL;

    switch (resourceExchange->Type) {
        case VIOGPU_RESOURCE_TYPE_3D:
            allocation = new (NonPagedPoolNx) VioGpuAllocation(adapter, &resourceExchange->Options3D, resourceExchange->Size);
            break;
        case VIOGPU_RESOURCE_TYPE_BLOB:
            // Actual resource creation is deferred to a later time (render)
            allocation = new (NonPagedPoolNx) VioGpuAllocation(adapter, &resourceExchange->OptionsBlob, resourceExchange->Size);
            break;
        case VIOGPU_RESOURCE_TYPE_IMPORT:
            // Adopts an existing host res_id; no mint, no RESOURCE_CREATE_BLOB.
            allocation = new (NonPagedPoolNx) VioGpuAllocation(adapter, &resourceExchange->OptionsImport, resourceExchange->Size);
            break;
        case VIOGPU_RESOURCE_TYPE_SHARED:
            // Blob-backed shared/presentable texture (dmabuf export staged by
            // the UMD; res_id minted here, bound at first open).
            allocation = new (NonPagedPoolNx) VioGpuAllocation(adapter, &resourceExchange->OptionsShared, resourceExchange->Size);
            break;
        default:
            DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s invalid resource type %d\n", __FUNCTION__, resourceExchange->Type));
            return STATUS_INVALID_PARAMETER;
    }

    allocationInfo->hAllocation = allocation->ToHandle();

    if (pCreateAllocation->Flags.Resource)
    {
        VioGpuResource *resource = new (NonPagedPoolNx) VioGpuResource();
        pCreateAllocation->hResource = resource->ToHandle();
    }

    allocationInfo->Alignment = 0;
    allocationInfo->Size = (SIZE_T)resourceExchange->Size;
    allocationInfo->PitchAlignedSize = 0;
    allocationInfo->HintedBank.Value = 0;
    // Control-ring blobs are CPU-mapped and polled for the device's lifetime;
    // if VidMm DISCARDs their segment-2 backing under VRAM pressure the guest
    // mapping zeroes and the ring wedges (status=0x0).  MAXIMUM priority makes
    // VidMm evict everything else first.
    allocationInfo->AllocationPriority = allocation->IsPinned()
                                             ? D3DDDI_ALLOCATIONPRIORITY_MAXIMUM
                                             : D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    allocationInfo->Flags.Value = 0;
    allocationInfo->MaximumRenamingListLength = 0;
    allocationInfo->pAllocationUsageHint = NULL;
    allocationInfo->PhysicalAdapterIndex = 0;

    allocationInfo->PreferredSegment.Value = 0;

    switch (resourceExchange->Type) {
        case VIOGPU_RESOURCE_TYPE_3D:
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s 3d res_id=%d size=%d %dx%d\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           allocationInfo->Size,
                                           resourceExchange->Options3D.width,
                                           resourceExchange->Options3D.height));
            allocationInfo->EvictionSegmentSet = 1; // don't use apperture for eviction
            allocationInfo->PreferredSegment.SegmentId0 = 1;
            allocationInfo->PreferredSegment.Direction0 = 0;
            allocationInfo->Flags.CpuVisible = TRUE;
            allocationInfo->SupportedReadSegmentSet = 0b1;
            allocationInfo->SupportedWriteSegmentSet = 0b1;
            break;
        case VIOGPU_RESOURCE_TYPE_BLOB:
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s blob res_id=%d size=%d\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           allocationInfo->Size));
            if (allocation->IsGuestBlob())
            {
                //allocationInfo->EvictionSegmentSet = 0b01; // don't use apperture for eviction
                allocationInfo->PreferredSegment.SegmentId0 = 1;
                allocationInfo->PreferredSegment.Direction0 = 0;
                allocationInfo->Flags.CpuVisible = TRUE;
                allocationInfo->SupportedReadSegmentSet = 0b1;
                allocationInfo->SupportedWriteSegmentSet = 0b1;
            }
            else
            {
                // EvictionSegmentSet lists the segments an allocation may be
                // evicted TO, which must be aperture/system memory. Segment 2
                // is the non-aperture host shmem BAR (the residence, not an
                // eviction target); naming it here is invalid and the video
                // memory manager rejects the allocation (STATUS_INVALID_-
                // PARAMETER). Leave it 0, matching the guest-blob branch:
                // these host-backed BAR blobs are effectively pinned.
                allocationInfo->PreferredSegment.SegmentId0 = 2;
                allocationInfo->PreferredSegment.Direction0 = 0;
                allocationInfo->Flags.CpuVisible = !!(resourceExchange->OptionsBlob.blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE);
                // AccessedPhysically + ExplicitResidencyNotification were both
                // rejected by the video memory manager here (STATUS_INVALID_-
                // PARAMETER on D3DKMTCreateAllocation, observed on the Neptune
                // ring). The working 3D/guest-blob branches set neither, so
                // mirror them: a CpuVisible allocation in the CpuVisible shmem
                // segment is addressed through its CpuTranslatedAddress.
                // allocationInfo->Flags.Swizzled = TRUE;
                allocationInfo->SupportedReadSegmentSet = 0b10;
                allocationInfo->SupportedWriteSegmentSet = 0b10;
            }
            break;
        case VIOGPU_RESOURCE_TYPE_IMPORT:
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s import res_id=%d size=%lld\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           allocationInfo->Size));
            // Host-backed alias of an existing dmabuf res_id: residency matches
            // a non-mappable HOST3D blob (host shmem BAR segment, pinned; not
            // CpuVisible -- the guest never maps the host dmabuf).
            allocationInfo->PreferredSegment.SegmentId0 = 2;
            allocationInfo->PreferredSegment.Direction0 = 0;
            allocationInfo->Flags.CpuVisible = FALSE;
            allocationInfo->SupportedReadSegmentSet = 0b10;
            allocationInfo->SupportedWriteSegmentSet = 0b10;
            break;
        case VIOGPU_RESOURCE_TYPE_SHARED:
            // Reside in the CPU-visible aperture (segment 1) with DXGK-supplied
            // backing pages, like a 3D allocation. The pages are never read --
            // the content is the host texture's dmabuf -- but segment 1 both
            // satisfies the video memory manager for a shareable allocation and
            // lets dxgkrnl accept a primary as a VidPnSource scanout target.
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s shared res_id=%d blob_id=0x%llx size=%d %dx%d primary=%d\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           resourceExchange->OptionsShared.blob_id,
                                           allocationInfo->Size,
                                           resourceExchange->OptionsShared.width,
                                           resourceExchange->OptionsShared.height,
                                           resourceExchange->OptionsShared.primary));
            allocationInfo->EvictionSegmentSet = 1;
            allocationInfo->PreferredSegment.SegmentId0 = 1;
            allocationInfo->PreferredSegment.Direction0 = 0;
            allocationInfo->Flags.CpuVisible = TRUE;
            allocationInfo->SupportedReadSegmentSet = 0b1;
            allocationInfo->SupportedWriteSegmentSet = 0b1;
            if (resourceExchange->OptionsShared.primary)
            {
                // Promote to the active scanout source as soon as the runtime
                // mints it: the runtime owns rotation between back buffers,
                // and the most recently created primary is what the runtime
                // is presenting from until the first flip latches a source.
                adapter->vidpn.SetScanoutSource(allocation);
            }
            break;
    }

    return STATUS_SUCCESS;
}

// Minimal DXGI_FORMAT -> D3DDDIFORMAT mapping for the shared-allocation
// describe path. Numeric DXGI values avoid an interface-only dxgiformat.h
// dependency in the miniport. Unknown formats fall back to A8R8G8B8, the
// layout DWM's shared surfaces use.
static D3DDDIFORMAT VioGpuDxgiFormatToD3DDDI(UINT dxgiFormat)
{
    switch (dxgiFormat) {
    case 87: /* DXGI_FORMAT_B8G8R8A8_UNORM */ return D3DDDIFMT_A8R8G8B8;
    case 88: /* DXGI_FORMAT_B8G8R8X8_UNORM */ return D3DDDIFMT_X8R8G8B8;
    case 28: /* DXGI_FORMAT_R8G8B8A8_UNORM */ return D3DDDIFMT_A8B8G8R8;
    default:                                  return D3DDDIFMT_A8R8G8B8;
    }
}

NTSTATUS VioGpuAllocation::DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    auto lock_guard = LockGuard();

    if (m_IsShared) {
        // Host-COM-backed share: dimensions/format come from the producer's
        // descriptor; there is no virtio resource to query.
        pDescribeAllocation->Width = m_SharedWidth;
        pDescribeAllocation->Height = m_SharedHeight;
        pDescribeAllocation->PrivateDriverFormatAttribute = 0;
        pDescribeAllocation->Format = VioGpuDxgiFormatToD3DDDI(m_SharedFormat);
        pDescribeAllocation->MultisampleMethod.NumQualityLevels = 0;
        pDescribeAllocation->MultisampleMethod.NumSamples = 1;
        pDescribeAllocation->RefreshRate.Numerator = 60;
        pDescribeAllocation->RefreshRate.Denominator = 1;
        return STATUS_SUCCESS;
    }

    if (m_IsBlob) {
        if (!m_Blob.InfoValid) {
            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s res_id=%d does not have valid info\n", __FUNCTION__, m_Id));
            return STATUS_INVALID_PARAMETER;
        }

        pDescribeAllocation->Width = m_Blob.Info.width;
        pDescribeAllocation->Height = m_Blob.Info.height;
        pDescribeAllocation->PrivateDriverFormatAttribute = 0;

        pDescribeAllocation->Format = VioGpuToD3DDDIColorFormat((virtio_gpu_formats)m_Blob.Info.format);
    } else {
        pDescribeAllocation->Width = m_3dOptions.width;
        pDescribeAllocation->Height = m_3dOptions.height;
        pDescribeAllocation->PrivateDriverFormatAttribute = 0;

        pDescribeAllocation->Format = VioGpuToD3DDDIColorFormat((virtio_gpu_formats)m_3dOptions.format);
    }

    // Multisample mirrors the resource's nr_samples: 0 or 1 means
    // non-MSAA, anything else is MSAA with a single quality level
    // (vendor-specific quality variants are not surfaced). Blob
    // resources don't expose nr_samples, so treat them as non-MSAA.
    UINT nr_samples = m_IsBlob ? 0 : m_3dOptions.nr_samples;
    if (nr_samples > 1)
    {
        pDescribeAllocation->MultisampleMethod.NumQualityLevels = 1;
        pDescribeAllocation->MultisampleMethod.NumSamples = (UCHAR)nr_samples;
    }
    else
    {
        pDescribeAllocation->MultisampleMethod.NumQualityLevels = 0;
        pDescribeAllocation->MultisampleMethod.NumSamples = 1;
    }

    // Refresh rate: active VidPN mode's rate if a source is pinned,
    // otherwise 60/1 as a safe default.
    D3DDDI_RATIONAL refresh = m_adapter->vidpn.GetActiveRefreshRate();
    if (refresh.Numerator && refresh.Denominator)
    {
        pDescribeAllocation->RefreshRate = refresh;
    }
    else
    {
        pDescribeAllocation->RefreshRate.Numerator = 60;
        pDescribeAllocation->RefreshRate.Denominator = 1;
    }

    return STATUS_SUCCESS;
};

NTSTATUS VioGpuAllocation::MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    size_t pageCount = pBuildPagingBuffer->MapApertureSegment.NumberOfPages;
    size_t mdlPageOffset = pBuildPagingBuffer->MapApertureSegment.MdlOffset;

    MDL *pMdl = pBuildPagingBuffer->MapApertureSegment.pMdl;

    if (!IsBlob() || IsGuestBlob()) {
        AttachBacking(pMdl, pageCount, mdlPageOffset);
        SetDxPhysicalAddress(pBuildPagingBuffer->MapApertureSegment.OffsetInPages * PAGE_SIZE);
        return STATUS_SUCCESS;
    }

    // Host-only blob (e.g. an IMPORT primary aliasing a host dmabuf): the
    // content lives host-side and nothing reads it through the aperture,
    // but VidMm still pages the allocation in when a DMA submission
    // references it (the flip present's fenced EXECBUF does).  Accept the
    // map as bookkeeping; failing it fails the paging operation, which
    // dxgkrnl escalates to a bugcheck.
    SetDxPhysicalAddress(pBuildPagingBuffer->MapApertureSegment.OffsetInPages * PAGE_SIZE);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    UNREFERENCED_PARAMETER(pBuildPagingBuffer);
    DetachBacking();
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d isBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    auto lock_guard = LockGuard();

    resInfo->Id = m_Id;
    resInfo->Size = m_Size;
    if (m_IsBlob)
    {
        resInfo->IsBlob = TRUE;
        resInfo->IsCreated = m_Blob.Created;
        resInfo->InfoValid = m_Blob.InfoValid;
        resInfo->BlobMem = m_Blob.Options.blob_mem;
        resInfo->BlobId = m_Blob.Options.blob_id;
        resInfo->Info = m_Blob.Info;
    }
    else
    {
        resInfo->IsBlob = FALSE;
    }

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    while (resBusy->Wait && m_busy != 0)
    {
        KeWaitForSingleObject(&m_busyNotification, UserRequest, KernelMode, FALSE, NULL);
    }

    resBusy->IsBusy = m_busy != 0;

    return STATUS_SUCCESS;
}

LinkedList<VioGpuDeviceAllocation>::Entry *VioGpuAllocation::Find(VioGpuDevice *pDevice)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return m_DeviceAllocations.find([pDevice](VioGpuDeviceAllocation *pDeviceAllocation) [[msvc::forceinline]]
    {
        return pDeviceAllocation->GetDevice() == pDevice;
    });
}

VioGpuDeviceAllocation *VioGpuAllocation::Open(VioGpuDevice *pDevice)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p\n", __FUNCTION__, this));
    auto lock_guard = LockGuard();

    auto pEntry = Find(pDevice);
    if (pEntry == nullptr)
    {
        m_DeviceAllocations.emplace_back(pDevice, this);
        pEntry = m_DeviceAllocations.back();
    }
    else
    {
        pEntry->value.Ref();
    }

    if (pEntry == nullptr)
    {
        VioGpuDbgBreak();
    }

    VioGpuDeviceAllocation *pDeviceAllocation = &pEntry->value;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p devalloc=%p ref=%lld dev=%p\n", __FUNCTION__, this, pDeviceAllocation, pDeviceAllocation->GetRef(), pDeviceAllocation->GetDevice()));

    return pDeviceAllocation;
}

void VioGpuAllocation::Close(VioGpuDeviceAllocation *pDeviceAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p devalloc=%p ref=%lld dev=%p\n", __FUNCTION__, this, pDeviceAllocation, pDeviceAllocation->GetRef(), pDeviceAllocation->GetDevice()));
    auto lock_guard = LockGuard();

    auto pEntry = Find(pDeviceAllocation->GetDevice());
    if (pEntry == nullptr || &pEntry->value != pDeviceAllocation)
    {
        VioGpuDbgBreak();
    }

    if (pEntry->value.Unref())
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p removing devalloc=%p size=%zu\n", __FUNCTION__, this, &pEntry->value, m_DeviceAllocations.size()));
        UnmapBlobLocked(pEntry->value.GetCtxId(), NULL, NULL);
        m_DeviceAllocations.remove(pEntry);
    }
}

PAGED_CODE_SEG_END

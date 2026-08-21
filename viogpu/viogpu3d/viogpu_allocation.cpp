#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "viogpu_allocation.h"
#include "viogpu_adapter.h"
#include "virgl_hw.h"

// KeStackAttachProcess / KAPC_STATE are declared only in ntifs.h, which
// cannot coexist with the ntddk.h this display miniport already pulls.
// Declare the ABI-stable prototypes locally (the import is matched by
// name and the pointer arguments are size-identical).  VIOGPU_KAPC_STATE
// mirrors the frozen KAPC_STATE layout with slack so KeStackAttachProcess
// never writes past its storage.
extern "C" {
typedef struct _VIOGPU_KAPC_STATE
{
    LIST_ENTRY ApcListHead[2];
    PVOID Process;
    UCHAR InProgressFlags;
    BOOLEAN KernelApcPending;
    BOOLEAN UserApcPendingAll;
    UCHAR Reserved[16];
} VIOGPU_KAPC_STATE;
NTKERNELAPI VOID KeStackAttachProcess(PEPROCESS Process, VIOGPU_KAPC_STATE *ApcState);
NTKERNELAPI VOID KeUnstackDetachProcess(VIOGPU_KAPC_STATE *ApcState);
}

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_BLOB_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s BLOB\n", __FUNCTION__));

    m_adapter = adapter;
    // A residency-only blob names no host resource: minting a res_id would
    // burn the id space at one id per D3D12 resource and then unref an id the
    // host never created.  Born "Created" so Open() never issues
    // RESOURCE_CREATE_BLOB either.
    const BOOLEAN residencyOnly =
        !!(options->blob_flags & VIOGPU_BLOB_FLAG_RESIDENCY_ONLY);
    m_Id = residencyOnly ? 0 : m_adapter->resourceIdr.GetId();
    m_IsImport = FALSE;
    m_IsPrimary = FALSE;
    m_IsShared = FALSE;
    memcpy(&m_Blob.Options, options, sizeof(*options));
    RtlZeroMemory(&m_Blob.Info, sizeof(m_Blob.Info));
    // TODO: find a way to make valid
    // Probably via escape or something
    m_Blob.InfoValid = FALSE;
    m_Blob.Created = residencyOnly ? TRUE : FALSE;
    m_Size = size;
    m_IsBlob = TRUE;
    m_Blob.Mapped = FALSE;

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_BackingAttached = FALSE;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());
    m_deferReleaseCount = 0;
    m_deferReleaseQueued = 0;
    KeInitializeDpc(&m_deferReleaseDpc, VioGpuAllocation::DeferredReleaseDpc, this);

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
    m_BackingAttached = FALSE;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());
    m_deferReleaseCount = 0;
    m_deferReleaseQueued = 0;
    KeInitializeDpc(&m_deferReleaseDpc, VioGpuAllocation::DeferredReleaseDpc, this);

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
    m_BackingAttached = FALSE;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());
    m_deferReleaseCount = 0;
    m_deferReleaseQueued = 0;
    KeInitializeDpc(&m_deferReleaseDpc, VioGpuAllocation::DeferredReleaseDpc, this);

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
    m_BackingAttached = FALSE;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());
    m_deferReleaseCount = 0;
    m_deferReleaseQueued = 0;
    KeInitializeDpc(&m_deferReleaseDpc, VioGpuAllocation::DeferredReleaseDpc, this);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s SHARED res_id=%d blob_id=0x%llx size=%lld\n", __FUNCTION__,
                                   m_Id, options->blob_id, size));
}

static BOOLEAN IsLinear32BppVirtioFormat(ULONG format)
{
    switch (format)
    {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return TRUE;
        default:
            return FALSE;
    }
}

BOOLEAN VioGpuAllocation::GetTransferLayout(LONG x, LONG y, ULONG *pStride, ULONGLONG *pOffset) const
{
    ULONG format;
    ULONG width;
    ULONG height;
    ULONG stride;
    ULONGLONG planeOffset;

    if (!pStride || !pOffset || x < 0 || y < 0)
        return FALSE;

    if (m_IsBlob)
    {
        if (!m_Blob.InfoValid)
            return FALSE;

        format = m_Blob.Info.format;
        width = m_Blob.Info.width;
        height = m_Blob.Info.height;
        stride = m_Blob.Info.strides[0];
        planeOffset = m_Blob.Info.offsets[0];
    }
    else
    {
        format = m_3dOptions.format;
        width = m_3dOptions.width;
        height = m_3dOptions.height;
        planeOffset = 0;

        ULONGLONG rowBytes = (ULONGLONG)width * 4;
        if (rowBytes > MAXULONG)
            return FALSE;
        stride = (ULONG)rowBytes;
    }

    if (!IsLinear32BppVirtioFormat(format) || !width || !height ||
        (ULONG)x > width || (ULONG)y > height ||
        stride < (ULONGLONG)width * 4)
        return FALSE;

    ULONGLONG offset = planeOffset + (ULONGLONG)(ULONG)y * stride + (ULONGLONG)(ULONG)x * 4;
    if (offset < planeOffset || (m_Size && offset > m_Size))
        return FALSE;

    *pStride = stride;
    *pOffset = offset;
    return TRUE;
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

// Requires IRQL <= DISPATCH_LEVEL. Only the caller that flips
// m_deferReleaseQueued gets to queue the single pre-allocated work item;
// the rest coalesce onto it through m_deferReleaseCount.
void VioGpuAllocation::QueueDeferredReleaseWorkItem()
{
    if (InterlockedCompareExchange(&m_deferReleaseQueued, 1, 0) == 0)
    {
        IoQueueWorkItem(m_deferReleaseItem,
                        VioGpuAllocation::DeferredReleaseWorker,
                        DelayedWorkQueue,
                        this);
    }
}

VOID NTAPI VioGpuAllocation::DeferredReleaseDpc(_KDPC *Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    reinterpret_cast<VioGpuAllocation *>(Context)->QueueDeferredReleaseWorkItem();
}

void VioGpuAllocation::ReleaseDeferred()
{
    // The destructor tears down LinkedList<VioGpuDeviceAllocation>, whose
    // entries' dtor is PAGED_CODE(). A Release that drops the last ref
    // from a DPC (or higher) would trip that contract, so at raised IRQL the
    // Release is punted to a work item that runs at PASSIVE_LEVEL.
    //
    // IoQueueWorkItem is itself only callable at IRQL <= DISPATCH_LEVEL, and
    // this function is reached above that: with FlipOnVSyncMmIo,
    // dxgmms1!VidSchiExecuteMmIoFlipAtISR invokes DdiSetVidPnSourceAddress
    // through KeSynchronizeExecution, i.e. at DIRQL holding the interrupt spin
    // lock. Queuing a work item from there deadlocks the machine in
    // nt!KiExitDispatcher -> HalpInterruptSendIpi ->
    // nt!KxWaitForSpinLockAndAcquire, and every other VidSch caller then piles
    // up behind the lock that is never released. A DPC may be queued from any
    // IRQL and runs at DISPATCH_LEVEL, so above DISPATCH_LEVEL hop through one
    // and let it queue the work item.
    if (!m_deferReleaseItem)
    {
        Release();
        return;
    }

    KIRQL irql = KeGetCurrentIrql();
    if (irql < DISPATCH_LEVEL)
    {
        Release();
        return;
    }

    InterlockedIncrement(&m_deferReleaseCount);

    if (irql > DISPATCH_LEVEL)
    {
        // KeInsertQueueDpc returning FALSE just means the DPC is already
        // queued; that pending DPC will drain the count we just bumped.
        KeInsertQueueDpc(&m_deferReleaseDpc, NULL, NULL);
    }
    else
    {
        QueueDeferredReleaseWorkItem();
    }
}

VOID NTAPI VioGpuAllocation::DeferredReleaseWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    VioGpuAllocation *alloc = reinterpret_cast<VioGpuAllocation *>(Context);

    // Claim the pending count before re-arming, never the other way round: a
    // ReleaseDeferred that bumps the count after the re-arm would queue a
    // second work item, and this one could meanwhile drop that reference and
    // destroy `alloc` out from under it. A count bumped in the window between
    // the two exchanges found the queue flag still set and so did not re-arm
    // itself; pick it up here.
    LONG pending = InterlockedExchange(&alloc->m_deferReleaseCount, 0);
    InterlockedExchange(&alloc->m_deferReleaseQueued, 0);
    if (alloc->m_deferReleaseCount != 0)
    {
        alloc->QueueDeferredReleaseWorkItem();
    }

    // `pending` is at least 1, so `alloc` is live for everything above; the
    // Releases below may free it, so touch no member past this point.
    while (pending-- > 0)
    {
        alloc->Release();
    }
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

    // A pending deferred release always holds a reference, so the DPC cannot
    // normally still be queued here; dequeue it anyway rather than leave a
    // callback pointing at freed memory.
    KeRemoveQueueDpc(&m_deferReleaseDpc);
    // Drop any stale pending-create entry (created but never opened) and
    // any KMT-map bindings a dead process left behind.
    m_adapter->PendingCreateRemove(this);
    m_adapter->KmtMapRemoveByAllocation(this);
    m_adapter->CookieMapRemoveByAllocation(this);

    // Tear down the KMD-owned shmem mappings (WDDM2).  A user VA must be
    // unmapped in the process it was mapped into before the BAR window is
    // recycled below, or the next blob placed there is aliased by a stale
    // mapping in that process.  A shared blob is legitimately RES_INFO'd
    // from several still-live processes (e.g. DWM), so this cannot assume
    // only the current process holds a mapping.
    for (KmdShmemMapping *m = m_KmdShmem.Maps; m != NULL;)
    {
        KmdShmemMapping *next = m->Next;
        if (m->UserVa != NULL)
        {
            if (m->Process == PsGetCurrentProcess())
            {
                MmUnmapLockedPages(m->UserVa, m->Mdl);
            }
            else if (PsGetProcessExitStatus(m->Process) == STATUS_PENDING)
            {
                // Another live process's mapping: unmap in its context.
                VIOGPU_KAPC_STATE apc;
                KeStackAttachProcess(m->Process, &apc);
                MmUnmapLockedPages(m->UserVa, m->Mdl);
                KeUnstackDetachProcess(&apc);
            }
            // else: the process exited; the OS already reclaimed its user VAs.
        }
        // No MmUnlockPages: the PFN array was hand-built over the BAR
        // range (nothing was probe-locked).
        IoFreeMdl(m->Mdl);
        ObDereferenceObject(m->Process);
        ExFreePoolWithTag(m, VIOGPUTAG);
        m = next;
    }
    m_KmdShmem.Maps = NULL;

    if (m_deferReleaseItem)
    {
        IoFreeWorkItem(m_deferReleaseItem);
        m_deferReleaseItem = NULL;
    }

    if (m_GpummuPages != NULL)
    {
        // No DetachBacking here: the DestroyResource below unrefs the host
        // resource, which drops its backing with it.
        delete[] m_GpummuPages;
        m_GpummuPages = NULL;
    }

    m_DeviceAllocations.clear();

    if (m_IsBlob && m_Blob.Mapped)
    {
        // Orphaned mapping (process died without the UMD unmap).  Do NOT
        // send UNMAP_BLOB here: back-to-back unmap+destroy hits the QEMU
        // async-unmap/destroy race and wedges its main loop (cmdq suspended
        // forever on the unmap).  The DISCARD_CONTENT paging op
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
    else if (m_Id == 0)
    {
        // Residency-only blob: no res_id was minted and no host resource was
        // created, so RESOURCE_UNREF would be rejected INVALID_RESOURCE_ID
        // once per resource.
    }
    else
    {
        m_adapter->ctrlQueue.DestroyResource(m_Id, NotifyResourceDestroyed, &m_adapter->resourceIdr);
        // m_adapter->resourceIdr.PutId(m_Id);
    }

    // Release the BAR window LAST, after the command that drops the host
    // mapping is queued.  ShmemAlloc hands a freed window straight back out,
    // so the next blob placed there is MAP_BLOB'd immediately, and that map
    // must reach the host after this resource's mapping is gone or QEMU holds
    // two subregions at one hostmem offset.  The control queue is FIFO and
    // QueueBuffer adds to it synchronously, so queueing order is host
    // ordering: freeing after DestroyResource, whose RESOURCE_UNREF detaches
    // the subregion, holds on every path -- including the orphaned-mapping arm
    // above, which sends no UNMAP_BLOB.  The IMPORT arm queues no UNREF but
    // never owns a window either: its blob_flags carry no USE_MAPPABLE, so
    // IsMappable() is false and EscapeResourceInfo never places one.
    if (m_KmdShmem.Placed)
    {
        m_adapter->ShmemFree(m_Blob.MapOffset, m_KmdShmem.MapSize);
        m_KmdShmem.Placed = FALSE;
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

BOOLEAN VioGpuAllocation::AttachBacking(MDL *pMDL, size_t pageCount, size_t pageOffset)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d, IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    auto lock_guard = LockGuard();

    if (m_Id == 0)
    {
        // Residency-only blob: no host resource to attach pages to.  Record
        // the MDL so DetachBacking stays symmetric.
        m_pMDL = pMDL;
        m_pageCount = pageCount;
        m_pageOffset = pageOffset;
        m_BackingAttached = TRUE;
        return TRUE;
    }

    m_pMDL = pMDL;
    m_pageCount = pageCount;
    m_pageOffset = pageOffset;

    GPU_MEM_ENTRY *ents = new (NonPagedPoolNx) GPU_MEM_ENTRY[pageCount];
    if (ents == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--- %s res_id=%d cannot allocate %zu backing entries\n", __FUNCTION__, m_Id, pageCount));
        return FALSE;
    }

    for (UINT i = 0; i < pageCount; i++)
    {
        // PFN_NUMBER is 32-bit on i386, so widen before shifting -- otherwise
        // any page above the 4GiB line wraps and we hand the host a bogus PA.
        ents[i].addr = (ULONGLONG)MmGetMdlPfnArray(pMDL)[pageOffset + i] << PAGE_SHIFT;
        ents[i].length = PAGE_SIZE;
        ents[i].padding = 0;
    }

    m_adapter->ctrlQueue.AttachBacking(m_Id, ents, (UINT)pageCount);
    m_BackingAttached = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

void VioGpuAllocation::DetachBacking()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    auto lock_guard = LockGuard();

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;

    // Detaching backing the host never received answers with an error for
    // every allocation whose attach was dropped.
    if (!m_BackingAttached)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d no backing to detach\n", __FUNCTION__, m_Id));
        return;
    }
    m_BackingAttached = FALSE;

    if (m_Id == 0)
    {
        // Residency-only blob: the attach recorded state only, so there is
        // no host backing to detach.
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s residency-only\n", __FUNCTION__));
        return;
    }

    m_adapter->ctrlQueue.DetachBacking(m_Id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void VioGpuAllocation::HandlePageTableUpdate(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *upt)
{
    PAGED_CODE();

    // Guest backing only makes sense for TYPE_3D resources: every blob
    // variant (guest blob, HOST3D, import, shared texture) either carries its
    // own storage or aliases someone else's.
    if (m_IsBlob)
    {
        return;
    }
    // Only level-0 entries name data pages, and Use64KBPages describes a
    // different array with a 64KB stride -- the driver reports no 64KB page
    // support, so such an update is not ours.
    if (upt->PageTableLevel != 0 || upt->Flags.Use64KBPages || upt->pPageTableEntries == NULL ||
        upt->NumPageTableEntries == 0)
    {
        return;
    }

    auto lock_guard = LockGuard();

    const size_t total = (size_t)((m_Size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (total == 0)
    {
        return;
    }
    if (m_GpummuPages == NULL)
    {
        m_GpummuPages = new (NonPagedPoolNx) ULONGLONG[total];
        if (m_GpummuPages == NULL)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s res_id=%d cannot allocate %zu page slots\n", __FUNCTION__, m_Id, total));
            return;
        }
        for (size_t i = 0; i < total; i++)
        {
            m_GpummuPages[i] = GPUMMU_PAGE_UNFILLED;
        }
        m_GpummuFilled = 0;
    }

    // Repeat means the array holds exactly ONE entry replicated across the
    // range; indexing it per iteration would read off the end of an OS-owned
    // allocation.
    const BOOLEAN repeat = upt->Flags.Repeat ? TRUE : FALSE;
    const size_t firstPage = (size_t)(upt->AllocationOffsetInBytes >> PAGE_SHIFT);
    BOOLEAN changedAttached = FALSE;

    for (UINT i = 0; i < upt->NumPageTableEntries; i++)
    {
        const DXGK_PTE *p = &upt->pPageTableEntries[repeat ? 0 : i];
        const size_t page = firstPage + i;
        if (page >= total)
        {
            break;
        }
        // Segment 0 is system memory, and only there is PageAddress a
        // physical page number.  A Zero entry resolves to the zero page and a
        // LargePage covers more than one page -- neither names a byte of
        // backing this driver may hand to the host.
        if (p->Valid && !p->Zero && !p->LargePage && p->Segment == 0)
        {
            const ULONGLONG addr = (ULONGLONG)p->PageAddress << PAGE_SHIFT;
            if (m_GpummuPages[page] == GPUMMU_PAGE_UNFILLED)
            {
                m_GpummuFilled++;
            }
            else if (m_GpummuPages[page] != addr && m_BackingAttached)
            {
                changedAttached = TRUE;
            }
            m_GpummuPages[page] = addr;
        }
        else
        {
            if (m_GpummuPages[page] != GPUMMU_PAGE_UNFILLED)
            {
                m_GpummuPages[page] = GPUMMU_PAGE_UNFILLED;
                m_GpummuFilled--;
                if (m_BackingAttached)
                {
                    changedAttached = TRUE;
                }
            }
        }
    }

    // Reconcile the host resource with the new residency state.  Both queue
    // posts are ordered against any in-flight transfer for this resource by
    // the control queue itself.  (ctrlQueue is used directly: AttachBacking/
    // DetachBacking would re-acquire the non-recursive allocation mutex.)
    if (m_BackingAttached && changedAttached)
    {
        m_adapter->ctrlQueue.DetachBacking(m_Id);
        m_BackingAttached = FALSE;
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s detach res_id=%d filled=%zu/%zu\n", __FUNCTION__, m_Id, m_GpummuFilled, total));
    }
    if (!m_BackingAttached && m_GpummuFilled == total)
    {
        GPU_MEM_ENTRY *ents = new (NonPagedPoolNx) GPU_MEM_ENTRY[total];
        if (ents == NULL)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s res_id=%d cannot allocate %zu backing entries\n", __FUNCTION__, m_Id, total));
            return;
        }
        for (size_t i = 0; i < total; i++)
        {
            ents[i].addr = m_GpummuPages[i];
            ents[i].length = PAGE_SIZE;
            ents[i].padding = 0;
        }
        m_adapter->ctrlQueue.AttachBacking(m_Id, ents, (UINT)total);
        m_BackingAttached = TRUE;
        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s attach res_id=%d pages=%zu\n", __FUNCTION__, m_Id, total));
    }
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
    if (!m_adapter->ctrlQueue.ResourceMapBlob(m_Id, ctx_id, m_Blob.MapOffset, complete_cb, complete_ctx))
    {
        // Nothing was queued, so complete_cb will never fire.  Leaving
        // Mapped set would also make the paired unmap believe there is a
        // host mapping to tear down.
        DbgPrint(TRACE_LEVEL_ERROR, ("%s res_id=%d map not issued\n", __FUNCTION__, m_Id));
        return FALSE;
    }
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
    if (!m_adapter->ctrlQueue.ResourceUnmapBlob(m_Id, ctx_id, complete_cb, complete_ctx))
    {
        // Nothing was queued, so complete_cb will never fire.  Mapped stays
        // set: the host mapping is still live and a later unmap must retry.
        DbgPrint(TRACE_LEVEL_ERROR, ("%s res_id=%d unmap not issued\n", __FUNCTION__, m_Id));
        return FALSE;
    }
    m_Blob.Mapped = FALSE;
    return TRUE;
}

BOOLEAN VioGpuAllocation::MapBlob(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    if (!m_IsBlob) return FALSE;

    // Already mapped: no host command issued, so complete_cb will not fire --
    // report FALSE so the caller's pending count stays balanced.
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

// NONPAGED: called from VioGpuCommand::Run / AttachAllocations on the
// commander thread at DISPATCH_LEVEL (spinlocks held). Living in the
// PAGE segment bugchecks 0xD1 (instruction-fetch of paged-out code at
// IRQL 2) when UnmarkBusy is reached from Run.
void VioGpuAllocation::MarkBusy()
{
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

PAGED_CODE_SEG_BEGIN

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
        // EX carries the trailing lookup cookie written below.  The size is
        // UMD-facing ABI and must match the paired user-mode driver.
        pStandardAllocation->AllocationPrivateDriverDataSize = sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX);
        return STATUS_SUCCESS;
    }

    VIOGPU_CREATE_ALLOCATION_EXCHANGE *allocationExchange =
        (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)pStandardAllocation->pAllocationPrivateDriverData;
    {
        // Standard allocations are created in one process/session context and
        // opened from others, where GetHandleData, the create->open pairing,
        // and the KMT map all miss.  dxgkrnl replays these bytes at every
        // open, so a unique cookie recorded here resolves those opens.  Bit 63
        // namespaces KMD-minted cookies.
        static volatile LONG64 s_StdAllocCookieCounter = 0;
        ((VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX *)allocationExchange)->LookupCookie =
            (1ULL << 63) | (ULONGLONG)InterlockedIncrement64(&s_StdAllocCookieCounter);
    }

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

    // Only element [0] is ever filled in below, so a multi-allocation create
    // would hand dxgkrnl uninitialised hAllocation values to destroy later.
    // The paired UMD always asks for exactly one.
    if (pCreateAllocation->NumAllocations != 1)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--- %s unsupported NumAllocations %d\n", __FUNCTION__, pCreateAllocation->NumAllocations));
        return STATUS_INVALID_PARAMETER;
    }

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

    if (allocation == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed to allocate VioGpuAllocation\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }

    allocationInfo->hAllocation = allocation->ToHandle();
    // The lookup cookie rides after the exchange when the authoring side wrote
    // one; its presence is signalled by the private-data size.  Recording it
    // here is what lets cross-process opens resolve when every handle path
    // misses.
    {
        UINT selectedSize = (pCreateAllocation->PrivateDriverDataSize > allocationInfo->PrivateDriverDataSize)
                                ? pCreateAllocation->PrivateDriverDataSize
                                : allocationInfo->PrivateDriverDataSize;
        ULONGLONG cookie = 0;
        if (selectedSize >= sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX))
        {
            cookie = ((VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX *)resourceExchange)->LookupCookie;
        }
        // Shared/presentable textures are UMD-authored and carry no trailing
        // cookie, but (create_ctx_id, blob_id) is already unique among live
        // shared textures, since the pending blob is staged per transport
        // context with a per-context-monotonic blob_id.  DWM opens these
        // cross-process; with no resolution path the open fails and dwmcore
        // fail-fasts, blacking the desktop and taking every D3D app with it.
        if (cookie == 0 && resourceExchange->Type == VIOGPU_RESOURCE_TYPE_SHARED)
        {
            cookie = VioGpuSharedTexCookie(resourceExchange->OptionsShared.create_ctx_id,
                                           resourceExchange->OptionsShared.blob_id);
        }
        if (cookie != 0)
        {
            adapter->CookieMapInsert(cookie, allocation);
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("%s cookie=%llx -> alloc=%p\n", __FUNCTION__, cookie, allocation));
        }
    }
    // Queue for the paired in-create DxgkDdiOpenAllocation, which is the only
    // way to reach allocations that carry no lookup cookie.
    adapter->PendingCreatePush(allocation);

    // Only mint a resource for the first allocation of one: when dxgkrnl adds
    // an allocation to an existing resource it passes that resource's handle
    // back in, and overwriting it stranded the previous VioGpuResource (only
    // the final handle is ever destroyed).
    if (pCreateAllocation->Flags.Resource && pCreateAllocation->hResource == NULL)
    {
        VioGpuResource *resource = new (NonPagedPoolNx) VioGpuResource();
        if (resource == NULL)
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed to allocate VioGpuResource\n", __FUNCTION__));
            // Release() unwinds the cookie/pending-create registrations above
            // via ~VioGpuAllocation.
            allocationInfo->hAllocation = NULL;
            allocation->Release();
            return STATUS_NO_MEMORY;
        }
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
    // Every [out] field must be written: the type-specific blocks below only
    // set EvictionSegmentSet for 3D and SHARED, leaving BLOB/IMPORT to inherit
    // whatever was in the caller's array.
    allocationInfo->EvictionSegmentSet = 0;

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

void VioGpuAllocation::SetSegmentPlacement(UINT segmentId, ULONGLONG addr, const char *source)
{
    // Paging ops deliver segment-RELATIVE offsets while allocation lists
    // deliver segment-GLOBAL addresses.  Both segment sizes (1 GB aperture,
    // 8 GB shmem) are smaller than their base addresses, so comparing against
    // the base disambiguates the two.
    ULONGLONG base = (segmentId == 2)   ? VioGpuAdapter::SHMEM_GPU_BASE_VA
                     : (segmentId == 1) ? 0xC0000000ull
                                        : 0;
    ULONGLONG global = (addr >= base) ? addr : base + addr;
    m_SegmentAddress.QuadPart = (LONGLONG)global;
    if (IsBlob() && segmentId == 2)
    {
        // Single placement authority: once the KMD-owned window placed
        // this blob, no other caller may move its map offset (guest
        // window and host MAP_BLOB both hang off it -- see the Patch
        // path comment in viogpu_command.cpp).
        ULONGLONG off = global - VioGpuAdapter::SHMEM_GPU_BASE_VA;
        if (m_KmdShmem.Placed && off != m_Blob.MapOffset)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s res_id=%d KEEPING kmd-owned mapoff=0x%llx (caller %s wanted 0x%llx)\n",
                      __FUNCTION__, m_Id, m_Blob.MapOffset, source, off));
        }
        else
        {
            m_Blob.MapOffset = off;
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s res_id=%d seg=%u addr=0x%llx via %s -> segaddr=0x%llx mapoff=0x%llx\n",
              __FUNCTION__, m_Id, segmentId, addr, source, global,
              (IsBlob() && segmentId == 2) ? m_Blob.MapOffset : 0ull));
}

NTSTATUS VioGpuAllocation::MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    size_t pageCount = pBuildPagingBuffer->MapApertureSegment.NumberOfPages;
    size_t mdlPageOffset = pBuildPagingBuffer->MapApertureSegment.MdlOffset;

    MDL *pMdl = pBuildPagingBuffer->MapApertureSegment.pMdl;

    if (!IsBlob() || IsGuestBlob()) {
        if (!AttachBacking(pMdl, pageCount, mdlPageOffset))
        {
            // Every transfer against this res_id now moves nothing, so say
            // so loudly.  The status cannot carry it: dxgkrnl treats any
            // failure other than ALLOCATION_BUSY / INSUFFICIENT_DMA_BUFFER
            // as fatal, and retrying through INSUFFICIENT_DMA_BUFFER spins
            // -- VidMm hands back a fresh paging buffer, which is not what
            // ran out.  DetachBacking is gated on the attach having landed
            // so the host is not asked to detach backing it never received.
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--- %s res_id=%d aperture mapped with NO host backing\n", __FUNCTION__, m_Id));
        }
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

    // VidMm never commits our host-backed blobs into the shmem
    // segment under GpuMmu (locks hand out system staging pages), so the
    // KMD owns placement: suballocate a shmem offset, map BAR+offset into
    // the calling process, and return the VA -- the UMD uses it instead
    // of D3DKMTLock.  The escape runs in-process, so the user mapping
    // lands in the right address space.
    if (m_IsBlob && IsMappable())
    {
        if (!m_KmdShmem.Placed)
        {
            ULONGLONG off = m_adapter->ShmemAlloc((SIZE_T)m_Size);
            if (off != (ULONGLONG)-1)
            {
                // Place before setting the latch: SetSegmentPlacement refuses
                // to move a KMD-owned offset once Placed is set.
                SetSegmentPlacement(2, off, "kmd-owned");
                m_KmdShmem.Placed = TRUE;
                m_KmdShmem.MapSize = (SIZE_T)m_Size;
            }
        }
        // One mapping per calling process: a shared blob is RES_INFO'd both by
        // its creator and by cross-process openers, and each needs the window
        // in its own address space.  A single-owner slot would starve whichever
        // process came second into the D3DKMTLock fallback, which under GpuMmu
        // hands out staging pages and corrupts data silently.
        PVOID userVa = NULL;
        if (m_KmdShmem.Placed)
        {
            PEPROCESS self = PsGetCurrentProcess();
            for (KmdShmemMapping *m = m_KmdShmem.Maps; m != NULL; m = m->Next)
            {
                if (m->Process == self)
                {
                    userVa = m->UserVa;
                    break;
                }
            }
            if (userVa == NULL)
            {
                // Build the MDL over the physical BAR range directly:
                // MmBuildMdlForNonPagedPool is only reliable for real nonpaged
                // pool and yields wrong PFNs past the first few pages of an
                // MmMapIoSpaceEx range.
                ULONGLONG pa = m_adapter->GetShmemPA() + m_Blob.MapOffset;
                PMDL mdl = IoAllocateMdl((PVOID)(ULONG_PTR)pa, (ULONG)m_KmdShmem.MapSize, FALSE, FALSE, NULL);
                if (mdl != NULL)
                {
                    PPFN_NUMBER pfns = MmGetMdlPfnArray(mdl);
                    ULONG pages = (ULONG)ADDRESS_AND_SIZE_TO_SPAN_PAGES(pa, m_KmdShmem.MapSize);
                    for (ULONG i = 0; i < pages; i++)
                    {
                        pfns[i] = (PFN_NUMBER)((pa >> PAGE_SHIFT) + i);
                    }
                    mdl->MdlFlags |= MDL_PAGES_LOCKED;
                    __try
                    {
                        // CACHED (WB), matching the host: the render server
                        // maps these memfd pages write-back and virglrenderer
                        // reports MAP_CACHE_CACHED for them.  Under
                        // honor-guest-pat=on a WC guest mapping of host-WB
                        // pages is the SDM-undefined WB/WC alias and produces
                        // stale reads; the Linux guest maps these blobs WB
                        // for the same reason.
                        userVa = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, NULL, FALSE,
                                                              NormalPagePriority);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s res_id=%d user map raised 0x%x\n", __FUNCTION__, m_Id, GetExceptionCode()));
                    }
                    KmdShmemMapping *node = NULL;
                    if (userVa != NULL)
                    {
                        node = (KmdShmemMapping *)ExAllocatePoolZero(NonPagedPoolNx, sizeof(KmdShmemMapping),
                                                                     VIOGPUTAG);
                    }
                    if (node != NULL)
                    {
                        // Reference the process: entries are pointer-
                        // compared, and an unreferenced EPROCESS could be
                        // reused by a new process after the owner dies.
                        ObReferenceObject(self);
                        node->Process = self;
                        node->Mdl = mdl;
                        node->UserVa = userVa;
                        node->Next = m_KmdShmem.Maps;
                        m_KmdShmem.Maps = node;
                    }
                    else
                    {
                        if (userVa != NULL)
                        {
                            MmUnmapLockedPages(userVa, mdl);
                            userVa = NULL;
                        }
                        IoFreeMdl(mdl);
                    }
                }
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("%s res_id=%d kmd-owned shmem off=0x%llx size=%zu pa=0x%llx uva=%p proc=%p\n",
                          __FUNCTION__, m_Id, m_Blob.MapOffset, m_KmdShmem.MapSize, pa, userVa, self));
            }
        }
        if (userVa == NULL)
        {
            // Without a mapping the UMD would fall back to D3DKMTLock,
            // which under GpuMmu hands out staging pages -- reads and
            // writes silently land in the wrong memory.  Fail loudly.
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s res_id=%d no shmem mapping (placed=%d) -- failing RES_INFO\n",
                      __FUNCTION__, m_Id, m_KmdShmem.Placed));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        resInfo->UserVa = (ULONGLONG)(ULONG_PTR)userVa;
    }
    else
    {
        resInfo->UserVa = 0;
    }

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

    // The busy count only drops when the host worker answers the in-flight
    // command and the vbuf callback runs UnmarkBusy, so an untimed wait
    // would strand this thread on a dead host until adapter teardown.  Wait
    // in 1 s rounds up to 10 s, bail early once the adapter stops, and
    // report a status the UMD can treat as device-lost.
    ULONG rounds = 0;
    while (resBusy->Wait && m_busy != 0)
    {
        if (!m_adapter->IsDriverActive())
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s res_id=%d abandoned: adapter stopped\n", __FUNCTION__, m_Id));
            resBusy->IsBusy = TRUE;
            return STATUS_DEVICE_REMOVED;
        }
        LARGE_INTEGER timeout;
        timeout.QuadPart = -10LL * 1000 * 1000; // 1 s, relative
        NTSTATUS waitStatus = KeWaitForSingleObject(&m_busyNotification, UserRequest, KernelMode, FALSE, &timeout);
        if (waitStatus == STATUS_TIMEOUT && ++rounds >= 10)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s res_id=%d still busy after %u s -- host worker dead? "
                      "returning DEVICE_HUNG\n",
                      __FUNCTION__, m_Id, rounds));
            resBusy->IsBusy = TRUE;
            return STATUS_DEVICE_HUNG;
        }
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

BOOLEAN VioGpuAllocation::IsOpenOn(VioGpuDevice *pDevice)
{
    PAGED_CODE();

    if (pDevice == NULL)
    {
        return FALSE;
    }
    auto lock_guard = LockGuard();
    return Find(pDevice) != nullptr;
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
    if (pEntry == nullptr)
    {
        // Stale close: dxgkrnl tears down a device-allocation whose list
        // entry is already gone, which happens at process exit.
        // Dereferencing the NULL entry bugchecks 0x3B in
        // VioGpuDeviceAllocation::Unref, so log and bail; the object is
        // leaked rather than double-freed.
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s STALE close alloc=%p devalloc=%p dev=%p entry=NULL\n", __FUNCTION__, this, pDeviceAllocation,
                  pDeviceAllocation->GetDevice()));
        VioGpuDbgBreak();
        return;
    }
    if (&pEntry->value != pDeviceAllocation)
    {
        // A pointer mismatch is normal operation: an allocation opened
        // more than once on a device closes with a different devalloc
        // pointer than the list entry.  Unref the FOUND entry -- bailing
        // out here instead leaks every such close and wedges the 1.3
        // desktop black.
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s devalloc mismatch alloc=%p devalloc=%p entry=%p\n", __FUNCTION__, this, pDeviceAllocation,
                  &pEntry->value));
        VioGpuDbgBreak();
    }

    if (pEntry->value.Unref())
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p removing devalloc=%p size=%zu\n", __FUNCTION__, this, &pEntry->value, m_DeviceAllocations.size()));
        // Retire the open's KMT-handle binding only when the last ref
        // drops (WDDM2 GetHandleData fallback).
        if (pEntry->value.m_hKmtAllocation != 0)
        {
            m_adapter->KmtMapRemove(pEntry->value.m_hKmtAllocation, this);
            pEntry->value.m_hKmtAllocation = 0;
        }
        UnmapBlobLocked(pEntry->value.GetCtxId(), NULL, NULL);
        m_DeviceAllocations.remove(pEntry);
    }
}

PAGED_CODE_SEG_END

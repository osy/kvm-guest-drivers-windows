#pragma once
#include "handle.h"
#include "linked_list.h"

#include "viogpum.h"
#include "virgl_hw.h"

#include "viogpu_queue.h"
#include "viogpu_device.h"

class VioGpuAdapter;
class VioGpuDeviceAllocation;

class VioGpuResource final : public HandleBase<"VIOGRESO"_M, VioGpuResource>
{
  public:
  private:
};

class VioGpuAllocation;

class VioGpuAllocationLockGuard
{
  friend class VioGpuAllocation;
  public:
    ~VioGpuAllocationLockGuard();
  protected:
    VioGpuAllocationLockGuard(VioGpuAllocation *allocation);
  private:
    VioGpuAllocation *m_Allocation;
};

class VioGpuAllocation final : public HandleBase<"VIOGALLO"_M, VioGpuAllocation>
{
  friend class VioGpuDeviceAllocation;
  friend class VioGpuDevice;
  friend class VioGpuAllocationLockGuard;
  friend class VioGpuCommander;
  friend class VioGpuCommand;
  public:
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_BLOB_OPTIONS *options, ULONGLONG size);
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_3D_OPTIONS *options, ULONGLONG size);

    // Import: adopt an already-created host res_id owned by another
    // allocation. Behaves like a HOST3D blob for residency, but mints no
    // id, issues no RESOURCE_CREATE_BLOB, and (m_IsImport) skips
    // DestroyResource at teardown so the owning allocation's res_id is not
    // unref'd or freed twice.  Opening it attaches the resource to the
    // opening device's virtio context (Venus-style dma-buf import).
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_IMPORT_OPTIONS *options, ULONGLONG size);

    // Shared / presentable texture: a host D3D11 texture with exportable
    // (dmabuf) storage, bound to a virtio-gpu blob resource.  The UMD
    // staged the export as a pending blob under blob_id on create_ctx_id;
    // this allocation mints the res_id and issues RESOURCE_CREATE_BLOB
    // there when first opened.  primary marks a flippable scanout target
    // (segment-1 residency + scanout promotion + SET_SCANOUT_BLOB info).
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *options, ULONGLONG size);

    ~VioGpuAllocation(void);

    // Refcounting. Constructor starts at 1 (the DXGK reference). AddRef
    // before publishing this pointer to a path that may outlive
    // DxgkDestroyAllocation; the matching Release runs after the pointer
    // is dropped. Object is deleted when the count reaches zero.
    void AddRef();
    void Release();

    // Variant of Release safe at IRQL >= DISPATCH_LEVEL. ~VioGpuAllocation
    // tears down LinkedList<VioGpuDeviceAllocation>, whose entries' dtor
    // is PAGED_CODE(); a Release that drops the last ref from a DPC or
    // higher would trip that contract. At raised IRQL the Release runs
    // through a pre-allocated work item so the destructor lands at
    // PASSIVE_LEVEL.
    void ReleaseDeferred();

    inline UINT GetId(void) const
    {
        return m_Id;
    }

    void MarkBusy();
    void UnmarkBusy();

    void SetDxPhysicalAddress(size_t DxPhysicalAddress)
    {
        m_DxPhysicalAddress = DxPhysicalAddress;
    };

    size_t GetDxPhysicalAddress() const
    {
        return m_DxPhysicalAddress;
    };

#define VIOGPU_BLOB_MEM_GUEST             0x0001
#define VIOGPU_BLOB_MEM_HOST3D            0x0002
#define VIOGPU_BLOB_MEM_HOST3D_GUEST      0x0003

#define VIOGPU_BLOB_FLAG_USE_MAPPABLE     0x0001
#define VIOGPU_BLOB_FLAG_USE_SHAREABLE    0x0002
#define VIOGPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004
/* Control-ring blobs the guest CPU-maps and polls forever: pin at MAXIMUM
 * priority so VidMm won't DISCARD their backing under VRAM pressure (a discard
 * zeroes the guest mapping -> ring wedge). */
#define VIOGPU_BLOB_FLAG_PINNED           0x0008
// See wddm_hw.h: a blob that names no host resource, created only so its
// D3D12 resource owns a kernel allocation.
#define VIOGPU_BLOB_FLAG_RESIDENCY_ONLY   0x0010

    inline BOOL IsCoherent() const
    {
        // FIXME: what's this even supposed to mean for blob resources?
        return (!m_IsBlob && (m_3dOptions.flags & VIRGL_RESOURCE_FLAG_MAP_COHERENT) != 0) || IsGuestBlob();
    }

    inline BOOL IsBlob() const
    {
        return m_IsBlob;
    }

    inline BOOL IsGuestBlob() const
    {
        return m_IsBlob && (m_Blob.Options.blob_mem == VIOGPU_BLOB_MEM_GUEST || m_Blob.Options.blob_mem == VIOGPU_BLOB_MEM_HOST3D_GUEST);
    }

    inline BOOL IsHost3dBlob() const
    {
        return m_IsBlob && (m_Blob.Options.blob_mem == VIOGPU_BLOB_MEM_HOST3D || m_Blob.Options.blob_mem == VIOGPU_BLOB_MEM_HOST3D_GUEST);
    }

    inline BOOL IsCreated() const
    {
        return !m_IsBlob || m_Blob.Created;
    }

    inline BOOL IsMappable() const
    {
        return m_IsBlob && m_Blob.Created && (m_Blob.Options.blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE) != 0;
    }

    // The KMD-owned shmem window placement is latched, making it the single
    // authority for this blob's map offset.
    inline BOOL IsKmdShmemPlaced() const
    {
        return m_KmdShmem.Placed;
    }

    inline BOOL IsPinned() const
    {
        return m_IsBlob && (m_Blob.Options.blob_flags & VIOGPU_BLOB_FLAG_PINNED) != 0;
    }

    inline BOOL IsMapped() const
    {
        return m_IsBlob && m_Blob.Created && m_Blob.Mapped;
    }

    inline BOOL IsPrimary() const
    {
        return m_IsPrimary;
    }

    // Segment address of the allocation as of its last Patch (zero until
    // first patched).  The vsync interrupt reports the latched primary's
    // address so dxgkrnl sees flips progress on the display.
    PHYSICAL_ADDRESS m_SegmentAddress = {};

    // Backing byte size of the allocation (framebuffer bytes for a primary).
    // Always populated, including for IMPORT primaries, so it is the reliable
    // discriminator between the full-screen desktop primary and DWM's
    // sub-screen cursor / per-window primaries when gating scanout promotion.
    inline ULONGLONG GetSize() const
    {
        return m_Size;
    }

    // Translate a 2D resource coordinate to the attached guest backing
    // layout used by VIRTIO_GPU_CMD_TRANSFER_{TO,FROM}_HOST_3D.  The box
    // selects pixels in the host resource; offset independently selects the
    // first byte in the guest backing, so partial transfers must include x/y.
    BOOLEAN GetTransferLayout(LONG x, LONG y, ULONG *pStride, ULONGLONG *pOffset) const;

    // FALSE => RESOURCE_ATTACH_BACKING was never issued, so the host side of
    // this resource has no pages behind it.
    BOOLEAN AttachBacking(MDL *pMdl, size_t pageCount, size_t pageOffset);
    void DetachBacking();

    // WDDM2/GpuMmu: capture the allocation's system pages from level-0
    // UPDATE_PAGE_TABLE writes and keep the host resource's guest backing in
    // step with residency.  Under GpuMmu VidMm points GPU PTEs straight at
    // system memory and never maps the aperture segment, so this paging op is
    // the only place the pages behind a TYPE_3D allocation are ever named.
    void HandlePageTableUpdate(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *upt);

    void FlushToScreen(UINT scan_id);

    static NTSTATUS GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation);
    static NTSTATUS DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation);

    NTSTATUS DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation);
    NTSTATUS MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);
    NTSTATUS UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);

    // pMapDevice non-NULL also completes the blob's host mapping, synchronously,
    // on that device's context -- the placement is only usable once the host has
    // mapped memory behind the BAR window.  The blt-present path passes NULL: it
    // reads the descriptor and must not establish a mapping of its own.
    NTSTATUS EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo, VioGpuDevice *pMapDevice);
    NTSTATUS EscapeReleaseWindow(VioGpuDevice *pDevice);
    NTSTATUS EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy);

    VOID CreateBlob(UINT ctx_id);
    // Return TRUE if a host map/unmap command was actually issued (so
    // complete_cb will fire from the queue-completion DPC), FALSE if the
    // blob was already in the requested state and no host round-trip --
    // and therefore no callback -- happened. The caller relies on this to
    // keep its outstanding-callback accounting exact.
    BOOLEAN MapBlob(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);
    BOOLEAN UnmapBlob(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);

  protected:
    BOOL m_IsBlob;
    // Import allocation: m_Id is adopted (owned by another allocation), so
    // the destructor must not DestroyResource it. See the import ctor.
    BOOL m_IsImport;
    // Flippable primary: the allocation is a VidPnSource scanout target and
    // VioGpuDevice::Present updates VioGpuVidPN's m_sourceRes when it sees
    // this allocation as the blt-present source.
    BOOL m_IsPrimary;
    // Shared/presentable blob-backed texture. Dimensions/format are retained
    // for DxgkDdiDescribeAllocation when another process opens the share.
    BOOL m_IsShared;
    UINT m_SharedWidth;
    UINT m_SharedHeight;
    UINT m_SharedFormat; // DXGI_FORMAT
    // DRM modifier of the host dmabuf backing a shared blob.  Blt Present
    // must pass this through PIPE_RESOURCE_SET_TYPE so virglrenderer imports
    // tiled allocations with their real layout.  Other blob types retain
    // INVALID, matching the old implicit-modifier behavior.
    ULONGLONG m_BlobModifier = 0x00FFFFFFFFFFFFFFull;
    // Context the deferred RESOURCE_CREATE_BLOB targets (the UMD transport
    // context that staged the pending blob).  0 = the opening device's own
    // context (transport shmem blobs).
    UINT m_CreateCtxId = 0;
    union {
        VIOGPU_RESOURCE_3D_OPTIONS m_3dOptions;
        struct {
            VIOGPU_RESOURCE_BLOB_OPTIONS Options;
            VIOGPU_BLOB_INFO Info;
            ULONGLONG MapOffset;
            BOOL Mapped;
            BOOL InfoValid;
            BOOL Created;
        } m_Blob;
    };
    ULONGLONG m_Size;

    VioGpuAllocationLockGuard LockGuard() {
        return VioGpuAllocationLockGuard{this};
    }

    VOID Lock();
    VOID Unlock();

  public:
    VioGpuDeviceAllocation *Open(VioGpuDevice *pDevice);
    void Close(VioGpuDeviceAllocation *pDeviceAllocation);
    // Whether pDevice has this allocation open.  dxgkrnl validates an
    // allocation handle against the calling device for every DDI that takes
    // one, but escape payloads are unvalidated UMD bytes, so this is what
    // stands between a caller and an allocation it never opened.
    BOOLEAN IsOpenOn(VioGpuDevice *pDevice);
  private:
    inline LinkedList<VioGpuDeviceAllocation>::Entry *Find(VioGpuDevice *pDevice);
    inline BOOLEAN MapBlobLocked(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);
    inline BOOLEAN MapBlobSyncLocked(VioGpuDevice *pDevice);
    inline BOOLEAN UnmapBlobLocked(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);

    static VOID NTAPI DeferredReleaseWorker(PDEVICE_OBJECT DeviceObject, PVOID Context);
    static VOID NTAPI DeferredReleaseDpc(_KDPC *Dpc, PVOID Context, PVOID Arg1, PVOID Arg2);
    void QueueDeferredReleaseWorkItem();

    VioGpuAdapter *m_adapter;
    UINT m_Id;

  public:
    // Create->open pairing state, owned by VioGpuAdapter's
    // m_PendingCreateLock.
    LIST_ENTRY m_PendingCreateEntry;
    PKTHREAD m_PendingCreateThread;

    // Segment placement arrives via paging ops (NotifyResidency /
    // UpdatePageTable PTEs / Transfer destinations), because DxgkDdiPatch
    // never runs for GpuMmu virtual contexts.  Normalizes to the
    // segment-global convention and refreshes m_Blob.MapOffset.
    void SetSegmentPlacement(UINT segmentId, ULONGLONG addr, const char *source);

    // KMD-owned shmem mappings for mappable blobs.  Placement happens once per
    // allocation; user mappings are created by EscapeResourceInfo in the
    // calling process, one per process, because a shared blob is legitimately
    // RES_INFO'd by both its creator and every cross-process opener.  All torn
    // down in the destructor.
    struct KmdShmemMapping
    {
        KmdShmemMapping *Next;
        PEPROCESS Process; // referenced (identity must outlive the entry)
        PMDL Mdl;
        PVOID UserVa;
    };
    struct
    {
        BOOLEAN Placed = FALSE;
        SIZE_T MapSize = 0;
        KmdShmemMapping *Maps = NULL;
    } m_KmdShmem;

  private:
    FAST_MUTEX m_Lock;

    LinkedList<VioGpuDeviceAllocation> m_DeviceAllocations;

    MDL *m_pMDL;
    size_t m_pageCount;
    size_t m_pageOffset;
    // RESOURCE_ATTACH_BACKING reached the host; gates the paired detach.
    BOOLEAN m_BackingAttached;

    // GpuMmu residency capture (HandlePageTableUpdate): per-page physical
    // addresses harvested from UPDATE_PAGE_TABLE, GPUMMU_PAGE_UNFILLED where
    // the page is not (or no longer) resident.  Backing is attached when
    // every page is known and detached the moment any page is invalidated.
    static constexpr ULONGLONG GPUMMU_PAGE_UNFILLED = ~0ull;
    ULONGLONG *m_GpummuPages = NULL;
    size_t m_GpummuFilled = 0;

    size_t m_DxPhysicalAddress;

    KEVENT m_busyNotification;
    volatile LONG m_busy;
    KSPIN_LOCK m_busyLock;

    volatile LONG m_refCount;

    // Pre-allocated at construction time so ReleaseDeferred can punt
    // the trailing Release out of a raised-IRQL caller (e.g. the DIRQL
    // arm of DxgkDdiSetVidPnSourceAddress) without risking
    // IoAllocateWorkItem failure on the hot path.
    PIO_WORKITEM m_deferReleaseItem;

    // Trampoline for ReleaseDeferred callers above DISPATCH_LEVEL, where
    // IoQueueWorkItem may not be called at all; see ReleaseDeferred. The count
    // lets repeat calls coalesce onto the single DPC and work item without
    // losing a reference.
    KDPC m_deferReleaseDpc;
    volatile LONG m_deferReleaseCount;
    volatile LONG m_deferReleaseQueued;
};

extern void NotifyResourceDestroyed(void *ctx, void *cmd, void *resp);

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
    // allocation. Behaves like a HOST3D blob for residency/scanout, but
    // mints no id, issues no RESOURCE_CREATE_BLOB, and (m_IsImport) skips
    // DestroyResource at teardown so the owning allocation's res_id is not
    // unref'd or freed twice.
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_IMPORT_OPTIONS *options, ULONGLONG size);

    // Shared: a host-COM-backed texture made cross-process shareable. Carries
    // no virtio res_id (m_Id == 0) and creates no host virtio resource -- the
    // pixels live in the host D3D11 texture reached via the Neptune COM
    // transport. The allocation is only a WDDM sharing token + private-data
    // carrier; the destructor skips DestroyResource and Open skips the context
    // attach.
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_SHARED_OPTIONS *options, ULONGLONG size);

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

    inline BOOL IsMapped() const
    {
        return m_IsBlob && m_Blob.Created && m_Blob.Mapped;
    }

    inline BOOL IsPrimary() const
    {
        return m_IsPrimary;
    }

    // Host-COM-backed shared allocation: no virtio res_id, no context attach,
    // no DestroyResource. Exists only as a WDDM sharing token.
    inline BOOL IsShared() const
    {
        return m_IsShared;
    }

    void AttachBacking(MDL *pMdl, size_t pageCount, size_t pageOffset);
    void DetachBacking();

    void FlushToScreen(UINT scan_id);

    static NTSTATUS GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation);
    static NTSTATUS DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation);

    NTSTATUS DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation);
    NTSTATUS MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);
    NTSTATUS UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);

    NTSTATUS EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo);
    NTSTATUS EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy);
    NTSTATUS EscapeResourceBlobSetInfo(VIOGPU_RES_BLOB_SET_INFO_REQ *resBlob);

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
    // Host-COM-backed cross-process shared allocation (no virtio res_id, no
    // context attach, no DestroyResource). Dimensions/format are retained for
    // DxgkDdiDescribeAllocation when another process opens the share.
    BOOL m_IsShared;
    UINT m_SharedWidth;
    UINT m_SharedHeight;
    UINT m_SharedFormat; // DXGI_FORMAT
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
  private:
    inline LinkedList<VioGpuDeviceAllocation>::Entry *Find(VioGpuDevice *pDevice);
    inline BOOLEAN MapBlobLocked(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);
    inline BOOLEAN UnmapBlobLocked(UINT ctx_id, void (*complete_cb)(void *, void *, void *), void *complete_ctx);

    static VOID NTAPI DeferredReleaseWorker(PDEVICE_OBJECT DeviceObject, PVOID Context);

    VioGpuAdapter *m_adapter;
    UINT m_Id;

    FAST_MUTEX m_Lock;

    LinkedList<VioGpuDeviceAllocation> m_DeviceAllocations;

    MDL *m_pMDL;
    size_t m_pageCount;
    size_t m_pageOffset;

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
};

extern void NotifyResourceDestroyed(void *ctx, void *cmd, void *resp);

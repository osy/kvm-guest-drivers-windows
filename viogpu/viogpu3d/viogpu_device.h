#pragma once
#include "handle.h"
#include "viogpum.h"
#include "viogpu.h"

class VioGpuAdapter;
class VioGpuDevice;
class VioGpuAllocation;
class VioGpuDeviceAllocation;
class CtrlQueue;

// This class represents the actual VirtIO 3D ctx_id
class VioGpuContext
{
  public:
    VioGpuContext(VioGpuAdapter *pAdapter);
    ~VioGpuContext();

    void Init(VIOGPU_CTX_INIT_REQ *pOptions);

    inline ULONG GetId() const
    {
        return m_id;
    }

    inline BOOLEAN IsVirgl() const {
        return m_Capset == VIRTIO_GPU_CAPSET_VIRGL ||
               m_Capset == VIRTIO_GPU_CAPSET_VIRGL2;
    }

    inline UINT GetCapset() const {
        return m_Capset;
    }

    inline BOOL IsEmpty() const
    {
        return m_empty;
    }

  private:
    VioGpuAdapter *m_pAdapter;
    ULONG m_id;
    BOOL m_empty;
    UINT m_Capset;
};

// Class that represents DXGKRNL Context, often passed as hContext
class VioGpuDevice final : public HandleBase<"VIOGDEVI"_M, VioGpuDevice>
{
  friend class VioGpuContext;
  friend class VioGpuCommander;
  public:
    VioGpuDevice(VioGpuAdapter *pAdapter);
    ~VioGpuDevice();

    inline BOOLEAN CanBlit() const {
        return m_pBlit != nullptr;
    }

    //NTSTATUS Init(VIOGPU_CTX_INIT_REQ *pOptions);

    NTSTATUS OpenAllocation(_In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation);

    NTSTATUS GenerateBltPresent(DXGKARG_PRESENT *pPresent, VioGpuDeviceAllocation *src, VioGpuDeviceAllocation *dst);
    NTSTATUS GenerateBltPresentUM(DXGKARG_PRESENT *pPresent, VioGpuAllocation *src, VioGpuAllocation *dst);
    NTSTATUS Present(_Inout_ DXGKARG_PRESENT *pPresent);
    NTSTATUS Render(DXGKARG_RENDER *pRender);

    CtrlQueue *GetCtrlQueue();

    VioGpuContext m_Context;
    VioGpuContext m_Virgl;

    PRKEVENT m_hUM, m_hKM;
    volatile PVIOGPU_BLIT_PRESENT m_pBlit;
  protected:
    VioGpuAdapter *m_pAdapter;
};

class VioGpuDeviceAllocation final : public HandleBase<"VIOGDEAL"_M, VioGpuDeviceAllocation>
{
  friend class VioGpuDevice;
  friend class VioGpuAllocation;
  public:
    VioGpuDeviceAllocation(VioGpuDevice *device, VioGpuAllocation *allocation);
    VioGpuDeviceAllocation(const VioGpuDeviceAllocation &other) = delete;
    VioGpuDeviceAllocation& operator=(const VioGpuDeviceAllocation &other) = delete;

    ~VioGpuDeviceAllocation();

    VioGpuAllocation *GetAllocation();
    VioGpuDevice *GetDevice();

    inline ULONG GetCtxId() const
    {
        return m_pDevice->m_Context.GetId();
    }

    // D3DKMT handle this open was made under; keys the adapter's KMT map.
    // 0 when unknown.
    D3DKMT_HANDLE m_hKmtAllocation = 0;

  protected:
    void Ref()
    {
        InterlockedIncrement64(&m_RefCount);
    }

    bool Unref()
    {
        return InterlockedDecrement64(&m_RefCount) == 0;
    }

    LONGLONG GetRef()
    {
        return m_RefCount;
    }

    bool m_AttachedToVirgl;
  private:
    VioGpuAllocation *m_pAllocation;
    VioGpuDevice *m_pDevice;
    volatile LONGLONG m_RefCount;
    // Whether CTX_ATTACH_RESOURCE actually issued. The destructor
    // skips DETACH if FALSE so we never send a stray DETACH for a
    // pair that never attached (e.g., when OpenAllocation's blob
    // create fails after the device-allocation is constructed).
    bool m_attached;
};

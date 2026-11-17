#include "baseobj.h"
#include "viogpu.h"

// clang-format off
_When_((PoolType & NonPagedPoolMustSucceed) != 0,
    __drv_reportError("Must succeed pool allocations are forbidden. "
        "Allocation failures cause a system crash"))
    void* __cdecl operator new(size_t Size, POOL_TYPE PoolType)
// clang-format on
{
    Size = (Size != 0) ? Size : 1;

    void *pObject = ExAllocatePoolUninitialized(PoolType, Size, VIOGPUTAG);

    if (pObject != NULL)
    {
#if DBG
        RtlFillMemory(pObject, Size, 0xCD);
#else
        RtlZeroMemory(pObject, Size);
#endif // DBG
    }
    return pObject;
}

_When_((PoolType & NonPagedPoolMustSucceed) != 0,
       __drv_reportError("Must succeed pool allocations are forbidden. "
                         "Allocation failures cause a system crash")) void *__cdecl
operator new[](size_t Size, POOL_TYPE PoolType)
{

    Size = (Size != 0) ? Size : 1;

    void *pObject = ExAllocatePoolUninitialized(PoolType, Size, VIOGPUTAG);

    if (pObject != NULL)
    {
#if DBG
        RtlFillMemory(pObject, Size, 0xCD);
#else
        RtlZeroMemory(pObject, Size);
#endif
    }
    return pObject;
}

void __cdecl operator delete(void *pObject)
{

    if (pObject != NULL)
    {
        ExFreePoolWithTag(pObject, VIOGPUTAG);
    }
}

void __cdecl operator delete[](void *pObject)
{

    if (pObject != NULL)
    {
        ExFreePoolWithTag(pObject, VIOGPUTAG);
    }
}

void __cdecl operator delete(void *pObject, size_t Size)
{

    UNREFERENCED_PARAMETER(Size);
    ::operator delete(pObject);
}

// Placement-delete forwarders that match the placement operator new
// overloads above. The compiler invokes these when a constructor throws
// inside a placement-new expression; without them the linker would
// emit "unresolved external" the moment any constructor learns to
// throw. None of the driver's constructors currently throw, but the
// matching pair must exist for the language to be well-formed.
void __cdecl operator delete(void *pObject, POOL_TYPE PoolType)
{
    UNREFERENCED_PARAMETER(PoolType);
    ::operator delete(pObject);
}

void __cdecl operator delete[](void *pObject, POOL_TYPE PoolType)
{
    UNREFERENCED_PARAMETER(PoolType);
    ::operator delete[](pObject);
}

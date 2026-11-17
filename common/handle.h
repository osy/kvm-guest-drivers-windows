#pragma once

#include "helper.h"

constexpr unsigned long long operator ""_M(const char *s, size_t n)
{
    unsigned long long result = 0;

    for (size_t i = 0; i <= n; i++) {
        result <<= 8;
        result |= s[n - i];
    }

    return result;
}

class IHandleBase
{
  public:
    virtual unsigned long long Magic() const = 0;
};

template<unsigned long long MAGIC, class T>
class HandleBase : public IHandleBase
{
  public:
    __forceinline static T *FromHandle(void *handle)
    {
        if (handle == nullptr)
        {
            return nullptr;
        }

        IHandleBase *base = static_cast<IHandleBase *>(handle);
        if (base->Magic() != MAGIC)
        {
            return nullptr;
        }

        return static_cast<T *>(base);
    }

    __forceinline void *ToHandle()
    {
        IHandleBase *base = static_cast<IHandleBase *>(this);
        return base;
    }

  private:
    inline unsigned long long Magic() const override
    {
        return MAGIC;
    }
};

/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "viogpu_idr.h"
#include "viogpu.h"
#include "baseobj.h"
#if !DBG
#include "viogpu_idr.tmh"
#endif

#if 1

#ifdef VIOGPUIDR_LIST

VioGpuIdr::VioGpuIdr()
{
    m_nextId = 0;
    KeInitializeSpinLock(&m_lock);
    InitializeListHead(&m_freeList);
}

VioGpuIdr::~VioGpuIdr()
{
    Close();
}

BOOLEAN VioGpuIdr::Init(_In_ ULONG start)
{
    Close();
    m_nextId = start;

    return true;
}

ULONG VioGpuIdr::GetId(VOID)
{
    // Allocate res/ctx ids MONOTONICALLY -- never reuse a freed id within a
    // boot.  QEMU's virtio-gpu-gl caches scanout/EGL dmabuf imports keyed by
    // res_id; reusing a just-freed id (before that cache and the host resource
    // are fully torn down) makes the reborn resource alias the old one's stale
    // pages.  For a command ring that means the host's ALIVE heartbeat lands on
    // the wrong page and never reaches the guest -> guest ring watchdog fires
    // "ring wedged head=0 tail=0 status=0x0" and the screen goes black.  A boot
    // mints at most a few thousand ids, far below the 32-bit space, so retiring
    // ids permanently is safe.  InterlockedIncrement keeps this IRQL-agnostic.
    ULONG id = (ULONG)InterlockedIncrement((volatile LONG *)&m_nextId) - 1;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("[%s] id = %d\n", __FUNCTION__, id));

    return id;
}

VOID VioGpuIdr::PutId(_In_ ULONG id)
{
    // Ids are retired permanently (GetId is monotonic) to keep a res_id from
    // being reused while QEMU still caches a stale import for it.  Nothing to
    // free-list: the id is simply never handed out again this boot.
    UNREFERENCED_PARAMETER(id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("[%s] id = %d (retired)\n", __FUNCTION__, id));
}

VOID VioGpuIdr::Close(VOID)
{

    FreeId *freeId = NULL;
    do
    {
        freeId = reinterpret_cast<FreeId *>(ExInterlockedRemoveHeadList(&m_freeList, &m_lock));
        if (freeId != NULL)
        {
            delete freeId;
        }
    } while (freeId != NULL);
}

#else

#define VIOGPU_IDR_INITIAL_CAPACITY 256


VioGpuIdr::VioGpuIdr()
{
    m_capacity = 0;
    m_count = 0;
    m_items = NULL;
    m_nextId = 0;

    KeInitializeSpinLock(&m_lock);
}

VioGpuIdr::~VioGpuIdr()
{
    if (m_items) delete[] m_items;
}

BOOLEAN VioGpuIdr::Init(_In_ ULONG start)
{
    m_nextId = start;
    return true;
}

ULONG VioGpuIdr::GetId(VOID)
{
    KIRQL irql = Lock();

    ULONG id;
    if (m_count > 0) {
        id = m_items[--m_count];
    } else {
        id = m_nextId++;
    }

    Unlock(irql);
    return new_id;
}

VOID VioGpuIdr::PutId(_In_ ULONG id)
{
    KIRQL irql = Lock();

    Reserve(m_count + 1);
    m_items[m_count++] = id;

    Unlock(irql);
}

KIRQL VioGpuIdr::Lock()
{
    KIRQL SavedIrql = KeGetCurrentIrql();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, SavedIrql));

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_lock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_lock);
    }
    else
    {
        VioGpuDbgBreak();
    }

    return SavedIrql;
}

VOID VioGpuIdr::Unlock(KIRQL Irql)
{
    if (Irql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_lock, Irql);
    }
    else
    {
        KeReleaseSpinLockFromDpcLevel(&m_lock);
    }
}

VOID VioGpuIdr::Reserve(_In_ ULONGLONG NewSize)
{
    if (NewSize > m_capacity) {
        if (m_capacity == 0)
        {
            m_capacity = VIOGPU_IDR_INITIAL_CAPACITY;
        }

        while (NewSize > m_capacity)
        {
            m_capacity *= 2;
        }

        ULONG *new_items = new (NonPagedPoolNX) ULONG[m_capacity];

        if (m_items)
        {
            RtlCopyMemory(new_items, m_items, m_count);
            delete[] m_items;
        }

        m_items = new_items;
    }
}

#endif
#endif

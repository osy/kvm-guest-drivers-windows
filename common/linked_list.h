#pragma once
#include "baseobj.h"
#include "helper.h"

template <typename T>
class LinkedList
{
  public:
    struct Entry
    {
        LIST_ENTRY base;
        T value;

        template <typename... Args>
        Entry(Args&&... args) : base(nullptr, nullptr), value(args...) {}
    };

    static_assert(offsetof(Entry, base) == 0);

    LinkedList()
    {
        InitializeListHead(&m_Head);
    }

    ~LinkedList()
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s empty=%d\n", __FUNCTION__, empty()));
        clear();
    }

    bool empty()
    {
        return IsListEmpty(&m_Head);
    }

    Entry *front()
    {
        if (empty()) return nullptr;

        return reinterpret_cast<Entry *>(m_Head.Flink);
    }

    Entry *back()
    {
        if (empty()) return nullptr;

        return reinterpret_cast<Entry *>(m_Head.Blink);
    }

    template <typename... Args>
    void clear(Args&&... args)
    {
        /* FIXME: Crashes without this */
        if (empty()) return;

        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s empty=%d\n", __FUNCTION__, empty()));
        Entry *e = nullptr;
        do
        {
            e = pop_front(args...);
            if (e != nullptr && &e->base != &m_Head)
            {
                delete e;
            }
        }
        while (e != nullptr);
    }

    template <typename... Args>
    void emplace_back(Args&&... args)
    {
        Entry *e = new (NonPagedPoolNx) Entry(args...);
        InsertTailList(&m_Head, &e->base);
    }

    //template <typename... Args>
    //void emplace_back(PKSPIN_LOCK lock, Args&&... args)
    //{
    //    Entry *e = new (NonPagedPoolNx) Entry(args...);
    //    ExInterlockedInsertTailList(&m_Head, &e->base, lock);
    //}

    // Caller owns the entry now
    void pop(Entry *e)
    {
        if (e == front())
        {
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s removing head\n", __FUNCTION__));
            RemoveHeadList(&m_Head);
        }
        else if (e == back())
        {
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s removing tail\n", __FUNCTION__));
            RemoveTailList(&m_Head);
        }
        else
        {
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s removing generic entry\n", __FUNCTION__));
            RemoveEntryList(&e->base);
        }
    }

    void remove(Entry *e)
    {
        pop(e);
        delete e;
    }

    // Caller owns the entry now
    Entry *pop_front()
    {
        if (empty()) return nullptr;

        return reinterpret_cast<Entry *>(RemoveHeadList(&m_Head));
    }

    //Entry *pop_front(PKSPIN_LOCK lock)
    //{
    //    return reinterpret_cast<Entry *>(ExInterlockedRemoveHeadList(&m_Head, lock));
    //}

    // Entry is owned by the list
    template <typename P>
    Entry *find(P &&predicate)
    {
        if (empty()) return nullptr;

        Entry *e = reinterpret_cast<Entry *>(m_Head.Flink);
        do
        {
            if (predicate(&e->value))
            {
                return e;
            }
            e = reinterpret_cast<Entry *>(e->base.Flink);
        }
        while (&e->base != &m_Head);
        return nullptr;
    }

    template <typename F>
    void foreach(F &&callback)
    {
        if (empty()) return;

        Entry *e = reinterpret_cast<Entry *>(m_Head.Flink);
        do
        {
            callback(&e->value);
            e = reinterpret_cast<Entry *>(e->base.Flink);
        }
        while (&e->base != &m_Head);
    }

    size_t size()
    {
        size_t n = 0;
        foreach([&n](auto) [[msvc::forceinline]] {
            n++;
        });

        return n;
    }

  private:
    LIST_ENTRY m_Head;
};

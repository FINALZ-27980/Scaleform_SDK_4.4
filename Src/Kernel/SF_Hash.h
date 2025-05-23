/**************************************************************************

PublicHeader:   None
Filename    :   SF_Hash.h
Content     :   Template hash-table/set implementation
Created     :   August 20, 2001
Authors     :   Michael Antonov, Sergey Sikorskiy

Copyright   :   Copyright 2011 Autodesk, Inc. All Rights reserved.

Use of this software is subject to the terms of the Autodesk license
agreement provided at the time of installation or download, or which
otherwise accompanies this software in either electronic or hard copy form.

**************************************************************************/

#ifndef INC_SF_Kernel_Hash_H
#define INC_SF_Kernel_Hash_H

#include "SF_Allocator.h"
#include "SF_Alg.h"

// ***** Disable template-unfriendly MS VC++ warnings
#if defined(SF_CC_MSVC)
// Pragma to prevent long name warnings in in VC++
#pragma warning(disable : 4503)
#endif

#undef new

namespace Scaleform {

// ***** Hash Table Implementation

// HastSet and Hash.
//
// Hash table, linear probing, internal chaining.  One  interesting/nice thing
// about this implementation is that the table itself is a flat chunk of memory
// containing no pointers, only relative indices. If the key and value types
// of the Hash contain no pointers, then the Hash can be serialized using raw IO.
//
// Never shrinks, unless you explicitly Clear() it.  Expands on
// demand, though.  For best results, if you know roughly how big your
// table will be, default it to that size when you create it.
//
// Key usability feature:
//
//   1. Allows node hash values to either be cached or not.
//
//   2. Allows for alternative keys with methods such as GetAlt(). Handy
//      if you need to search nodes by their components; no need to create
//      temporary nodes.
//


// *** Hash functors:
//
//  IdentityHash  - use when the key is already a good hash
//  HFixedSizeHash - general hash based on object's in-memory representation.


// Hash is just the input value; can use this for integer-indexed hash tables.
template<class C>
class IdentityHash
{
public:
    UPInt operator()(const C& data) const
    { return (UPInt) data; }
};

// Computes a hash of an object's representation.
// WARNING: If you are using a struct/class for the key ('C') that may be padded then the padded part 
//          may be uninitialized, due to C++ object member initialization. In this case, make sure your 
//          key class initializes the padding, or has manual padding to account for this.
template<class C>
class FixedSizeHash
{
public:
    // Alternative: "sdbm" hash function, suggested at same web page
    // above, http::/www.cse.yorku.ca/~oz/hash.html
    // This is somewhat slower then Bernstein, but it works way better than the above
    // hash function for hashing large numbers of 32-bit ints.
    static SF_INLINE UPInt SDBM_Hash(const void* data_in, UPInt size, UPInt seed = 5381)     
    {
        const UByte* data = (const UByte*) data_in;
        UPInt        h = seed;
        while (size > 0)
        {
            size--;
            h = (h << 16) + (h << 6) - h + (UPInt)data[size];
        }   
        return h;
    }

    UPInt operator()(const C& data) const
    {
        unsigned char*  p = (unsigned char*) &data;
        int size = sizeof(C);

        return SDBM_Hash(p, size);
    }
};

// *** HashsetEntry Entry types. 

// Compact hash table Entry type that re-computes hash keys during hash traversal.
// Good to use if the hash function is cheap or the hash value is already cached in C.
template<class C, class HashF>
class HashsetEntry
{
public:
    // Internal chaining for collisions.
    SPInt       NextInChain;
    C           Value;

    HashsetEntry()
        : NextInChain(-2) { }
    HashsetEntry(const HashsetEntry& e)
        : NextInChain(e.NextInChain), Value(e.Value)
    {
        SF_ASSERT(NextInChain >= -2);
    }
    HashsetEntry(const C& key, SPInt next)
        : NextInChain(next), Value(key)
    {
        SF_ASSERT(NextInChain >= -2);
    }

    bool    IsEmpty() const          { return NextInChain == -2;  }
    bool    IsEndOfChain() const     { return NextInChain == -1;  }

    // Cached hash value access - can be optimized bu storing hash locally.
    // Mask value only needs to be used if SetCachedHash is not implemented.
    UPInt   GetCachedHash(UPInt maskValue) const  { return HashF()(Value) & maskValue; }
    void    SetCachedHash(UPInt)                  {}

    void    Clear()
    {        
        Value.~C(); // placement delete
        NextInChain = -2;
    }
    // Free is only used from dtor of hash; Clear is used during regular operations:
    // assignment, hash reallocations, value reassignments, so on.
    void    Free() { Clear(); }
};

// Hash table Entry type that caches the Entry hash value for nodes, so that it
// does not need to be re-computed during access.
template<class C, class HashF>
class HashsetCachedEntry
{
public:
    // Internal chaining for collisions.
    SPInt       NextInChain;
    UPInt       HashValue;
    C           Value;

    HashsetCachedEntry()
        : NextInChain(-2) { }
    HashsetCachedEntry(const HashsetCachedEntry& e)
        : NextInChain(e.NextInChain), HashValue(e.HashValue), Value(e.Value)
    {
        SF_ASSERT(NextInChain >= -2);
    }
    HashsetCachedEntry(const C& key, SPInt next)
        : NextInChain(next), HashValue(), Value(key)
    {
        SF_ASSERT(NextInChain >= -2);
    }

    bool    IsEmpty() const          { return NextInChain == -2;  }
    bool    IsEndOfChain() const     { return NextInChain == -1;  }

    // Cached hash value access - can be optimized bu storing hash locally.
    // Mask value only needs to be used if SetCachedHash is not implemented.
    UPInt   GetCachedHash(UPInt maskValue) const  { SF_UNUSED(maskValue); return HashValue; }
    void    SetCachedHash(UPInt hashValue)        { HashValue = hashValue; }

    void    Clear()
    {
        Value.~C();
        NextInChain = -2;
    }
    // Free is only used from dtor of hash; Clear is used during regular operations:
    // assignment, hash reallocations, value reassignments, so on.
    void    Free() { Clear(); }
};



// *** HashSet implementation - relies on either cached or regular entries.
//
// Use: Entry = HashsetCachedEntry<C, HashF> if hashes are expensive to
//              compute and thus need caching in entries.
//      Entry = HashsetEntry<C, HashF> if hashes are already externally cached.
//
template<class C, class HashF = FixedSizeHash<C>,
         class AltHashF = HashF, 
         class Allocator = AllocatorGH<C>,         
         class Entry = HashsetCachedEntry<C, HashF> >
class HashSetBase
{
    enum { HashMinSize = 8 };

public:
    SF_MEMORY_REDEFINE_NEW(HashSetBase, Allocator::StatId)

    typedef HashSetBase<C, HashF, AltHashF, Allocator, Entry>    SelfType;

    HashSetBase() : pTable(NULL)                       {   }
    HashSetBase(int sizeHint) : pTable(NULL)           { SetCapacity(this, sizeHint);  }
    explicit HashSetBase(void* pmemAddr) : pTable(NULL){ SF_UNUSED(pmemAddr);  }
    HashSetBase(void* pmemAddr, int sizeHint) : pTable(NULL) { SetCapacity(pmemAddr, sizeHint);  }
    HashSetBase(const SelfType& src) : pTable(NULL)    { Assign(this, src); }
    ~HashSetBase()                                     
    { 
        if (pTable)
        {
            // Delete the entries.
            for (UPInt i = 0, n = pTable->SizeMask; i <= n; i++)
            {
                Entry*  e = &E(i);
                if (!e->IsEmpty())             
                    e->Free();
            }            

            Allocator::Free(pTable);
            pTable = NULL;
        }
    }


    void Assign(void* pmemAddr, const SelfType& src)
    {
        Clear();
        if (src.IsEmpty() == false)
        {
            SetCapacity(pmemAddr, src.GetSize());

            for (ConstIterator it = src.Begin(); it != src.End(); ++it)
            {
                Add(pmemAddr, *it);
            }
        }
    }


    // Remove all entries from the HashSet table.
    void Clear() 
    {
        if (pTable)
        {
            // Delete the entries.
            for (UPInt i = 0, n = pTable->SizeMask; i <= n; i++)
            {
                Entry*  e = &E(i);
                if (!e->IsEmpty())             
                    e->Clear();
            }            
                
            Allocator::Free(pTable);
            pTable = NULL;
        }
    }

    // Returns true if the HashSet is empty.
    bool IsEmpty() const
    {
        return pTable == NULL || pTable->EntryCount == 0;
    }


    // Set a new or existing value under the key, to the value.
    // Pass a different class of 'key' so that assignment reference object
    // can be passed instead of the actual object.
    template<class CRef>
    void Set(void* pmemAddr, const CRef& key)
    {
        const UPInt hashValue = HashF()(key);
        SPInt index = (SPInt)-1;

        if (pTable != NULL)
            index = findIndexCore(key, hashValue & pTable->SizeMask);

        if (index >= 0)
        {            
            E(index).Value = key;
        }
        else
        {
            // Entry under key doesn't exist.
            add(pmemAddr, key, hashValue);
        }
    }

    template<class CRef>
    inline void Add(void* pmemAddr, const CRef& key)
    {
        UPInt hashValue = HashF()(key);
        add(pmemAddr, key, hashValue);
    }

    // Remove by alternative key.
    // Return true on success.
    template<class K>
    bool RemoveAlt(const K& key)
    {   
        if (pTable == NULL)
            return false;

        const UPInt hashValue = AltHashF()(key);
        SPInt index = hashValue & pTable->SizeMask;

        Entry*  e = &E(index);

        // If empty node or occupied by collider, we have nothing to remove.
        if (e->IsEmpty() || (e->GetCachedHash(pTable->SizeMask) != (UPInt)index))
            return false;        

        // Save index
        const SPInt naturalIndex = index;
        SPInt prevIndex    = -1;

        while ((e->GetCachedHash(pTable->SizeMask) != (UPInt)naturalIndex) || !(e->Value == key))
        {
            // Keep looking through the chain.
            prevIndex   = index;
            SF_ASSERT(e->NextInChain >= -2);
            index       = e->NextInChain;
            if (index == -1)
                return false; // End of chain, item not found
            e = &E(index);
        }

        // Found it - our item is at index
        if (naturalIndex == index)
        {
            // If we have a follower, move it to us
            if (!e->IsEndOfChain())
            {               
                SF_ASSERT(e->NextInChain >= -2);
                Entry*  enext = &E(e->NextInChain);
                e->Clear();
                new (e) Entry(*enext);
                // Point us to the follower's cell that will be cleared
                e = enext;
            }
        }
        else
        {
            // We are not at natural index, so deal with the prev items next index
            SF_ASSERT(e->NextInChain >= -2);
            E(prevIndex).NextInChain = e->NextInChain;
        }

        // Clear us, of the follower cell that was moved.
        e->Clear();
        --pTable->EntryCount;
        // Should we check the size to condense hash? ...
        return true;
    }

    // Remove by main key.
    // Return true on success.
    template<class CRef>
    bool Remove(const CRef& key)
    {
        return RemoveAlt(key);
    }

    // Retrieve the pointer to a value under the given key.
    //  - If there's no value under the key, then return NULL.    
    //  - If there is a value, return the pointer.    
    template<class K>
    C* Get(const K& key)
    {
        SPInt   index = findIndex(key);
        if (index >= 0)        
            return &E(index).Value;        
        return 0;
    }   

    template<class K>
    const C* Get(const K& key) const
    {
        SPInt   index = findIndex(key);
        if (index >= 0)        
            return &E(index).Value;        
        return 0;
    }

    // Alternative key versions of Get. Used by Hash.
    template<class K>
    const C* GetAlt(const K& key) const
    {
        SPInt   index = findIndexAlt(key);
        if (index >= 0)        
            return &E(index).Value;
        return 0;
    }

    template<class K>
    C* GetAlt(const K& key)
    {
        SPInt   index = findIndexAlt(key);
        if (index >= 0)        
            return &E(index).Value;
        return 0;
    }   

    template<class K>
    bool GetAlt(const K& key, C* pval) const
    {
        SPInt   index = findIndexAlt(key);
        if (index >= 0)
        {
            if (pval)
                *pval = E(index).Value;
            return true;
        }
        return false;
    }


    UPInt GetSize() const
    {
        return pTable == NULL ? 0 : (UPInt)pTable->EntryCount;
    }


    // Resize the HashSet table to fit one more Entry.  Often this
    // doesn't involve any action.
    void CheckExpand(void* pmemAddr)  
    {
        if (pTable == NULL)
        {
            // Initial creation of table.  Make a minimum-sized table.
            setRawCapacity(pmemAddr, HashMinSize);
        }
        else if (pTable->EntryCount * 5 > (pTable->SizeMask + 1) * 4)
        {
            // pTable is more than 5/4 ths full.  Expand.
            setRawCapacity(pmemAddr, (pTable->SizeMask + 1) * 2);
        }
    }

    // Hint the bucket count to >= n.
    void Resize(void* pmemAddr, UPInt n)    
    {
        // Not really sure what this means in relation to
        // STLport's hash_map... they say they "increase the
        // bucket count to at least n" -- but does that mean
        // their real capacity after Resize(n) is more like
        // n*2 (since they do linked-list chaining within
        // buckets?).
        SetCapacity(pmemAddr, n);
    }

    // Size the HashSet so that it can comfortably contain the given
    // number of elements.  If the HashSet already contains more
    // elements than newSize, then this may be a no-op.
    void SetCapacity(void* pmemAddr, UPInt newSize)
    {
        UPInt newRawSize = (newSize * 5) / 4;
        if (newRawSize <= GetSize())
            return;
        setRawCapacity(pmemAddr, newRawSize);
    }

    // Disable inappropriate 'operator ->' warning on MSVC6.
#ifdef SF_CC_MSVC
#if (SF_CC_MSVC < 1300)
# pragma warning(disable : 4284)
#endif
#endif

    // Iterator API, like STL.
    struct ConstIterator
    {   
        const C&    operator * () const
        {            
            SF_ASSERT(Index >= 0 && Index <= (SPInt)pHash->pTable->SizeMask);
            return pHash->E(Index).Value;
        }

        const C*    operator -> () const
        {
            SF_ASSERT(Index >= 0 && Index <= (SPInt)pHash->pTable->SizeMask);
            return &pHash->E(Index).Value;
        }

        void    operator ++ ()
        {
            // Find next non-empty Entry.
            if (Index <= (SPInt)pHash->pTable->SizeMask)
            {
                Index++;
                while ((UPInt)Index <= pHash->pTable->SizeMask &&
                    pHash->E(Index).IsEmpty())
                {
                    Index++;
                }
            }
        }

        bool    operator == (const ConstIterator& it) const
        {
            if (IsEnd() && it.IsEnd())
            {
                return true;
            }
            else
            {
                return (pHash == it.pHash) && (Index == it.Index);
            }
        }

        bool    operator != (const ConstIterator& it) const
        {
            return ! (*this == it);
        }

        bool    IsEnd() const
        {
            return (pHash == NULL) || 
                (pHash->pTable == NULL) || 
                (Index > (SPInt)pHash->pTable->SizeMask);
        }

        ConstIterator()
            : pHash(NULL), Index(0)
        { }

    public:
        // Constructor was intentionally made public to allow create
        // iterator with arbitrary index.
        ConstIterator(const SelfType* h, SPInt index)
            : pHash(h), Index(index)
        { }

        const SelfType* GetContainer() const
        {
            return pHash;
        }
        SPInt GetIndex() const
        {
            return Index;
        }

    protected:
        friend class HashSetBase<C, HashF, AltHashF, Allocator, Entry>;

        const SelfType* pHash;
        SPInt           Index;
    };

    friend struct ConstIterator;


    // Non-const Iterator; Get most of it from ConstIterator.
    struct Iterator : public ConstIterator
    {
        // Allow non-const access to entries.
        C&  operator*() const
        {            
            SF_ASSERT(ConstIterator::Index >= 0 && ConstIterator::Index <= (SPInt)ConstIterator::pHash->pTable->SizeMask);
            return const_cast<SelfType*>(ConstIterator::pHash)->E(ConstIterator::Index).Value;
        }    

        C*  operator->() const 
        {
            return &(operator*());
        }

        Iterator()
            : ConstIterator(NULL, 0)
        { }

        // Removes current element from Hash
        // Return true on success.
        void Remove()
        {
            RemoveAlt(operator*());
        }

        // Return true on success.
        template <class K>
        bool RemoveAlt(const K& key)
        {
            SelfType*   phash = const_cast<SelfType*>(ConstIterator::pHash);
            //Entry*      ee = &phash->E(ConstIterator::Index);
            //const C&    key = ee->Value;

            UPInt       hashValue = AltHashF()(key);
            SPInt       index     = hashValue & phash->pTable->SizeMask;

            Entry*      e = &phash->E(index);

            // If empty node or occupied by collider, we have nothing to remove.
            if (e->IsEmpty() || (e->GetCachedHash(phash->pTable->SizeMask) != (UPInt)index))
                return false;

            // Save index
            const SPInt naturalIndex = index;
            SPInt prevIndex    = -1;

            while ((e->GetCachedHash(phash->pTable->SizeMask) != (UPInt)naturalIndex) || !(e->Value == key))
            {
                // Keep looking through the chain.
                prevIndex   = index;
                SF_ASSERT(e->NextInChain >= -2);
                index       = e->NextInChain;
                if (index == -1)
                    return false; // End of chain, item not found
                e = &phash->E(index);
            }

            if (index == (SPInt)ConstIterator::Index)
            {
                // Found it - our item is at index
                if (naturalIndex == index)
                {
                    // If we have a follower, move it to us
                    if (!e->IsEndOfChain())
                    {               
                        SF_ASSERT(e->NextInChain >= -2);
                        Entry*  enext = &phash->E(e->NextInChain);
                        e->Clear();
                        new (e) Entry(*enext);
                        // Point us to the follower's cell that will be cleared
                        e = enext;
                        --ConstIterator::Index;
                    }
                }
                else
                {
                    // We are not at natural index, so deal with the prev items next index
                    SF_ASSERT(e->NextInChain >= -2);
                    phash->E(prevIndex).NextInChain = e->NextInChain;
                }

                // Clear us, of the follower cell that was moved.
                e->Clear();
                --phash->pTable->EntryCount;
            }
            else 
                SF_ASSERT(0); //?

            return true;
        }

    public:
        // Constructor was intentionally made public to allow create
        // iterator with arbitrary index.
        Iterator(const SelfType* h, SPInt index)
            : ConstIterator(h, index)
        { }
    };

    friend struct Iterator;

    Iterator    Begin()
    {
        if (pTable == 0)
            return Iterator(NULL, 0);

        // Scan till we hit the First valid Entry.
        UPInt  i0 = 0;
        while (i0 <= pTable->SizeMask && E(i0).IsEmpty())
        {
            i0++;
        }
        return Iterator(this, i0);
    }
    Iterator        End()           { return Iterator(NULL, 0); }

    ConstIterator   Begin() const   { return const_cast<SelfType*>(this)->Begin();     }
    ConstIterator   End() const     { return const_cast<SelfType*>(this)->End();   }

    template<class K>
    Iterator Find(const K& key)
    {
        SPInt index = findIndex(key);
        if (index >= 0)        
            return Iterator(this, index);        
        return Iterator(NULL, 0);
    }

    template<class K>
    Iterator FindAlt(const K& key)
    {
        SPInt index = findIndexAlt(key);
        if (index >= 0)        
            return Iterator(this, index);        
        return Iterator(NULL, 0);
    }

    template<class K>
    ConstIterator Find(const K& key) const       { return const_cast<SelfType*>(this)->Find(key); }

    template<class K>
    ConstIterator FindAlt(const K& key) const    { return const_cast<SelfType*>(this)->FindAlt(key); }

private:
    // Find the index of the matching Entry.  If no match, then return -1.
    template<class K>
    SPInt findIndex(const K& key) const
    {
        if (pTable == NULL)
            return -1;
        UPInt hashValue = HashF()(key) & pTable->SizeMask;
        return findIndexCore(key, hashValue);
    }

    template<class K>
    SPInt findIndexAlt(const K& key) const
    {
        if (pTable == NULL)
            return -1;
        UPInt hashValue = AltHashF()(key) & pTable->SizeMask;
        return findIndexCore(key, hashValue);
    }

    // Find the index of the matching Entry.  If no match, then return -1.
    template<class K>
    SPInt findIndexCore(const K& key, UPInt hashValue) const
    {
        // Table must exist.
        SF_ASSERT(pTable != 0);
        // Hash key must be 'and-ed' by the caller.
        SF_ASSERT((hashValue & ~pTable->SizeMask) == 0);

        UPInt           index = hashValue;
        const Entry*    e     = &E(index);
        SF_ASSERT(e->NextInChain >= -2);

        // If empty or occupied by a collider, not found.
        if (e->IsEmpty() || (e->GetCachedHash(pTable->SizeMask) != index))
            return -1;

        while(1)
        {
            SF_ASSERT(e->GetCachedHash(pTable->SizeMask) == hashValue);

            if (e->GetCachedHash(pTable->SizeMask) == hashValue && e->Value == key)
            {
                // Found it.
                return index;
            }
            // Values can not be equal at this point.
            // That would mean that the hash key for the same value differs.
            SF_ASSERT(!(e->Value == key));

            // Keep looking through the chain.
            SF_ASSERT(e->NextInChain >= -2);
            index = e->NextInChain;
            if (index == (UPInt)-1)
                break; // end of chain

            e = &E(index);
            SF_ASSERT(!e->IsEmpty());
        }
        return -1;
    }


    // Add a new value to the HashSet table, under the specified key.
    template<class CRef>
    void add(void* pmemAddr, const CRef& key, UPInt hashValue)
    {
        CheckExpand(pmemAddr);
        hashValue &= pTable->SizeMask;

        pTable->EntryCount++;

        const SPInt index   = hashValue;
        Entry* naturalEntry = &(E(index));

        if (naturalEntry->IsEmpty())
        {
            // Put the new Entry in.
            new (naturalEntry) Entry(key, -1);
        }
        else
        {
            // Find a blank spot.
            SPInt blankIndex = index;
            do {
                blankIndex = (blankIndex + 1) & pTable->SizeMask;
            } while(!E(blankIndex).IsEmpty());

            Entry* blankEntry = &E(blankIndex);

            if (naturalEntry->GetCachedHash(pTable->SizeMask) == (UPInt)index)
            {
                // Collision.  Link into this chain.

                // Move existing list head.
                new (blankEntry) Entry(*naturalEntry);    // placement new, copy ctor

                // Put the new info in the natural Entry.
                naturalEntry->Value       = key;
                SF_ASSERT(blankIndex >= 0 && blankIndex <= (SPInt)pTable->SizeMask);
                naturalEntry->NextInChain = blankIndex;
            }
            else
            {
                // Existing Entry does not naturally
                // belong in this slot.  Existing
                // Entry must be moved.

                // Find natural location of collided element (i.e. root of chain)
                SPInt collidedIndex = naturalEntry->GetCachedHash(pTable->SizeMask);
                SF_ASSERT(collidedIndex >= 0 && collidedIndex <= (SPInt)pTable->SizeMask);
                for (;;)
                {
                    Entry*  e = &E(collidedIndex);
                    SF_ASSERT(e->NextInChain >= -1);
                    if (e->NextInChain == index)
                    {
                        // Here's where we need to splice.
                        new (blankEntry) Entry(*naturalEntry);
                        e->NextInChain = blankIndex;
                        break;
                    }
                    SF_ASSERT(e->NextInChain >= -1);
                    collidedIndex = e->NextInChain;
                    SF_ASSERT(collidedIndex >= 0 && collidedIndex <= (SPInt)pTable->SizeMask);
                }

                // Put the new data in the natural Entry.
                naturalEntry->Value       = key;
                naturalEntry->NextInChain = -1;                
            }            
        }

        // Record hash value: has effect only if cached node is used.
        naturalEntry->SetCachedHash(hashValue);
    }

    // Index access helpers.
    Entry& E(UPInt index)
    {
        // Must have pTable and access needs to be within bounds.
        SF_ASSERT(index <= pTable->SizeMask);
        return *(((Entry*) (pTable + 1)) + index);
    }
    const Entry& E(UPInt index) const
    {        
        SF_ASSERT(index <= pTable->SizeMask);
        return *(((Entry*) (pTable + 1)) + index);
    }


    // Resize the HashSet table to the given size (Rehash the
    // contents of the current table).  The arg is the number of
    // HashSet table entries, not the number of elements we should
    // actually contain (which will be less than this).
    void    setRawCapacity(void* pheapAddr, UPInt newSize)    
    {
        if (newSize == 0)
        {
            // Special case.
            Clear();
            return;
        }

        // Minimum size; don't incur rehashing cost when expanding
        // very small tables. Not that we perform this check before 
        // 'log2f' call to avoid fp exception with newSize == 1.
        if (newSize < HashMinSize)        
            newSize = HashMinSize;       
        else
        {
            // Force newSize to be a power of two.
            int bits = Alg::UpperBit(newSize-1) + 1; // Chop( Log2f((float)(newSize-1)) + 1);
            SF_ASSERT((UPInt(1) << bits) >= newSize);
            newSize = UPInt(1) << bits;
        }

        SelfType  newHash;
        newHash.pTable = (TableType*)
            Allocator::Alloc(
                pheapAddr, 
                sizeof(TableType) + sizeof(Entry) * newSize,
                __FILE__, __LINE__);
        // Need to do something on alloc failure!
        SF_ASSERT(newHash.pTable);

        newHash.pTable->EntryCount = 0;
        newHash.pTable->SizeMask = newSize - 1;
        UPInt i, n;

        // Mark all entries as empty.
        for (i = 0; i < newSize; i++)
            newHash.E(i).NextInChain = -2;

        // Copy stuff to newHash
        if (pTable)
        {            
            for (i = 0, n = pTable->SizeMask; i <= n; i++)
            {
                Entry*  e = &E(i);
                if (e->IsEmpty() == false)
                {
                    // Insert old Entry into new HashSet.
                    newHash.Add(pheapAddr, e->Value);
                    // placement delete of old element
                    e->Clear();
                }
            }

            // Delete our old data buffer.
            Allocator::Free(pTable);
        }

        // Steal newHash's data.
        pTable = newHash.pTable;
        newHash.pTable = NULL;
    }

    struct TableType
    {
        UPInt EntryCount;
        UPInt SizeMask;
        // Entry array follows this structure
        // in memory.
    };
    TableType*  pTable;
};




template<class C, class HashF = FixedSizeHash<C>,
         class AltHashF = HashF, 
         class Allocator = AllocatorGH<C>,         
         class Entry = HashsetCachedEntry<C, HashF> >
class HashSet : public HashSetBase<C, HashF, AltHashF, Allocator, Entry>
{
public:
    typedef HashSetBase<C, HashF, AltHashF, Allocator, Entry> BaseType;
    typedef HashSet<C, HashF, AltHashF, Allocator, Entry>     SelfType;
    typedef C                                                 ValueType;

    HashSet()                                      {   }
    HashSet(int sizeHint) : BaseType(sizeHint)     {   }
    explicit HashSet(void* pheap) : BaseType(pheap)                {   }
    HashSet(void* pheap, int sizeHint) : BaseType(pheap, sizeHint) {   }
    HashSet(const SelfType& src) : BaseType(src)   {   }
    ~HashSet()                                     {   }

    void operator = (const SelfType& src)   { BaseType::Assign(this, src); }

    // Set a new or existing value under the key, to the value.
    // Pass a different class of 'key' so that assignment reference object
    // can be passed instead of the actual object.
    template<class CRef>
    void Set(const CRef& key)
    {
        BaseType::Set(this, key);
    }

    template<class CRef>
    inline void Add(const CRef& key)
    {
        BaseType::Add(this, key);
    }

    // Resize the HashSet table to fit one more Entry.  Often this
    // doesn't involve any action.
    void CheckExpand()  
    {
        BaseType::CheckExpand(this);
    }

    // Hint the bucket count to >= n.
    void Resize(UPInt n)    
    {
        BaseType::SetCapacity(this, n);
    }

    // Size the HashSet so that it can comfortably contain the given
    // number of elements.  If the HashSet already contains more
    // elements than newSize, then this may be a no-op.
    void SetCapacity(UPInt newSize)
    {
        BaseType::SetCapacity(this, newSize);
    }

};

// HashSet for local member only allocation (auto-heap).
template<class C, class HashF = FixedSizeHash<C>,
         class AltHashF = HashF, 
         int SID = Stat_Default_Mem,         
         class Entry = HashsetCachedEntry<C, HashF> >
class HashSetLH : public HashSet<C, HashF, AltHashF, AllocatorLH<C, SID>, Entry>
{
public:
    typedef HashSetLH<C, HashF, AltHashF, SID, Entry>                   SelfType;
    typedef HashSet<C, HashF, AltHashF, AllocatorLH<C, SID>, Entry >    BaseType;
    typedef C                                                           ValueType;

    // Delegated constructors.
    HashSetLH()                                      { }
    HashSetLH(int sizeHint) : BaseType(sizeHint)     { }
    HashSetLH(const SelfType& src) : BaseType(src)   { }
    ~HashSetLH()                                     { }
    
    void    operator = (const SelfType& src)
    {
        BaseType::operator = (src);
    }
};

template<class C, class HashF = FixedSizeHash<C>,
         class AltHashF = HashF, 
         int SID = Stat_Default_Mem,         
         class Entry = HashsetCachedEntry<C, HashF> >
class HashSetDH : public HashSetBase<C, HashF, AltHashF, AllocatorDH<C, SID>, Entry>
{
    void* pHeap;
public:
    typedef HashSetDH<C, HashF, AltHashF, SID, Entry>                       SelfType;
    typedef HashSetBase<C, HashF, AltHashF, AllocatorDH<C, SID>, Entry>     BaseType;
    typedef C                                                               ValueType;

    explicit HashSetDH(void* pheap) : pHeap(pheap)                                 {   }
    HashSetDH(void* pheap, int sizeHint) : BaseType(pheap, sizeHint), pHeap(pheap) {   }
    HashSetDH(const SelfType& src) : BaseType(src), pHeap(src.pHeap)               {   }
    ~HashSetDH()                                     {   }

    void operator = (const SelfType& src)
    { 
        BaseType::Assign(src.pHeap, src); 
        pHeap = src.pHeap;
    }

    // Set a new or existing value under the key, to the value.
    // Pass a different class of 'key' so that assignment reference object
    // can be passed instead of the actual object.
    template<class CRef>
    void Set(const CRef& key)
    {
        BaseType::Set(pHeap, key);
    }

    template<class CRef>
    inline void Add(const CRef& key)
    {
        BaseType::Add(pHeap, key);
    }

    // Resize the HashSet table to fit one more Entry.  Often this
    // doesn't involve any action.
    void CheckExpand()  
    {
        BaseType::CheckExpand(this);
    }

    // Hint the bucket count to >= n.
    void Resize(UPInt n)    
    {
        BaseType::SetCapacity(pHeap, n);
    }

    // Size the HashSet so that it can comfortably contain the given
    // number of elements.  If the HashSet already contains more
    // elements than newSize, then this may be a no-op.
    void SetCapacity(UPInt newSize)
    {
        BaseType::SetCapacity(pHeap, newSize);
    }

};

// HashSet with uncached hash code; declared for convenience.
template<class C, class HashF = FixedSizeHash<C>,
                  class AltHashF = HashF,
                  class Allocator = AllocatorGH<C> >
class HashSetUncached : public HashSet<C, HashF, AltHashF, Allocator, HashsetEntry<C, HashF> >
{
public:
    
    typedef HashSetUncached<C, HashF, AltHashF, Allocator>                  SelfType;
    typedef HashSet<C, HashF, AltHashF, Allocator, HashsetEntry<C, HashF> > BaseType;
    typedef C                                                               ValueType;

    // Delegated constructors.
    HashSetUncached()                                        { }
    HashSetUncached(int sizeHint) : BaseType(sizeHint)       { }
    HashSetUncached(const SelfType& src) : BaseType(src)     { }
    ~HashSetUncached()                                       { }
    
    void    operator = (const SelfType& src)
    {
        BaseType::operator = (src);
    }
};

// HashSet with uncached hash code, for local-only use.
template<class C, class HashF = FixedSizeHash<C>,
                  class AltHashF = HashF,
                  int SID = Stat_Default_Mem >
class HashSetUncachedLH : public HashSet<C, HashF, AltHashF, AllocatorLH<C, SID>, HashsetEntry<C, HashF> >
{
public:
    
    typedef HashSetUncachedLH<C, HashF, AltHashF, SID>                                  SelfType;
    typedef HashSet<C, HashF, AltHashF, AllocatorLH<C, SID>, HashsetEntry<C, HashF> >   BaseType;
    typedef C                                                                           ValueType;

    // Delegated constructors.
    HashSetUncachedLH()                                    { }
    HashSetUncachedLH(int sizeHint) : BaseType(sizeHint)   { }
    HashSetUncachedLH(const SelfType& src) : BaseType(src) { }
    ~HashSetUncachedLH()                                   { }
    
    void    operator = (const SelfType& src)
    {
        BaseType::operator = (src);
    }
};

// HashSet with uncached hash code, for using with custom heap.
template<class C, 
         class HashF = FixedSizeHash<C>,
         class AltHashF = HashF,
         int SID = Stat_Default_Mem >
class HashSetUncachedDH : public HashSetDH<C, HashF, AltHashF, SID, HashsetEntry<C, HashF> >
{
public:

    typedef HashSetUncachedDH<C, HashF, AltHashF, SID>                  SelfType;
    typedef HashSetDH<C, HashF, AltHashF, SID, HashsetEntry<C, HashF> > BaseType;
    typedef C                                                           ValueType;

    // Delegated constructors.
    explicit HashSetUncachedDH(void* pheap) : BaseType(pheap)                  { }
    HashSetUncachedDH(void* pheap, int sizeHint) : BaseType(pheap, sizeHint)   { }
    HashSetUncachedDH(const SelfType& src) : BaseType(src) { }
    ~HashSetUncachedDH()                                   { }

    void    operator = (const SelfType& src)
    {
        BaseType::operator = (src);
    }
};



// ***** Hash hash table implementation

// Node for Hash - necessary so that Hash can delegate its implementation
// to HashSet.
template<class C, class U, class HashF>
struct HashNode
{
    typedef HashNode<C, U, HashF>   SelfType;
    typedef C                       FirstType;
    typedef U                       SecondType;

    C   First;
    U   Second;

    // NodeRef is used to allow passing of elements into HashSet
    // without using a temporary object.
    struct NodeRef
    {
        const C*   pFirst;
        const U*   pSecond;

        NodeRef(const C& f, const U& s) : pFirst(&f), pSecond(&s) { }
        NodeRef(const NodeRef& src)     : pFirst(src.pFirst), pSecond(src.pSecond) { }

        // Enable computation of ghash_node_hashf.
        inline UPInt GetHash() const            { return HashF()(*pFirst); } 
        // Necessary conversion to allow HashNode::operator == to work.
        operator const C& () const              { return *pFirst; }
    };

    // Note: No default constructor is necessary.
     HashNode(const HashNode& src) : First(src.First), Second(src.Second)    { }
     HashNode(const NodeRef& src) : First(*src.pFirst), Second(*src.pSecond)  { }
    void operator = (const NodeRef& src)  { First  = *src.pFirst; Second = *src.pSecond; }

#ifdef SF_CC_RENESAS
    bool operator == (const NodeRef& src) const { return (First == *src.pFirst); }
#endif

    template<class K>
    bool operator == (const K& src) const   { return (First == src); }

    template<class K>
    static UPInt CalcHash(const K& data)   { return HashF()(data); }
    inline UPInt GetHash() const           { return HashF()(First); }

    // Hash functors used with this node. A separate functor is used for alternative
    // key lookup so that it does not need to access the '.First' element.    
    struct NodeHashF
    {    
        template<class K>
        UPInt operator()(const K& data) const { return data.GetHash(); } 
    };    
    struct NodeAltHashF
    {
        template<class K>
        UPInt operator()(const K& data) const { return HashNode<C,U,HashF>::CalcHash(data); }
    };
};



// **** Extra hashset_entry types to allow NodeRef construction.

// The big difference between the below types and the ones used in hash_set is that
// these allow initializing the node with 'typename C::NodeRef& keyRef', which
// is critical to avoid temporary node allocation on stack when using placement new.

// Compact hash table Entry type that re-computes hash keys during hash traversal.
// Good to use if the hash function is cheap or the hash value is already cached in C.
template<class C, class HashF>
class HashsetNodeEntry
{
public:
    // Internal chaining for collisions.
    SPInt NextInChain;
    C     Value;

    HashsetNodeEntry()
        : NextInChain(-2) { }
    HashsetNodeEntry(const HashsetNodeEntry& e)
        : NextInChain(e.NextInChain), Value(e.Value)
    {
        SF_ASSERT(NextInChain >= -2);
    }
    HashsetNodeEntry(const C& key, SPInt next)
        : NextInChain(next), Value(key)
    {
        SF_ASSERT(NextInChain >= -2);
    }    
    HashsetNodeEntry(const typename C::NodeRef& keyRef, SPInt next)
        : NextInChain(next), Value(keyRef)
    {
        SF_ASSERT(NextInChain >= -2);
    }

    bool    IsEmpty() const             { return NextInChain == -2;  }
    bool    IsEndOfChain() const        { return NextInChain == -1;  }
    UPInt   GetCachedHash(UPInt maskValue) const  { return HashF()(Value) & maskValue; }
    void    SetCachedHash(UPInt hashValue)        { SF_UNUSED(hashValue); }

    void    Clear()
    {        
        Value.~C(); // placement delete
        NextInChain = -2;
    }
    // Free is only used from dtor of hash; Clear is used during regular operations:
    // assignment, hash reallocations, value reassignments, so on.
    void    Free() { Clear(); }
};

// Hash table Entry type that caches the Entry hash value for nodes, so that it
// does not need to be re-computed during access.
template<class C, class HashF>
class HashsetCachedNodeEntry
{
public:
    // Internal chaining for collisions.
    SPInt NextInChain;
    UPInt HashValue;
    C     Value;

    HashsetCachedNodeEntry()
        : NextInChain(-2) { }
    HashsetCachedNodeEntry(const HashsetCachedNodeEntry& e)
        : NextInChain(e.NextInChain), HashValue(e.HashValue), Value(e.Value)    
    {
        SF_ASSERT(NextInChain >= -2);
    }
    HashsetCachedNodeEntry(const C& key, SPInt next)
        : NextInChain(next), Value(key)
    {
        SF_ASSERT(NextInChain >= -2);
    }
    HashsetCachedNodeEntry(const typename C::NodeRef& keyRef, SPInt next)
        : NextInChain(next), Value(keyRef)
    {
        SF_ASSERT(NextInChain >= -2);
    }

    bool    IsEmpty() const            { return NextInChain == -2;  }
    bool    IsEndOfChain() const       { return NextInChain == -1;  }
    UPInt   GetCachedHash(UPInt maskValue) const  { SF_UNUSED(maskValue); return HashValue; }
    void    SetCachedHash(UPInt hashValue)        { HashValue = hashValue; }

    void    Clear()
    {
        Value.~C();
        NextInChain = -2;
    }
    // Free is only used from dtor of hash; Clear is used during regular operations:
    // assignment, hash reallocations, value reassignments, so on.
    void    Free() { Clear(); }
};

// WARNING: If you are using a struct/class for the key ('C') that may be padded then the padded part 
//          may be uninitialized, due to C++ object member initialization. If this is the case, then 
//          the has computed by FixedSizeHash will be incorrect across the uninitialized portion, leading 
//          to unexpected results. In this case, make sure your key class initializes the padding, or
//          has manual padding to account for this.
template<class C, class U,
         class HashF = FixedSizeHash<C>,
         class Allocator = AllocatorGH<C>,
         class HashNode = Scaleform::HashNode<C,U,HashF>,
         class Entry = HashsetCachedNodeEntry<HashNode, typename HashNode::NodeHashF>,
         class Container =  HashSet<HashNode, typename HashNode::NodeHashF,
             typename HashNode::NodeAltHashF, Allocator,
             Entry> >
class Hash
{
public:
    SF_MEMORY_REDEFINE_NEW(Hash, Allocator::StatId)

    // Types used for hash_set.
    typedef U                                                           ValueType;
    typedef Hash<C, U, HashF, Allocator, HashNode, Entry, Container>    SelfType;

    // Actual hash table itself, implemented as hash_set.
    Container   mHash;

public:
    Hash()     {  }
    Hash(int sizeHint) : mHash(sizeHint)                        { }
    explicit Hash(void* pheap) : mHash(pheap)                   { }
    Hash(void* pheap, int sizeHint) : mHash(pheap, sizeHint)    { }
    Hash(const SelfType& src) : mHash(src.mHash)                { }
    ~Hash()                                                     { }

    void    operator = (const SelfType& src)    { mHash = src.mHash; }

    // Remove all entries from the Hash table.
    inline void    Clear() { mHash.Clear(); }
    // Returns true if the Hash is empty.
    inline bool    IsEmpty() const { return mHash.IsEmpty(); }

    // Access (set).
    inline void    Set(const C& key, const U& value)
    {
        typename HashNode::NodeRef e(key, value);
        mHash.Set(e);
    }
    inline void    Add(const C& key, const U& value)
    {
        typename HashNode::NodeRef e(key, value);
        mHash.Add(e);
    }

    // Removes an element by clearing its Entry.
    // Return true on success.
    inline bool    Remove(const C& key)
    {   
        return mHash.RemoveAlt(key);
    }
    // Return true on success.
    template<class K>
    inline bool    RemoveAlt(const K& key)
    {   
        return mHash.RemoveAlt(key);
    }

    // Retrieve the value under the given key.    
    //  - If there's no value under the key, then return false and leave *pvalue alone.
    //  - If there is a value, return true, and Set *Pvalue to the Entry's value.
    //  - If value == NULL, return true or false according to the presence of the key.    
    bool    Get(const C& key, U* pvalue) const   
    {
        const HashNode* p = mHash.GetAlt(key);
        if (p)
        {
            if (pvalue)
                *pvalue = p->Second;
            return true;
        }
        return false;
    }

    template<class K>
    bool    GetAlt(const K& key, U* pvalue) const   
    {
        const HashNode* p = mHash.GetAlt(key);
        if (p)
        {
            if (pvalue)
                *pvalue = p->Second;
            return true;
        }
        return false;
    }

    // Retrieve the pointer to a value under the given key.    
    //  - If there's no value under the key, then return NULL.    
    //  - If there is a value, return the pointer.    
    inline U*  Get(const C& key)
    {
        HashNode* p = mHash.GetAlt(key);
        return p ? &p->Second : 0;
    }
    inline const U* Get(const C& key) const
    {
        const HashNode* p = mHash.GetAlt(key);
        return p ? &p->Second : 0;
    }

    template<class K>
    inline U*  GetAlt(const K& key)
    {
        HashNode* p = mHash.GetAlt(key);
        return p ? &p->Second : 0;
    }
    template<class K>
    inline const U* GetAlt(const K& key) const
    {
        const HashNode* p = mHash.GetAlt(key);
        return p ? &p->Second : 0;
    }

    // Sizing methods - delegate to Hash.
    inline UPInt   GetSize() const              { return mHash.GetSize(); }    
    inline void    Resize(UPInt n)              { mHash.Resize(n); }
    inline void    SetCapacity(UPInt newSize)   { mHash.SetCapacity(newSize); }

    // Iterator API, like STL.
    typedef typename Container::ConstIterator   ConstIterator;
    typedef typename Container::Iterator        Iterator;

    inline Iterator        Begin()              { return mHash.Begin(); }
    inline Iterator        End()                { return mHash.End(); }
    inline ConstIterator   Begin() const        { return mHash.Begin(); }
    inline ConstIterator   End() const          { return mHash.End();   }

    Iterator        Find(const C& key)          { return mHash.FindAlt(key);  }
    ConstIterator   Find(const C& key) const    { return mHash.FindAlt(key);  }

    template<class K>
    Iterator        FindAlt(const K& key)       { return mHash.FindAlt(key);  }
    template<class K>
    ConstIterator   FindAlt(const K& key) const { return mHash.FindAlt(key);  }
};



// Local-only version of Hash
template<class C, class U,         
         class HashF = FixedSizeHash<C>,
         int SID = Stat_Default_Mem,        
         class HashNode = Scaleform::HashNode<C,U,HashF>,
         class Entry = HashsetCachedNodeEntry<HashNode, typename HashNode::NodeHashF> >
class HashLH
    : public Hash<C, U, HashF, AllocatorLH<C, SID>, HashNode, Entry>
{
public:
    typedef HashLH<C, U, HashF, SID, HashNode, Entry>               SelfType;
    typedef Hash<C, U, HashF, AllocatorLH<C, SID>, HashNode, Entry> BaseType;

    // Delegated constructors.
    HashLH()                                        { }
    HashLH(int sizeHint) : BaseType(sizeHint)       { }
    HashLH(const SelfType& src) : BaseType(src)     { }
    ~HashLH()                                       { }
    void operator = (const SelfType& src)            { BaseType::operator = (src); }
};


// Custom-heap version of Hash
template<class C, class U,         
         class HashF = FixedSizeHash<C>,
         int SID = Stat_Default_Mem,        
         class HashNode = Scaleform::HashNode<C,U,HashF>,
         class Entry = HashsetCachedNodeEntry<HashNode, typename HashNode::NodeHashF> >
class HashDH
    : public Hash<C, U, HashF, AllocatorDH<C, SID>, HashNode, Entry, 
                HashSetDH<HashNode, typename HashNode::NodeHashF,
                    typename HashNode::NodeAltHashF, SID, Entry> >
{
public:
    typedef HashDH<C, U, HashF, SID, HashNode, Entry>                  SelfType;
    typedef Hash<C, U, HashF, AllocatorDH<C, SID>, HashNode, Entry, 
        HashSetDH<HashNode, typename HashNode::NodeHashF,
            typename HashNode::NodeAltHashF, SID, Entry> >              BaseType;

    // Delegated constructors.
    explicit HashDH(void* pheap) : BaseType(pheap)                        { }
    HashDH(void* pheap, int sizeHint) : BaseType(pheap, sizeHint)         { }
    HashDH(const SelfType& src) : BaseType(src)     { }
    ~HashDH()                                       { }
    void operator = (const SelfType& src) { BaseType::operator = (src); }
};


// Hash with uncached hash code; declared for convenience.
template<class C, class U, class HashF = FixedSizeHash<C>, class Allocator = AllocatorGH<C> >
class HashUncached
    : public Hash<C, U, HashF, Allocator, HashNode<C,U,HashF>,
                   HashsetNodeEntry<HashNode<C,U,HashF>, typename HashNode<C,U,HashF>::NodeHashF> >
{
public:
    typedef HashUncached<C, U, HashF, Allocator>                                                   SelfType;
    typedef Hash<C, U, HashF, Allocator, HashNode<C,U,HashF>,
                  HashsetNodeEntry<HashNode<C,U,HashF>, typename HashNode<C,U,HashF>::NodeHashF> > BaseType;

    // Delegated constructors.
    HashUncached()                                        { }
    HashUncached(int sizeHint) : BaseType(sizeHint)       { }
    HashUncached(const SelfType& src) : BaseType(src)     { }
    ~HashUncached()                                       { }
    void operator = (const SelfType& src)                  { BaseType::operator = (src); }
};


// Local heap member-only hash
template<class C, class U, class HashF = FixedSizeHash<C>, int StatId = Stat_Default_Mem>
class HashUncachedLH
    : public HashLH<C, U, HashF, StatId, HashNode<C,U,HashF>,
                   HashsetNodeEntry<HashNode<C,U,HashF>, typename HashNode<C,U,HashF>::NodeHashF> >
{
public:
    typedef HashUncachedLH<C, U, HashF, StatId>                                          SelfType;
    typedef HashLH<C, U, HashF, StatId, HashNode<C,U,HashF>,
        HashsetNodeEntry<HashNode<C,U,HashF>, typename HashNode<C,U,HashF>::NodeHashF> > BaseType;

    // Delegated constructors.
    HashUncachedLH()                                        { }
    HashUncachedLH(int sizeHint) : BaseType(sizeHint)       { }
    HashUncachedLH(const SelfType& src) : BaseType(src)     { }
    ~HashUncachedLH()                                       { }
    void operator = (const SelfType& src)                    { BaseType::operator = (src); }
};

// Custom-heap hash
template<class C, class U, class HashF = FixedSizeHash<C>, int StatId = Stat_Default_Mem>
class HashUncachedDH
    : public HashDH<C, U, HashF, StatId, HashNode<C,U,HashF>,
    HashsetNodeEntry<HashNode<C,U,HashF>, typename HashNode<C,U,HashF>::NodeHashF> >
{
public:
    typedef HashUncachedDH<C, U, HashF, StatId>                                          SelfType;
    typedef HashDH<C, U, HashF, StatId, HashNode<C,U,HashF>,
        HashsetNodeEntry<HashNode<C,U,HashF>, typename HashNode<C,U,HashF>::NodeHashF> > BaseType;

    // Delegated constructors.
    explicit HashUncachedDH(void* pheap) : BaseType(pheap)  { }
    HashUncachedDH(void* pheap, int sizeHint) : BaseType(pheap, sizeHint) { }
    HashUncachedDH(const SelfType& src) : BaseType(src)     { }
    ~HashUncachedDH()                                       { }
    void operator = (const SelfType& src)                   { BaseType::operator = (src); }
};

// And identity hash in which keys serve as hash value. Can be uncached,
// since hash computation is assumed cheap.
template<class C, class U, class Allocator = AllocatorGH<C>, class HashF = IdentityHash<C> >
class HashIdentity
    : public HashUncached<C, U, HashF, Allocator>
{
public:
    typedef HashIdentity<C, U, Allocator, HashF> SelfType;
    typedef HashUncached<C, U, HashF, Allocator> BaseType;

    // Delegated constructors.
    HashIdentity()                                        { }
    HashIdentity(int sizeHint) : BaseType(sizeHint)       { }
    HashIdentity(const SelfType& src) : BaseType(src)     { }
    ~HashIdentity()                                       { }
    void operator = (const SelfType& src)                 { BaseType::operator = (src); }
};



// Local member-only heap hash identity
template<class C, class U, int SID = Stat_Default_Mem, class HashF = IdentityHash<C> >
class HashIdentityLH
    : public HashUncachedLH<C, U, HashF, SID>
{
public:
    typedef HashIdentityLH<C, U, SID, HashF>               SelfType;
    typedef HashUncachedLH<C, U, HashF, SID>               BaseType;

    // Delegated constructors.
    HashIdentityLH()                                       { }
    HashIdentityLH(int sizeHint) : BaseType(sizeHint)      { }
    HashIdentityLH(const SelfType& src) : BaseType(src)    { }
    ~HashIdentityLH()                                      { }
    void operator = (const SelfType& src)                  { BaseType::operator = (src); }
};

// Hash identity with specified heap
template<class C, class U, int SID = Stat_Default_Mem, class HashF = IdentityHash<C> >
class HashIdentityDH
    : public HashUncachedDH<C, U, HashF, SID>
{
public:
    typedef HashIdentityDH<C, U, SID, HashF>               SelfType;
    typedef HashUncachedDH<C, U, HashF, SID>               BaseType;

    // Delegated constructors.
    explicit HashIdentityDH(void* pheap) : BaseType(pheap) { }
    HashIdentityDH(void* pheap, int sizeHint) : BaseType(pheap, sizeHint)      { }
    HashIdentityDH(const SelfType& src) : BaseType(src)    { }
    ~HashIdentityDH()                                      { }
    void operator = (const SelfType& src)                  { BaseType::operator = (src); }
};

} // Scaleform

// Redefine operator 'new' if necessary.
#if defined(SF_DEFINE_NEW)
#define new SF_DEFINE_NEW
#endif

#endif

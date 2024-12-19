/**************************************************************************

PublicHeader:   Render
Filename    :   Render_SimpleMeshCache.h
Content     :   Implementation of mesh cache with single Allocator,
                intended as a base class for console mesh caches.
Created     :   May 2009
Authors     :   Michael Antonov

Copyright   :   Copyright 2011 Autodesk, Inc. All Rights reserved.

Use of this software is subject to the terms of the Autodesk license
agreement provided at the time of installation or download, or which
otherwise accompanies this software in either electronic or hard copy form.

**************************************************************************/

#ifndef INC_SF_Render_SimpleMeshCache_H
#define INC_SF_Render_SimpleMeshCache_H

#include "Render/Render_MeshCache.h"
#include "Render/Render_Sync.h"

namespace Scaleform { namespace Render {

class SimpleMeshBuffer;
class SimpleMeshCacheItem;
class SimpleMeshCache;


// SimpleMeshBuffer is a base MeshBuffer class that any MeshCache implementation
// that is derived from SimpleMeshCache should use.

class SimpleMeshBuffer : public Render::MeshBuffer
{
    friend class SimpleMeshCache;
public:

    enum {
        MinGranularity  = 16*1024,
        // We pass a smaller size to the Allocator to prevent it from merging
        // blocks which can be reported consecutively by PhysicalAlloc.
        // Users should still allocate and map full original size to the buffer.
        AllocSizeDelta  = sizeof(UPInt)
    };
   
    SimpleMeshBuffer(UPInt size, AllocType type, unsigned arena)
        : Render::MeshBuffer(size - AllocSizeDelta, type, arena)
    {        
        SF_ASSERT(size > AllocSizeDelta);
    }

    inline UPInt   GetFullSize() const { return GetSize() + AllocSizeDelta; }

    // Returns user maximum allocatable size given a specified buffer size.    
    inline static UPInt GetUsableSize(UPInt size) { return size - AllocSizeDelta; }
};


// SimpleMeshCacheItem stores an additional MeshBuffer pointer and
// and allocation/vertex offset data, in addition to base MeshCacheItem content.
// This class should be used as a base instead of Render::MeshCacheItem by
// all SimpleMeshCache derived mesh cache implementations.

class SimpleMeshCacheItem : public Render::MeshCacheItem
{
    friend class SimpleMeshCache;
public:
    static SimpleMeshCacheItem* Create(MeshType type,
                                       MeshCacheListSet* pcacheList, UPInt classSize,
                                       MeshBaseContent& mc, SimpleMeshBuffer* pbuffer,
                                       UPInt allocAddress, UPInt allocSize,
                                       UPInt vertexOffset, unsigned vertexCount,
                                       UPInt indexOffset, unsigned indexCount, const VertexFormat * pfmt )
    {
        UByte* memory = MeshCacheItem::alloc(Memory::GetHeapByAddress(pcacheList->GetCache()), sizeof(SimpleMeshCacheItem), mc);
        return new(memory) SimpleMeshCacheItem(type, pcacheList, classSize, mc, pbuffer, allocAddress, allocSize, 
            vertexOffset, vertexCount, indexOffset, indexCount, pfmt);
    }

    SimpleMeshCacheItem(MeshType type, MeshCacheListSet* pcacheList, UPInt classSize,
                        MeshBaseContent& mc, SimpleMeshBuffer* pbuffer,
                        UPInt allocAddress, UPInt allocSize,
                        UPInt vertexOffset, unsigned vertexCount,
                        UPInt indexOffset, unsigned indexCount, const VertexFormat * pfmt) :
            Render::MeshCacheItem(type, pcacheList, mc, classSize, allocSize, vertexCount, indexCount),
            pBuffer(pbuffer),
            AllocAddress(allocAddress),
            VertexOffset(vertexOffset),
            IndexOffset(indexOffset),
            pFormat(pfmt)

    {
    }

	SimpleMeshBuffer*   GetBuffer() const { return pBuffer; }
    UPInt			    GetAllocAddr() const { return AllocAddress; }
    UPInt			    GetVertexOffset() const { return VertexOffset; }
    UPInt			    GetIndexOffset() const { return IndexOffset; }
    const VertexFormat* GetVertexFormat() const { return pFormat; }

protected:

    SimpleMeshBuffer* pBuffer;    
    UPInt             AllocAddress;     // Absolute memory address of allocation. Points to the start of the vertex data.
    UPInt             VertexOffset;     // Offset from the MeshBuffer's start, where the vertex data begins.
    UPInt             IndexOffset;      // Offset from the MeshBuffer's start, where the index data begins.
    const VertexFormat * pFormat;       // Format of the vertex data within the buffer.
};


// SimpleMeshCache is a partial mesh cache implementation used to simplify
// platform-specific MeshCache implementation on consoles. It has the following
// characteristics:
//  - Supports allocation of multiple MeshBuffer blocks, delegating the actual
//    allocation to the createHWBuffer()/destroyHWBuffer() virtual functions.
//  - Handles grow/shrink of the cache.
//  - Implements allocBuffer() function that should be used by the PreparePrimitive
//    implementation in the derived class.
//  - Used passed RenderSync object to implement fencing.

class SimpleMeshCache : public Render::MeshCache
{   
protected:
    typedef MeshBuffer::AllocType AllocType;

    enum {
        Cache_NeverShrink = 1,
    };

    MeshCacheListSet            CacheList;
    AllocAddr                   Allocator;
    UPInt                       TotalSize;
    List<Render::MeshBuffer>    Buffers;            // Use base MeshBuffer to avoid List<> problems.
    RenderSync*                 pRenderSync;
    UInt32                      CacheFlags;
    
    inline MeshCache*   getThis() { return this; }

    // Allocates Vertex/Index buffer of specified size and adds it to free list.
    SimpleMeshBuffer*   allocMeshBuffer(UPInt size, AllocType atype, unsigned arena = 0);
    bool                releaseMeshBuffer(SimpleMeshBuffer* pbuffer);

    // Allocates reserve buffer, if any. Intended for use in Initialize implementation.
    bool                allocateReserve();
    // Cleared cache and releases all buffers including reserve. Intended for
    // use in MeshCache::Reset implementation.
    void                releaseAllBuffers();    

    bool                evictMeshesInBuffer(MeshCacheListSet::ListSlot* plist, UPInt count,
                                            SimpleMeshBuffer* pbuffer);

    // Allocates a number of bytes in the specified buffer, while evicting LRU data.
    // Buffer can contain either vertex and/or index data.
    bool                allocBuffer(UPInt* poffset, UPInt size, bool waitForCache);
    // Finds which buffer address offset returned by allocBuffer() belongs to...
    SimpleMeshBuffer*   findBuffer(UPInt address);


    // *** Virtual HW API to be implemented by derived class

    // Creates a SimpleMeshBuffer derived class and initializes its pData with
    // hw-allocated memory block of size.
    virtual SimpleMeshBuffer* createHWBuffer(UPInt size, AllocType atype, unsigned arena) = 0;
    // Frees buffers memory and deletes HW buffer.
    virtual void              destroyHWBuffer(SimpleMeshBuffer* pbufer) = 0;

    // Converts GPU buffers from one batch to another. The generic implemention in SimpleMeshCache implements this by doing the copy/conversion on the CPU,
    // assuming that GPU memory is addressable by the CPU.
    void copyAndConvertGPUBuffers( Mesh* pmesh, unsigned meshIndex, PrimitiveBatch* pbatch, UPInt vertexDataOffsetInBytes, UPInt indexDataOffsetInBytes, unsigned indexStart );

public:
    SimpleMeshCache(MemoryHeap* pheap,
                    const MeshCacheParams& params,
                    RenderSync* psync, UInt32 cacheFlags = 0);
    ~SimpleMeshCache();

    virtual void    ClearCache();
    virtual void    EndFrame();

    virtual UPInt   Evict(Render::MeshCacheItem* p, AllocAddr* pallocator = 0, MeshBase* pmesh = 0);

    virtual AllocResult AllocCacheItem(MeshCacheItem** pdata,
        MeshCacheItem::MeshType meshType,
        MeshCacheItem::MeshBaseContent &mc,
        UPInt vertexBufferSize,
        unsigned vertexCount, unsigned indexCount,
        bool waitForCache,
        const VertexFormat* pDestFormat);

    virtual void LockMeshCacheItem(Render::MeshCacheItem* pdata, UByte** pvertexDataStart, IndexType** pindexDataStart);

    RenderSync*     GetRenderSync() const { return pRenderSync; }

    virtual void    GetStats(Stats* stats);
};


}};  // namespace Scaleform::Render

#endif

/**************************************************************************

Filename    :   GL_MeshCache.cpp
Content     :   GL Mesh Cache implementation
Created     :   
Authors     :   

Copyright   :   Copyright 2011 Autodesk, Inc. All Rights reserved.

Use of this software is subject to the terms of the Autodesk license
agreement provided at the time of installation or download, or which
otherwise accompanies this software in either electronic or hard copy form.

**************************************************************************/

#include "Render/GL/GL_MeshCache.h"
#include "Kernel/SF_Debug.h"
#include "Kernel/SF_Alg.h"
#include "Kernel/SF_HeapNew.h"
#include "Render/GL/GL_Common.h"
#include "Render/GL/GL_HAL.h"

#include "Render/GL/GL_Common.h"

#include "Render/GL/GL_ExtensionMacros.h"

//#define SF_RENDER_LOG_CACHESIZE

namespace Scaleform { namespace Render { namespace GL {


MeshCacheItem* MeshCacheItem::Create(MeshType type, MeshCacheListSet* pcacheList, MeshBaseContent& mc, MeshBuffer* pvb, MeshBuffer* pib, 
                                     UPInt vertexOffset, UPInt vertexAllocSize, unsigned vertexCount, UPInt indexOffset, 
                                     UPInt indexAllocSize, unsigned indexCount )
{
    UByte* memory = MeshCacheItem::alloc(Memory::GetHeapByAddress(pcacheList->GetCache()), sizeof(MeshCacheItem), mc);
    return new(memory) MeshCacheItem(type, pcacheList, mc, pvb, pib, vertexOffset, vertexAllocSize, vertexCount, indexOffset, indexAllocSize, indexCount);
}

MeshCacheItem::MeshCacheItem(MeshType type, MeshCacheListSet* pcacheList, MeshBaseContent& mc, MeshBuffer* pvb, MeshBuffer* pib, 
                             UPInt vertexOffset, UPInt vertexAllocSize, unsigned vertexCount, UPInt indexOffset, 
                             UPInt indexAllocSize, unsigned indexCount) :
    Render::MeshCacheItem(type, pcacheList, mc, sizeof(MeshCacheItem), vertexAllocSize + indexAllocSize, vertexCount, indexCount),
    pVertexBuffer(pvb),
    pIndexBuffer(pib),
    VBAllocOffset(vertexOffset),
    VBAllocSize(vertexAllocSize),
    IBAllocOffset(indexOffset),
    IBAllocSize(indexAllocSize),
    VAOFormat(0),
    VAOOffset(0)
{
}

MeshCacheItem::~MeshCacheItem()
{
    VAO.Clear();
}

//------------------------------------------------------------------------

MeshBuffer::~MeshBuffer()
{
    if (Buffer)
        glDeleteBuffers(1, &Buffer.GetRawRef());
    if ( BufferData )
        SF_FREE(BufferData);

    Buffer = 0;
    BufferData = 0;
}

bool MeshBuffer::DoMap(UPInt offset, UPInt size)
{
    MeshCache::BufferUpdateType updateType = pHal->GetMeshCache().GetBufferUpdateType();
    if (!pData)
    {
        if (updateType == MeshCache::BufferUpdate_MapBuffer || updateType == MeshCache::BufferUpdate_MapBufferUnsynchronized)
        {
            // Unbind the current VAO, so it doesn't get modified if this is an index buffer.
            if (GetHAL()->ShouldUseVAOs())
                glBindVertexArray(0);

            glBindBuffer(Type, Buffer);

            if (updateType == MeshCache::BufferUpdate_MapBufferUnsynchronized)
            {
                // Map the entire buffer, but specify that it is unsynchronized, and manual flushing. 
                // We use fencing to ensure that the portions of the buffer we overwrite are not currently
                // in use by the GPU.
                pData = glMapBufferRange(Type, 0, Size, GL_MAP_WRITE_BIT|GL_MAP_FLUSH_EXPLICIT_BIT|GL_MAP_UNSYNCHRONIZED_BIT);
            }
            else if (updateType == MeshCache::BufferUpdate_MapBuffer)
            {
                pData = glMapBuffer(Type, GL_WRITE_ONLY);
            }
        }
        else
        {
            // Not using MapBuffer, allocate client memory.
            if (!BufferData)
                BufferData = (UByte*)SF_ALLOC(Size, StatRender_MeshStaging_Mem);
            pData = BufferData;
        }
    }

    // If we are using a method that does region updating, then record the portions that were modified.
    if (pData != 0 && (updateType == MeshCache::BufferUpdate_UpdateBuffer || updateType == MeshCache::BufferUpdate_MapBufferUnsynchronized))
    {
        MeshBufferUpdateEntry e(offset, size);
        MeshBufferUpdates.PushBack(e);
    }

    return pData != 0;
}

void MeshBuffer::Unmap()
{
    MeshCache::BufferUpdateType updateType = pHal->GetMeshCache().GetBufferUpdateType();
    if (pData && Buffer)
    {
        if (GetHAL()->ShouldUseVAOs())
            glBindVertexArray(0);

        glBindBuffer(Type, Buffer);
        
        if (updateType == MeshCache::BufferUpdate_MapBufferUnsynchronized)
        {
            // Flush the portions of the buffer that were modified.
            for (unsigned i =0; i < MeshBufferUpdates.GetSize(); ++i)
            {
                const MeshBufferUpdateEntry& e = MeshBufferUpdates[i];
                SF_UNUSED(e); // warning if glFlushMappedBufferRange is a no-op.
                glFlushMappedBufferRange(Type, e.Offset, e.Size);
            }
        }
        
        if (updateType == MeshCache::BufferUpdate_MapBufferUnsynchronized || updateType == MeshCache::BufferUpdate_MapBuffer)
        {
            GLboolean result = glUnmapBuffer(Type); // XXX - data loss can occur here
            SF_ASSERT(result);
            SF_UNUSED(result);
        }
        else if (updateType == MeshCache::BufferUpdate_UpdateBuffer)
        {
            // Update the portions of the buffer that were modified.
            for (unsigned i =0; i < MeshBufferUpdates.GetSize(); ++i)
            {
                const MeshBufferUpdateEntry& e = MeshBufferUpdates[i];
                glBufferSubData(Type, e.Offset, e.Size, ((char*)pData) +e.Offset);
            }
        }
    }
    MeshBufferUpdates.Clear();
    pData = 0;
}

UByte*  MeshBuffer::GetBufferBase() const
{
    MeshCache::BufferUpdateType updateType = pHal->GetMeshCache().GetBufferUpdateType();
    return (updateType == MeshCache::BufferUpdate_ClientBuffers) ? BufferData : 0;
}

bool MeshBuffer::allocBuffer()
{
    MeshCache::BufferUpdateType updateType = pHal->GetMeshCache().GetBufferUpdateType();
    if (Buffer)
        glDeleteBuffers(1, &Buffer.GetRawRef());

    // Unbind the current VAO, so it doesn't get modified if this is an index buffer.
    if (GetHAL()->ShouldUseVAOs())
        glBindVertexArray(0);

    if (updateType != MeshCache::BufferUpdate_ClientBuffers)
    {
        if (!Buffer)
            Buffer = *SF_NEW HALGLBuffer();
        glGenBuffers(1, &Buffer.GetRawRef());

        // Binding to the array or element target at creation is supposed to let drivers that need
        // separate vertex/index storage to know what the buffer will be used for.
        glBindBuffer(Type, Buffer);
        glBufferData(Type, Size, 0, GL_DYNAMIC_DRAW);
    }
    return 1;
}


// Helpers used to initialize default granularity sizes,
// splitting VB/Index size by 5/9.
inline UPInt calcVBGranularity(UPInt granularity)
{
    return (((granularity >> 4) * 5) / 9) << 4;
}
inline UPInt calcIBGranularity(UPInt granularity, UPInt vbGranularity)
{
    return (((granularity >> 4) - (vbGranularity >> 4)) << 4);
}


//------------------------------------------------------------------------
// ***** MeshCache

MeshCache::MeshCache(MemoryHeap* pheap, const MeshCacheParams& params)
  : Render::MeshCache(pheap, params),
    pHal(0), CacheList(getThis()),
    VertexBuffers(GL_ARRAY_BUFFER, pheap, calcVBGranularity(params.MemGranularity)),
    IndexBuffers(GL_ELEMENT_ARRAY_BUFFER, pheap,
                 calcIBGranularity(params.MemGranularity, VertexBuffers.GetGranularity())),
    UseSeparateIndexBuffers(false),
    BufferUpdate(BufferUpdate_MapBufferUnsynchronized),
    Mapped(false), VBSizeEvictedInMap(0),
    MaskEraseBatchVertexBuffer(0),
    MaskEraseBatchVAO(0)
{
}

MeshCache::~MeshCache()
{
    Reset();
}

// Initializes MeshCache for operation, including allocation of the reserve
// buffer. Typically called from SetVideoMode.
bool    MeshCache::Initialize(HAL* phal)
{
    pHal = phal;

    // Determine GL-capability settings. Needs to be called after the GL context is created.
    adjustMeshCacheParams(&Params);

    // Determine which mesh-buffer update method to use.
    BufferUpdate = BufferUpdate_Count;
    for (unsigned method = BufferUpdate_MapBufferUnsynchronized; method < BufferUpdate_Count; ++method)
    {
        switch(method)
        {
            case BufferUpdate_MapBufferUnsynchronized:
                if ((pHal->GetGraphicsDevice()->GetCaps() & (Cap_Sync|Cap_MapBufferRange)) == (Cap_Sync|Cap_MapBufferRange))
                    BufferUpdate = BufferUpdate_MapBufferUnsynchronized;
                break;
            case BufferUpdate_ClientBuffers:
                // Only restriction on using client buffers is the use of VAOs, or, using deferred context.
                if (!pHal->ShouldUseVAOs() && (pHal->GetConfigFlags() & HALConfig_SoftwareDeferredContext) == 0)
                    BufferUpdate = BufferUpdate_ClientBuffers;
                break;
            case BufferUpdate_MapBuffer:
                if (pHal->GetGraphicsDevice()->GetCaps() & Cap_MapBuffer)
                    BufferUpdate = BufferUpdate_MapBuffer;
                break;
            case BufferUpdate_UpdateBuffer:
                BufferUpdate = BufferUpdate_UpdateBuffer;
                break;
        }
        
        // If we have found a suitable method, quit.
        if (BufferUpdate != BufferUpdate_Count)
            break;
    }
    
    if (BufferUpdate == BufferUpdate_Count)
    {
        SF_DEBUG_ASSERT(0, "Unable to use any buffer update method.");
        return false;
    }
    
    if (!StagingBuffer.Initialize(pHeap, Params.StagingBufferSize))
        return false;

    UseSeparateIndexBuffers = true;

    VertexBuffers.SetGranularity(calcVBGranularity(Params.MemGranularity));
    IndexBuffers.SetGranularity(calcIBGranularity(Params.MemGranularity,
        VertexBuffers.GetGranularity()));

    if (!createStaticVertexBuffers())
    {
        Reset();
        return false;
    }

    if (Params.MemReserve &&
        !allocCacheBuffers(Params.MemReserve, MeshBuffer::AT_Reserve))
    {
        Reset();
        return false;
    }

    return true;
}

void MeshCache::Reset(bool lost)
{
    if (pHal)
    {
        destroyBuffers(MeshBuffer::AT_None, lost);
        destroyPendingBuffers(lost);
        if (!lost)
        {
            if (MaskEraseBatchVertexBuffer)
            {
                HALGLBuffer* b[1] = {&MaskEraseBatchVertexBuffer};
                glDeleteBuffers(1, b);
            }
            if (MaskEraseBatchVAO)
            {
                HALGLVertexArray* va[1] = {&MaskEraseBatchVAO};
                glDeleteVertexArrays(1, va);
            }
        }
        pHal = 0;
    }

    StagingBuffer.Reset();
}

void MeshCache::ClearCache()
{
    destroyBuffers(MeshBuffer::AT_Chunk);
    StagingBuffer.Reset();
    StagingBuffer.Initialize(pHeap, Params.StagingBufferSize);
    SF_ASSERT(BatchCacheItemHash.GetSize() == 0);
}

void MeshCache::destroyBuffers(MeshBuffer::AllocType at, bool lost)
{
    // TBD: Evict everything first!
    CacheList.EvictAll();
    VertexBuffers.DestroyBuffers(at, lost);
    IndexBuffers.DestroyBuffers(at, lost);
    ChunkBuffers.Clear();
}

bool MeshCache::SetParams(const MeshCacheParams& argParams)
{
    MeshCacheParams params(argParams);
    adjustMeshCacheParams(&params);

    if (pHal)
    {
        CacheList.EvictAll();

        if (Params.StagingBufferSize != params.StagingBufferSize)
        {
            if (!StagingBuffer.Initialize(pHeap, params.StagingBufferSize))
            {
                if (!StagingBuffer.Initialize(pHeap, Params.StagingBufferSize))
                {
                    SF_DEBUG_ERROR(1, "MeshCache::SetParams - couldn't restore StagingBuffer after fail");
                }
                return false;
            }
        }

        if ((Params.MemReserve != params.MemReserve) ||
            (Params.MemGranularity != params.MemGranularity))
        {
            destroyBuffers();

            // Allocate new reserve. If not possible, restore previous one and fail.
            if (params.MemReserve &&
                !allocCacheBuffers(params.MemReserve, MeshBuffer::AT_Reserve))
            {
                if (Params.MemReserve &&
                    !allocCacheBuffers(Params.MemReserve, MeshBuffer::AT_Reserve))
                {
                    SF_DEBUG_ERROR(1, "MeshCache::SetParams - couldn't restore Reserve after fail");
                }
                return false;
            }
        }
    }
    Params = params;
    return true;
}

void MeshCache::adjustMeshCacheParams(MeshCacheParams* p)
{
    if (p->MaxBatchInstances > SF_RENDER_MAX_BATCHES)
        p->MaxBatchInstances = SF_RENDER_MAX_BATCHES;

    if (p->VBLockEvictSizeLimit < 1024 * 256)
        p->VBLockEvictSizeLimit = 1024 * 256;

    UPInt maxStagingItemSize = p->MaxVerticesSizeInBatch +
                               sizeof(UInt16) * p->MaxIndicesInBatch;
    if (maxStagingItemSize * 2 > p->StagingBufferSize)
        p->StagingBufferSize = maxStagingItemSize * 2;
}


void MeshCache::destroyPendingBuffers(bool lost)
{
    // Destroy any pending buffers that are waiting to be destroyed (if possible).
    List<Render::MeshBuffer> remainingBuffers;
    MeshBuffer* p = (MeshBuffer*)PendingDestructionBuffers.GetFirst();
    while (!PendingDestructionBuffers.IsNull(p))
    {
        MeshCacheListSet::ListSlot& pendingFreeList = CacheList.GetSlot(MCL_PendingFree);
        MeshCacheItem* pitem = (MeshCacheItem*)pendingFreeList.GetFirst();
        bool itemsRemaining = false;
        MeshBuffer* pNext = (GL::MeshBuffer*)p->pNext;
        p->RemoveNode();
        while (!pendingFreeList.IsNull(pitem))
        {
            if ((pitem->pVertexBuffer == p) || (pitem->pIndexBuffer == p))
            {
                // If the fence is still pending, cannot destroy the buffer.
                if ( pitem->IsPending(FenceType_Vertex) )
                {
                    itemsRemaining = true;
                    remainingBuffers.PushFront(p);
                    break;
                }
            }
            pitem = (MeshCacheItem*)pitem->pNext;
        }
        if ( !itemsRemaining )
        {
            if (lost)
                p->Buffer = 0;
            delete p;
        }
        p = pNext;
    }
    PendingDestructionBuffers.PushListToFront(remainingBuffers);
}

void MeshCache::EndFrame()
{
    SF_AMP_SCOPE_RENDER_TIMER(__FUNCTION__, Amp_Profile_Level_Medium);

    CacheList.EndFrame();

    // Try and reclaim memory from items that have already been destroyed, but not freed.
    CacheList.EvictPendingFree(IndexBuffers.Allocator);
    CacheList.EvictPendingFree(VertexBuffers.Allocator);

    destroyPendingBuffers();


    // Simple Heuristic used to shrink cache. Shrink is possible once the
    // (Total_Frame_Size + LRUTailSize) exceed the allocated space by more then
    // one granularity unit. If this case, we destroy the cache buffer in the
    // order opposite to that of which it was created.

    // TBD: This may have a side effect of throwing away the current frame items
    // as well. Such effect is undesirable and can perhaps be avoided on consoles
    // with buffer data copies (copy PrevFrame content into other buffers before evict).

    UPInt totalFrameSize = CacheList.GetSlotSize(MCL_PrevFrame);
    UPInt lruTailSize    = CacheList.GetSlotSize(MCL_LRUTail);
    UPInt expectedSize   = totalFrameSize + Alg::PMin(lruTailSize, Params.LRUTailSize);
    expectedSize += expectedSize / 4; // + 25%, to account for fragmentation.

    SPInt extraSpace = getTotalSize() - (SPInt)expectedSize;
    if (extraSpace > (SPInt)Params.MemGranularity)
    {        
        while (!ChunkBuffers.IsEmpty() && (extraSpace > (SPInt)Params.MemGranularity))
        {
            MeshBuffer* p = (MeshBuffer*)ChunkBuffers.GetLast();
            p->RemoveNode();
            extraSpace -= (SPInt)p->GetSize();

            MeshBufferSet&  mbs = (p->GetBufferType() == GL_ARRAY_BUFFER) ?
                                  (MeshBufferSet&)VertexBuffers : (MeshBufferSet&)IndexBuffers;

            // Evict first! This may fail if a query is pending on a mesh inside the buffer. In that case,
            // simply store the buffer to be destroyed later.
            bool allEvicted = evictMeshesInBuffer(CacheList.GetSlots(), MCL_ItemCount, p);
            mbs.DestroyBuffer(p, false, allEvicted);
            if ( !allEvicted )
                PendingDestructionBuffers.PushBack(p);


#ifdef SF_RENDER_LOG_CACHESIZE
            LogDebugMessage(Log_Message,
                "Cache shrank to %dK. Start FrameSize = %dK, LRU = %dK\n",
                getTotalSize() / 1024, totalFrameSize/1024, lruTailSize/1024);
#endif
        }
    }    
}



// Adds a fixed-size buffer to cache reserve; expected to be released at Release.
/*
bool MeshCache::AddReserveBuffer(unsigned size, unsigned arena)
{
}
bool MeshCache::ReleaseReserveBuffer(unsigned size, unsigned arena)
{
}
*/


// Allocates Vertex/Index buffer of specified size and adds it to free list.
bool MeshCache::allocCacheBuffers(UPInt size, MeshBuffer::AllocType type, unsigned arena)
{
    if (UseSeparateIndexBuffers)
    {
        UPInt vbsize = calcVBGranularity(size);
        UPInt ibsize = calcIBGranularity(size, vbsize);

        MeshBuffer *pvb = VertexBuffers.CreateBuffer(vbsize, type, arena, pHeap, pHal);
        if (!pvb)
            return false;
        MeshBuffer   *pib = IndexBuffers.CreateBuffer(ibsize, type, arena, pHeap, pHal);
        if (!pib)
        {
            VertexBuffers.DestroyBuffer(pvb);
            return false;
        }
    }
    else
    {
        MeshBuffer *pb = VertexBuffers.CreateBuffer(size, type, arena, pHeap, pHal);
        if (!pb)
            return false;
    }

#ifdef SF_RENDER_LOG_CACHESIZE
    LogDebugMessage(Log_Message, "Cache grew to %dK\n", getTotalSize() / 1024);
#endif
    return true;
}

bool MeshCache::createStaticVertexBuffers()
{
    return createInstancingVertexBuffer() &&
           createMaskEraseBatchVertexBuffer();
}

bool MeshCache::createInstancingVertexBuffer()
{
    return true;
}

bool MeshCache::createMaskEraseBatchVertexBuffer()
{
    VertexXY16iInstance pbuffer[6 * SF_RENDER_MAX_BATCHES];
    fillMaskEraseVertexBuffer<VertexXY16iAlpha>(reinterpret_cast<VertexXY16iAlpha*>(pbuffer), SF_RENDER_MAX_BATCHES);

    HALGLBuffer* b[1] = {&MaskEraseBatchVertexBuffer};
    glGenBuffers(1, b);
    if (GetHAL()->ShouldUseVAOs())
    {
        HALGLVertexArray* va[1] = {&MaskEraseBatchVAO};
        glGenVertexArrays(1, va);
        glBindVertexArray(&MaskEraseBatchVAO);
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, &MaskEraseBatchVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(pbuffer), pbuffer, GL_STATIC_DRAW);

    if (GetHAL()->ShouldUseVAOs())
    {
        // Fill out the VAO now.
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_SHORT, false, VertexXY16iInstance::Format.Size, 0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, false, VertexXY16iInstance::Format.Size, (GLvoid*)4);
        glBindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

bool MeshCache::LockBuffers()
{
    SF_ASSERT(!Mapped);
    Mapped = true;
    VBSizeEvictedInMap = 0;
    if (pRQCaches)
        pRQCaches->SetCacheLocked(Cache_Mesh);
    return true;
}

void MeshCache::UnlockBuffers()
{
    SF_ASSERT(Mapped != 0); 
    MappedBuffers.UnmapAll();
    Mapped = false;
    if (pRQCaches)
        pRQCaches->ClearCacheLocked(Cache_Mesh);
}


bool MeshCache::evictMeshesInBuffer(MeshCacheListSet::ListSlot* plist, UPInt count,
                                    MeshBuffer* pbuffer)
{
    bool evictionFailed = false;
    for (unsigned i=0; i< count; i++)
    {
        MeshCacheItem* pitem = (MeshCacheItem*)plist[i].GetFirst();
        while (!plist[i].IsNull(pitem))
        {
            if ((pitem->pVertexBuffer == pbuffer) || (pitem->pIndexBuffer == pbuffer))
            {
                // Evict returns the number of bytes released. If this is zero, it means the mesh
                // was still in use. 
                if ( Evict(pitem) == 0 )
                {
                    evictionFailed = true;
                    SF_ASSERT(pitem->Type == MeshCacheItem::Mesh_Destroyed);

                    // We still need to delete all the addresses allocated in the buffer, because it is 
                    // going to be deleted, and AllocAddr will break otherwise.
                    if ( pitem->pVertexBuffer == pbuffer)
                    {
                        VertexBuffers.Free(pitem->VBAllocSize, pitem->pVertexBuffer, pitem->VBAllocOffset);
                        pitem->pVertexBuffer = 0;
                    }
                    if ( pitem->pIndexBuffer == pbuffer)
                    {
                        IndexBuffers.Free(pitem->IBAllocSize, pitem->pIndexBuffer, pitem->IBAllocOffset);
                        pitem->pIndexBuffer = 0;
                    }
                }

                // Evict may potentially modify the cache items, so start again.
                // This is less than ideal, but better than accessing a dangling pointer.
                pitem = (MeshCacheItem*)plist[i].GetFirst();
                continue;
            }
            pitem = (MeshCacheItem*)pitem->pNext;
        }
    }
    return !evictionFailed;
}


UPInt MeshCache::Evict(Render::MeshCacheItem* pbatch, AllocAddr* pallocator, MeshBase* pskipMesh)
{
    MeshCacheItem* p = (MeshCacheItem*)pbatch;

    // If a fence is not pending, then the memory for the item can be reclaimed immediately.
    if ( !p->IsPending(FenceType_Vertex))
    {
	    // - Free allocator data.
        UPInt vbfree = p->pVertexBuffer ? VertexBuffers.Free(p->VBAllocSize, p->pVertexBuffer, p->VBAllocOffset) : 0;
        UPInt ibfree = p->pIndexBuffer  ? IndexBuffers.Free(p->IBAllocSize, p->pIndexBuffer, p->IBAllocOffset) : 0;
	    UPInt freedSize = pallocator ? ((&VertexBuffers.GetAllocator() == pallocator) ? vbfree : ibfree) : vbfree + ibfree;

        // If we are using VAOs, then destroy the VAO now, it will not be used again.
        if (GetHAL()->ShouldUseVAOs() && p->VAO)
            glDeleteVertexArrays(1, &p->VAO.GetRawRef());

        VBSizeEvictedInMap += (unsigned)  p->VBAllocSize;
        p->destroy(pskipMesh, true);
    	return freedSize;
	}
    else
    {
        // Still in use, push it on the pending to delete list.
        // It should be valid for this to be called multiple times for a single mesh (for example, in a PendingFree situation).
        p->destroy(pskipMesh, false);
        CacheList.PushFront(MCL_PendingFree, p);
        return 0;
    }

}


// Allocates the buffer, while evicting LRU data.
bool MeshCache::allocBuffer(UPInt* poffset, MeshBuffer** pbuffer,
                            MeshBufferSet& mbs, UPInt size, bool waitForCache)
{
    SF_UNUSED(waitForCache);

    if (mbs.Alloc(size, pbuffer, poffset))
        return true;

    // If allocation failed... need to apply swapping or grow buffer.
    MeshCacheItem* pitems;
    bool needMoreSpace = true;

    // #1. Try and reclaim memory from items that have already been destroyed, but not freed.
    //     These cannot be reused, so it is best to evict their memory first, if possible.
    if (CacheList.EvictPendingFree(mbs.Allocator))
        needMoreSpace = false;

    // #2. Then, apply LRU (least recently used) swapping from data stale in
    //    earlier frames until the total size
    if (needMoreSpace && (getTotalSize() + MinSupportedGranularity) <= Params.MemLimit)
    {
        if (CacheList.EvictLRUTillLimit(MCL_LRUTail, mbs.GetAllocator(),
                                        size, Params.LRUTailSize))
        {
            needMoreSpace = false;
        }
        else
        {
            SF_DEBUG_ASSERT(size <= mbs.GetGranularity(), "Attempt to allocate mesh larger than MeshCache granularity.");
            if (size > mbs.GetGranularity())
                return false;

            UPInt allocSize = Alg::PMin(Params.MemLimit - getTotalSize(), mbs.GetGranularity());
            if (size <= allocSize)
            {
                MeshBuffer* pbuff = mbs.CreateBuffer(allocSize, MeshBuffer::AT_Chunk, 0, pHeap, pHal);
                if (pbuff)
                {
                    ChunkBuffers.PushBack(pbuff);
#ifdef SF_RENDER_LOG_CACHESIZE
                    LogDebugMessage(Log_Message, "Cache grew to %dK\n", getTotalSize() / 1024);
#endif
                    needMoreSpace = false;
                }
            }
        }
    }

    if (needMoreSpace && CacheList.EvictLRU(MCL_LRUTail, mbs.GetAllocator(), size))
        needMoreSpace = false;

    if (VBSizeEvictedInMap > Params.VBLockEvictSizeLimit)
        return false;

    // #3. Apply MRU (most recently used) swapping to the current frame content.
    // NOTE: MRU (GetFirst(), pNext iteration) gives
    //       2x improvement here with "Stars" test swapping.
    if (needMoreSpace)
    {
        MeshCacheListSet::ListSlot& prevFrameList = CacheList.GetSlot(MCL_PrevFrame);
        pitems = (MeshCacheItem*)prevFrameList.GetFirst();
        while(!prevFrameList.IsNull(pitems))
        {
            if (!pitems->IsPending(FenceType_Vertex))
            {
                if (Evict(pitems, &mbs.GetAllocator()) >= size)
                {
                    needMoreSpace = false;
                    break;
                }

                // Get the first item in the list, because and the head of the list will now be different, due to eviction.
                pitems = (MeshCacheItem*)prevFrameList.GetFirst();
            }
            else
            {
                pitems = (MeshCacheItem*)prevFrameList.GetNext(pitems);
            }
        }
    }
    
    // #4. If MRU swapping didn't work for ThisFrame items due to them still
    // being processed by the GPU and we are being asked to wait, wait
    // until fences are passed to evict items.
    if (needMoreSpace)
    {
        MeshCacheListSet::ListSlot& thisFrameList = CacheList.GetSlot(MCL_ThisFrame);
        pitems = (MeshCacheItem*)thisFrameList.GetFirst();
        while(waitForCache && !thisFrameList.IsNull(pitems))
        {
            if ( pitems->IsPending(FenceType_Vertex))
                pitems->WaitFence(FenceType_Vertex);
            if (Evict(pitems, &mbs.GetAllocator()) >= size)
            {
                needMoreSpace = false;
                break;
            }
            pitems = (MeshCacheItem*)thisFrameList.GetFirst();
        }
    }
    
    // #5. Extremely rare case, where the entire MeshCache was completely filled
    // on the previous frame, and the first mesh of the frame does not have sufficient room.
    // In this case, if waitForCache is true, empty the MeshCache and try again.
    if (needMoreSpace && waitForCache)
        CacheList.EvictPendingFree(mbs.Allocator, true);

    if (needMoreSpace)
        return false;

    // At this point we know we have a large enough block either due to
    // swapping or buffer growth, so allocation shouldn't fail.
    if (!mbs.Alloc(size, pbuffer, poffset))
    {
        SF_DEBUG_ASSERT(0, "Expected MeshCache to have enough memory to allocate mesh, but allocation failed.");
        return false;
    }

    return true;
}


MeshCache::AllocResult
MeshCache::AllocCacheItem(Render::MeshCacheItem** pdata,
                          MeshCacheItem::MeshType meshType,
                          MeshCacheItem::MeshBaseContent &mc,
                          UPInt vertexBufferSize,
                          unsigned vertexCount, unsigned indexCount,
                          bool waitForCache, const VertexFormat*)
{
    if (!AreBuffersMapped() && !LockBuffers())
        return Alloc_StateError;

    // Compute and allocate appropriate VB/IB space.
    UPInt       vbOffset = 0, ibOffset = 0;
    MeshBuffer  *pvb = 0, *pib = 0;
    MeshCache::AllocResult failType = Alloc_Fail;

    if (!allocBuffer(&vbOffset, &pvb, VertexBuffers,
                     vertexBufferSize, waitForCache))
    {
        if (!VertexBuffers.CheckAllocationSize(vertexBufferSize, "Vertex"))
            failType = Alloc_Fail_TooBig;
handle_alloc_fail:
        if (pvb) VertexBuffers.Free(vertexBufferSize, pvb, vbOffset);
        if (pib) IndexBuffers.Free(indexCount * sizeof(IndexType), pib, ibOffset);
        return failType;
    }
    if (!allocBuffer(&ibOffset, &pib, IndexBuffers,
                     indexCount * sizeof(IndexType), waitForCache))
    {
        if (!IndexBuffers.CheckAllocationSize(indexCount * sizeof(IndexType), "Index"))
            failType = Alloc_Fail_TooBig;
        goto handle_alloc_fail;
    }

    // Create new MeshCacheItem; add it to hash.
    *pdata = MeshCacheItem::Create(meshType, &CacheList, mc, pvb, pib,
                    (unsigned)vbOffset, vertexBufferSize, vertexCount,
                    (unsigned)ibOffset, indexCount * sizeof(IndexType), indexCount);

    if (!*pdata)
    {
        // Memory error; free buffers, skip mesh.
        SF_ASSERT(0);
        failType = Alloc_StateError;
        goto handle_alloc_fail;
    }
    return Alloc_Success;
}

void MeshCache::LockMeshCacheItem(Render::MeshCacheItem* pdataIn, UByte** pvertexDataStart, IndexType** pindexDataStart)
{
    GL::MeshCacheItem* pdata = reinterpret_cast<GL::MeshCacheItem*>(pdataIn);
    MeshBuffer* pvb = pdata->pVertexBuffer;
    MeshBuffer* pib = pdata->pIndexBuffer;
    UByte       *pvdata, *pidata;

    pvdata = pvb->Map(MappedBuffers, pdata->VBAllocOffset, pdata->VBAllocSize);
    pidata = pib->Map(MappedBuffers, pdata->IBAllocOffset, pdata->IBAllocSize);

    *pvertexDataStart = pvdata + pdata->VBAllocOffset;
    *pindexDataStart  = (IndexType*)(pidata + pdata->IBAllocOffset);
}

void MeshCache::GetStats(Stats* stats)
{
    *stats = Stats();
    unsigned memType = (BufferUpdate != BufferUpdate_ClientBuffers) ? MeshBuffer_GpuMem : 0;

    stats->TotalSize[memType + MeshBuffer_Vertex] = VertexBuffers.GetTotalSize();
    stats->UsedSize[memType + MeshBuffer_Vertex] = VertexBuffers.Allocator.GetFreeSize() << MeshCache_AllocatorUnitShift;

    stats->TotalSize[memType + MeshBuffer_Index] = IndexBuffers.GetTotalSize();
    stats->UsedSize[memType + MeshBuffer_Index] = IndexBuffers.Allocator.GetFreeSize() << MeshCache_AllocatorUnitShift;
}

}}}; // namespace Scaleform::Render::GL


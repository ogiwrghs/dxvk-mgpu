#pragma once

#include <memory>
#include "../util/util_time.h"

#include "../util/sync/sync_signal.h"

#include "d3d11_context.h"
#include "d3d11_state_object.h"
#include "d3d11_video.h"

namespace dxvk {
  
  class D3D11Buffer;
  class D3D11CommonTexture;

  class D3D11ImmediateContext : public D3D11CommonContext<D3D11ImmediateContext> {
    friend class D3D11CommonContext<D3D11ImmediateContext>;
    friend class D3D11SwapChain;
    friend class D3D11VideoContext;
    friend class D3D11DXGIKeyedMutex;
  public:
    
    D3D11ImmediateContext(
            D3D11Device*    pParent,
      const Rc<DxvkDevice>& Device);
    ~D3D11ImmediateContext();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject);

    HRESULT STDMETHODCALLTYPE GetData(
            ID3D11Asynchronous*         pAsync,
            void*                       pData,
            UINT                        DataSize,
            UINT                        GetDataFlags);
    
    void STDMETHODCALLTYPE Begin(
            ID3D11Asynchronous*         pAsync);
    
    void STDMETHODCALLTYPE End(
            ID3D11Asynchronous*         pAsync);
    
    void STDMETHODCALLTYPE Flush();
    
    void STDMETHODCALLTYPE Flush1(
            D3D11_CONTEXT_TYPE          ContextType,
            HANDLE                      hEvent);

    HRESULT STDMETHODCALLTYPE Signal(
            ID3D11Fence*                pFence,
            UINT64                      Value);
    
    HRESULT STDMETHODCALLTYPE Wait(
            ID3D11Fence*                pFence,
            UINT64                      Value);

    void STDMETHODCALLTYPE ExecuteCommandList(
            ID3D11CommandList*  pCommandList,
            BOOL                RestoreContextState);
    
    HRESULT STDMETHODCALLTYPE FinishCommandList(
            BOOL                RestoreDeferredContextState,
            ID3D11CommandList   **ppCommandList);
    
    HRESULT STDMETHODCALLTYPE Map(
            ID3D11Resource*             pResource,
            UINT                        Subresource,
            D3D11_MAP                   MapType,
            UINT                        MapFlags,
            D3D11_MAPPED_SUBRESOURCE*   pMappedResource);
    
    void STDMETHODCALLTYPE Unmap(
            ID3D11Resource*             pResource,
            UINT                        Subresource);
            
    void STDMETHODCALLTYPE SwapDeviceContextState(
            ID3DDeviceContextState*           pState,
            ID3DDeviceContextState**          ppPreviousState);

    void Acquire11on12Resource(
            ID3D11Resource*             pResource,
            VkImageLayout               SrcLayout);

    void Release11on12Resource(
            ID3D11Resource*             pResource,
            VkImageLayout               DstLayout);

    void SynchronizeCsThread(
            uint64_t                          SequenceNumber);

    D3D10Multithread& GetMultithread() {
        return m_multithread;
    }

    D3D10DeviceLock LockContext() {
      return m_multithread.AcquireLock();
    }

    void InjectCsChunk(
            DxvkCsQueue                 Queue,
            DxvkCsChunkRef&&            Chunk,
            bool                        Synchronize);

    template<typename Fn>
    void InjectCs(
            DxvkCsQueue                 Queue,
            Fn&&                        Command) {
      auto chunk = AllocCsChunk();
      chunk->push(std::move(Command));

      InjectCsChunk(Queue, std::move(chunk), false);
    }

  private:
    
    DxvkCsThread            m_csThread;
    std::unique_ptr<DxvkCsThread> m_workerCsThread;
    DxvkCsChunkRef          m_workerCsChunk;
    bool                    m_workerFlushDebug = false;
    uint32_t                m_workerFlushCalls = 0u;
    uint32_t                m_workerFlushEmpty = 0u;

    // Composite cost accumulators. Spot-sampling one composite in
    // sixty proved actively misleading: consecutive identical runs
    // reported 3us and 252us for the same workload, which invites
    // reading noise as a result. These aggregate every composite so
    // the numbers describe the run rather than one arbitrary frame.
    // Split into the two blocking phases because they fail
    // differently: sync waits for the worker CS thread to reach the
    // chunk, submit waits for GPU1 to finish executing it, and a
    // stall in one looks like speed in the other.
    uint32_t                m_compositeCount    = 0u;
    uint64_t                m_compositeSyncSum  = 0ull;
    uint64_t                m_compositeSyncMin  = ~0ull;
    uint64_t                m_compositeSyncMax  = 0ull;
    uint64_t                m_compositeSubSum   = 0ull;
    uint64_t                m_compositeSubMin   = ~0ull;
    uint64_t                m_compositeSubMax   = 0ull;

    void LogCompositeStats();
    Rc<DxvkBuffer>          m_workerStaging;
    Rc<DxvkBuffer>          m_primaryStaging;

    // Raw Vulkan objects backing the shared host staging allocation. These
    // deliberately bypass DxvkMemoryAllocator: one plain system-RAM block is
    // imported into both devices via VK_EXT_external_memory_host, so GPU1
    // writes and GPU0 reads the same physical pages and no CPU copy is needed.
    // Because they are outside DXVK's lifetime tracking, they must be freed
    // by hand in DestroySharedHostStaging().
    void*                   m_sharedHostMem   = nullptr;
    VkDeviceSize            m_sharedHostSize  = 0u;
    VkBuffer                m_workerVkBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory          m_workerVkMemory  = VK_NULL_HANDLE;
    VkBuffer                m_primaryVkBuffer = VK_NULL_HANDLE;
    VkDeviceMemory          m_primaryVkMemory = VK_NULL_HANDLE;

    VkDeviceSize            m_stagingSize = 0u;
    uint64_t                m_csSeqNum = 0ull;

    uint32_t                m_mappedImageCount = 0u;

    Rc<sync::CallbackFence> m_submissionFence;
    uint64_t                m_submissionId = 0ull;
    DxvkSubmitStatus        m_submitStatus;

    uint64_t                m_flushSeqNum = 0ull;
    GpuFlushTracker         m_flushTracker;

    Rc<sync::Fence>         m_stagingBufferFence;

    Rc<DxvkImage>           m_workerFrameImage;

    VkDeviceSize            m_discardMemoryCounter = 0u;
    VkDeviceSize            m_discardMemoryOnFlush = 0u;

    D3D10Multithread        m_multithread;
    D3D11VideoContext       m_videoContext;

    Com<D3D11DeviceContextState, false> m_stateObject;

    D3DDestructionNotifier  m_destructionNotifier;

    std::string             m_flushReason;

    bool                    m_hasPendingUnresolvedPass = false;

    HRESULT MapBuffer(
            D3D11Buffer*                pResource,
            D3D11_MAP                   MapType,
            UINT                        MapFlags,
            D3D11_MAPPED_SUBRESOURCE*   pMappedResource);
    
    HRESULT MapImage(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource,
            D3D11_MAP                   MapType,
            UINT                        MapFlags,
            D3D11_MAPPED_SUBRESOURCE*   pMappedResource);
    
    void UnmapImage(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource);
    
    void ReadbackImageBuffer(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource);

    void UpdateDirtyImageRegion(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource,
      const D3D11_COMMON_TEXTURE_REGION* pRegion);

    void UpdateMappedBuffer(
            D3D11Buffer*                pDstBuffer,
            UINT                        Offset,
            UINT                        Length,
      const void*                       pSrcData,
            UINT                        CopyFlags);

    void SynchronizeDevice();

    void EndFrame(
            Rc<DxvkLatencyTracker>      LatencyTracker);
    
    bool WaitForResource(
      const DxvkPagedResource&          Resource,
            uint64_t                    SequenceNumber,
            D3D11_MAP                   MapType,
            UINT                        MapFlags);
    
    void EmitCsChunk(DxvkCsChunkRef&& chunk);

    // Worker-side equivalent of EmitCs. The worker's first mirrored
    // operations (clear, framebuffer bind) each allocated and
    // dispatched their own single-command chunk, which is fine at two
    // calls per frame but would become one allocation and one dispatch
    // per state change once draws are mirrored -- an order of
    // magnitude more chunk traffic than the primary path, which
    // batches into m_csChunk and only dispatches when it fills.
    // This batches the same way. Note the worker chunk deliberately
    // does not participate in ConsiderFlush or sequence number
    // tracking, matching the standalone nature of the worker pipeline.
    template<typename Cmd>
    void EmitWorkerCs(Cmd&& command) {
      if (!m_workerCsThread)
        return;

      if (unlikely(!m_workerCsChunk->push(command))) {
        m_workerCsThread->dispatchChunk(std::move(m_workerCsChunk));
        m_workerCsChunk = m_parent->AllocCsChunk(DxvkCsChunkFlag::SingleUse);
        m_workerCsChunk->push(command);
      }
    }

    void FlushWorkerCsChunk();

    void EmitWorkerClearRenderTarget(
      const DxvkAttachment&             Attachment,
      const VkClearValue&               ClearValue);

    void EmitWorkerBindFramebuffer(
            DxvkRenderTargets&&         Targets);

    void CompositeWorkerOutput(
      const Rc<DxvkImage>&              DstImage);

    bool CreateSharedHostStaging(
            VkDeviceSize                Size);

    void DestroySharedHostStaging();

    void TrackTextureSequenceNumber(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource);

    void TrackBufferSequenceNumber(
            D3D11Buffer*                pResource);

    uint64_t GetCurrentSequenceNumber();

    uint64_t GetPendingCsChunks();

    void ApplyDirtyNullBindings();

    void ConsiderFlush(
            GpuFlushType                FlushType);

    void ExecuteFlush(
            GpuFlushType                FlushType,
            HANDLE                      hEvent,
            BOOL                        Synchronize);

    void ThrottleAllocation();

    void ThrottleDiscard(
            VkDeviceSize                Size);

    void NotifyRenderPassBoundary(
            bool                        IsMultisampled);

    void NotifyResolve();

    void RequestFlush(
            D3D11_CONTEXT_TYPE          ContextType,
            HANDLE                      hEvent);

    DxvkStagingBufferStats GetStagingMemoryStatistics();

    static GpuFlushType GetMaxFlushType(
            D3D11Device*    pParent,
      const Rc<DxvkDevice>& Device);

  };
  
}
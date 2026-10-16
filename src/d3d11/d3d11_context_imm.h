#pragma once

#include <memory>
#include <vector>
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
    friend class D3D11DeferredContext;
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

    // Worker-side counterpart, used by D3D11Initializer to route
    // resource uploads to the second device. Returns false if there is
    // no worker, so callers can skip building the chunk entirely.
    bool InjectWorkerCsChunk(
            DxvkCsChunkRef&&            Chunk);

    bool HasWorkerDevice() const {
      return m_workerCsThread != nullptr;
    }

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
    bool                    m_workerDrawDebug = false;
    bool                    m_workerMapDebug = false;
    bool                    m_noScissor = false;
    bool                    m_noCompletionWait = false;
    bool                    m_mirrorNoOverwrite = false;
    bool                    m_noShadow = false;
    bool                    m_workerSlices = true;
    bool                    m_fixedSplit = false;
    uint64_t                m_splitAccUs = 0u;
    uint32_t                m_splitAccN  = 0u;

    struct SliceMapped {
      D3D11Buffer*  buffer;
      const void*   src;
      void*         dst;
      VkDeviceSize  size;
    };

    std::vector<SliceMapped>       m_sliceMapped;
    bool                    m_noWorkerFlush = false;
    VkExtent2D              m_presentExtent = { 0u, 0u };
    bool                    m_noLayout = false;

    // Per-frame Map traffic, used to size the constant/dynamic buffer
    // mirroring problem before choosing how to solve it.
    uint32_t                m_mapDiscardCount = 0u;
    uint32_t                m_mapNoOverwriteCount = 0u;
    uint32_t                m_mapOtherCount = 0u;
    uint64_t                m_mapDiscardBytes = 0ull;
    uint64_t                m_mapNoOverwriteBytes = 0ull;
    uint64_t                m_mapOtherBytes = 0ull;
    uint32_t                m_mapFrames = 0u;

    void LogMapStats();
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

    // Per-composite samples, kept so the summary can report the shape
    // of the distribution rather than just its endpoints. min/avg/max
    // cannot distinguish a few warm-up outliers from a periodic spike
    // from a uniformly heavy tail, and those have completely different
    // causes. Capped so a long session cannot grow this without bound.
    static constexpr size_t MaxCompositeSamples = 100000u;

    std::vector<uint32_t>   m_compositeSyncLog;
    std::vector<uint32_t>   m_compositeSubLog;

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

    // Frame-behind composite. GPU1's half of frame N lands in
    // m_pipeBlocks[N % 3]; GPU0's own finished frame N is saved in
    // m_pipeImages[N % 2]; the frame presented at Present N is N-1,
    // reassembled from both. Three shared blocks so that GPU1's write
    // into a block trails GPU0's last read of it by two frames.
    struct SharedBlock {
      void*                 mem             = nullptr;
      VkDeviceSize          size            = 0u;
      VkBuffer              workerVkBuffer  = VK_NULL_HANDLE;
      VkDeviceMemory        workerVkMemory  = VK_NULL_HANDLE;
      VkBuffer              primaryVkBuffer = VK_NULL_HANDLE;
      VkDeviceMemory        primaryVkMemory = VK_NULL_HANDLE;
      Rc<DxvkBuffer>        worker;
      Rc<DxvkBuffer>        primary;
      uint32_t              splitY          = 0u;
      uint64_t              workerSeq       = 0u;
    };
    static constexpr uint32_t PipeBlocks = 3u;
    std::array<SharedBlock, PipeBlocks> m_pipeBlocks;
    std::array<Rc<DxvkImage>, 2>        m_pipeImages;
    VkExtent3D              m_pipeExtent   = { 0u, 0u, 0u };
    uint32_t                m_pipeFrame    = 0u;
    bool                    m_pipeValid    = false;
    uint64_t                m_lastWorkerSeq = 0u;
    uint64_t                m_csSeqNum = 0ull;

    uint32_t                m_mappedImageCount = 0u;

    // Buffer currently mapped for writing, remembered so Unmap can
    // push the same bytes to the worker device. Unmap does nothing for
    // buffers upstream -- the GPU reads host-visible memory directly --
    // so this is the only point at which the write is known complete.
    D3D11Buffer*            m_mappedWorkerBuffer = nullptr;
    void*                   m_mappedWorkerPtr = nullptr;
    VkDeviceSize            m_mappedWorkerSize = 0u;
    bool                    m_mappedWorkerNoOverwrite = false;

    std::vector<D3D11Buffer*>      m_shadowMapped;
    std::vector<D3D11ShadowRange>  m_shadowRanges;

    void FlushShadow(D3D11Buffer* pBuffer);

    Rc<DxvkBuffer>          m_cbReadBuffer = nullptr;
    VkDeviceSize            m_cbReadOffset = 0u;
    uint8_t                 m_cbReadExpect[16] = { };
    bool                    m_cbReadValid = false;
    bool                    m_cbReadEnabled = false;
    uint32_t                m_cbReadCount = 0u;
    Rc<DxvkBuffer>          m_cbReadPrimary = nullptr;
    VkDeviceSize            m_cbReadPrimaryOffset = 0u;
    bool                    m_cbPoisonEnabled = false;

    Rc<sync::CallbackFence> m_submissionFence;
    uint64_t                m_submissionId = 0ull;
    DxvkSubmitStatus        m_submitStatus;

    uint64_t                m_flushSeqNum = 0ull;
    GpuFlushTracker         m_flushTracker;

    Rc<sync::Fence>         m_stagingBufferFence;

    Rc<DxvkImage>           m_workerFrameImage;
    Rc<DxvkImage>           m_workerLargestImage;
    Rc<DxvkImageView>       m_workerFrameView;

    // Map/Unmap mirroring stages bytes for the worker's device-local
    // copy. This has to be its own staging buffer: DxvkStagingBuffer
    // is bound to the device that created it, so the primary's buffer
    // is not a valid source for a copy recorded on the worker context.
    std::unique_ptr<DxvkStagingBuffer> m_workerStagingBuffer;

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
        DispatchWorkerChunkTimed(std::move(m_workerCsChunk));
        m_workerCsChunk = m_parent->AllocCsChunk(DxvkCsChunkFlag::SingleUse);
        m_workerCsChunk->push(command);
      }
    }

    void FlushWorkerCsChunk();

    uint64_t DispatchWorkerChunkTimed(DxvkCsChunkRef&& chunk);



    void EmitWorkerClearRenderTarget(
      const DxvkAttachment&             Attachment,
      const VkClearValue&               ClearValue);

    void EmitWorkerBindFramebuffer(
            DxvkRenderTargets&&         Targets);

    bool NoScissor() const { return m_noScissor; }
    VkExtent2D PresentExtent() const { return m_presentExtent; }

    bool NoLayout() const { return m_noLayout; }

    void DumpWorkerRenderTargets();

    void DumpWorkerBuffers();

    void CompositeWorkerOutput(
      const Rc<DxvkImage>&              DstImage,
      const Rc<DxvkImage>&              SrcImage);

    void CompositeWorkerOutputPipelined(
      const Rc<DxvkImage>&        DstImage,
      const Rc<DxvkImage>&        SrcImage);

    void MgpuFrameSpanEnd();
    void MgpuFrameSpanStart();

    struct MgpuSpanSlot {
      Rc<DxvkQuery> s0, e0, s1, e1;
      Rc<DxvkQuery> c0a, c0b, c0c, c0d, c1a, c1b;
      bool          valid     = false;
      bool          compValid = false;
    };

    struct MgpuSpanCalib {
      uint64_t gpuTicks = 0u;
      double   qpcNs    = 0.0;
      double   period   = 1.0;
    };

    MgpuSpanSlot      m_spanRing[8];
    MgpuSpanCalib     m_spanCalib[2];
    Rc<DxvkDevice>    m_spanDev[2];
    uint64_t          m_spanFrame     = 0u;
    bool              m_spanInit      = false;
    bool              m_spanOk        = false;
    bool              m_spanHaveStart = false;
    double            m_spanQpcNs     = 0.0;
    double            m_spanSum[4]    = { };
    double            m_spanComp[7]   = { };
    uint32_t          m_spanTailCount = 0u;
    uint32_t          m_spanCompCount = 0u;
    double            m_spanPeriodSum = 0.0;
    uint32_t          m_spanPeriodCount = 0u;
    double            m_spanPrevE0    = 0.0;
    uint64_t          m_spanPrevFrame = 0u;
    uint64_t          m_spanMaxDev    = 0u;
    double            m_spanDevSum    = 0.0;
    double            m_spanCallNs    = 0.0;
    uint32_t          m_spanCalls     = 0u;
    uint32_t          m_spanCount     = 0u;
    uint32_t          m_spanSkipped   = 0u;

    bool MgpuSpanCalibrate(uint32_t Index);
    double MgpuSpanToNs(uint32_t Index, uint64_t Ticks) const;

    bool StealSharedBlock(SharedBlock& Block);
    void ReleaseSharedBlock(SharedBlock& Block);
    void ReleasePipeline();
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
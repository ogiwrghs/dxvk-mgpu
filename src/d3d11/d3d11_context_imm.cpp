#include "d3d11_cmdlist.h"
#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_fence.h"
#include "d3d11_texture.h"

#include "../util/util_env.h"
#include "../util/util_win32_compat.h"

constexpr static uint32_t MinFlushIntervalUs = 750;
constexpr static uint32_t IncFlushIntervalUs = 250;
constexpr static uint32_t MaxPendingSubmits  = 6;

namespace dxvk {
  
  D3D11ImmediateContext::D3D11ImmediateContext(
          D3D11Device*    pParent,
    const Rc<DxvkDevice>& Device)
  : D3D11CommonContext<D3D11ImmediateContext>(pParent, Device, 0, DxvkCsChunkFlag::SingleUse),
    m_csThread(Device, Device->createContext()),
    m_submissionFence(new sync::CallbackFence()),
    m_flushTracker(GetMaxFlushType(pParent, Device)),
    m_stagingBufferFence(new sync::Fence(0)),
    m_multithread(this, false, pParent->GetOptions()->enableContextLock),
    m_videoContext(this, Device),
    m_destructionNotifier(this) {
    EmitCs([
      cDevice                 = m_device,
      cBarrierControlFlags    = pParent->GetOptionsBarrierControlFlags()
    ] (DxvkContext* ctx) {
      ctx->beginRecording(cDevice->createCommandList());

      ctx->setBarrierControl(cBarrierControlFlags);
    });

    // Stall here so that external submissions to the
    // CS thread can actually access the command list
    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);

    // Set up a second, independent CS thread + context on the
    // worker GPU, if one exists. This is a standalone pipeline:
    // it does not participate in sequence number tracking,
    // flush tracking, or any of the machinery above.
    
    Rc<DxvkDevice> workerDevice = pParent->GetWorkerDXVKDevice();
    if (workerDevice != nullptr) {
      m_workerCsThread = std::make_unique<DxvkCsThread>(
        workerDevice, workerDevice->createContext());

      // Prime the worker context exactly as the primary one is primed
      // above. Without a command list attached, the first operation
      // that actually needs to record a barrier (e.g. flushing a
      // deferred clear) dereferences a null m_cmd and crashes.
      DxvkCsChunkRef initChunk = pParent->AllocCsChunk(DxvkCsChunkFlag::SingleUse);

      initChunk->push([
        cWorkerDevice        = workerDevice,
        cBarrierControlFlags = pParent->GetOptionsBarrierControlFlags()
      ] (DxvkContext* ctx) {
        ctx->beginRecording(cWorkerDevice->createCommandList());
        ctx->setBarrierControl(cBarrierControlFlags);
      });

      uint64_t initSeq = m_workerCsThread->dispatchChunk(std::move(initChunk));

      // Stall so the command list is live before any real work lands.
      m_workerCsThread->synchronize(initSeq);
      // Batched chunk for mirrored work, allocated only when a worker
      // exists so that EmitWorkerCs's null-thread guard is the single
      // condition protecting it.
      m_workerCsChunk = pParent->AllocCsChunk(DxvkCsChunkFlag::SingleUse);
      m_workerStagingBuffer = std::make_unique<DxvkStagingBuffer>(
        workerDevice, 32ull << 20);

      m_workerFlushDebug = env::getEnvVar("DXVK_MGPU_DEBUG_FLUSH") == "1";
      m_workerDrawDebug  = env::getEnvVar("DXVK_MGPU_DEBUG_DRAW") == "1";
      m_workerMapDebug   = env::getEnvVar("DXVK_MGPU_DEBUG_MAP") == "1";
      m_noScissor        = env::getEnvVar("DXVK_MGPU_NO_SCISSOR") == "1";
      m_noLayout         = env::getEnvVar("DXVK_MGPU_NO_LAYOUT") == "1";
      m_noCompletionWait = env::getEnvVar("DXVK_MGPU_NO_COMPLETION_WAIT") == "1";
      m_mirrorNoOverwrite = env::getEnvVar("DXVK_MGPU_NO_MIRROR_NOOVERWRITE") != "1";
      m_noWorkerFlush     = env::getEnvVar("DXVK_MGPU_NO_WORKER_FLUSH") == "1";
      m_cbReadEnabled    = env::getEnvVar("DXVK_MGPU_TEST_CBREAD") == "1";
      m_cbPoisonEnabled  = env::getEnvVar("DXVK_MGPU_TEST_CBPOISON") == "1";
      Logger::info("D3D11ImmediateContext: Created worker CS thread and context");
    }

    // Deliberately after the worker context exists. ResetCommandListState
    // mirrors the default render states onto the worker now, but a mirror
    // dispatched while m_workerCsThread is still null is dropped by
    // EmitWorkerCs's guard -- which is exactly what left the worker's
    // pipeline state zeroed, colorWriteMask included.
    ClearState();
  }
  
  D3D11ImmediateContext::~D3D11ImmediateContext() {
    // Avoids hanging when in this state, see comment
    // in DxvkDevice::~DxvkDevice.
    if (this_thread::isInModuleDetachment())
      return;

    ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, true);
    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    SynchronizeDevice();
    LogCompositeStats();
    LogMapStats();
    // Dispatch anything still batched before the worker device is
    // idled and its buffers torn down.
    FlushWorkerCsChunk();
    DestroySharedHostStaging();
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11ImmediateContext::QueryInterface(REFIID riid, void** ppvObject) {
    if (riid == __uuidof(ID3D10Multithread)) {
      *ppvObject = ref(&m_multithread);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11VideoContext)) {
      *ppvObject = ref(&m_videoContext);
      return S_OK;
    }

    if (riid == __uuidof(ID3DDestructionNotifier)) {
      *ppvObject = ref(&m_destructionNotifier);
      return S_OK;
    }

    return D3D11CommonContext<D3D11ImmediateContext>::QueryInterface(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D11ImmediateContext::GetData(
          ID3D11Asynchronous*               pAsync,
          void*                             pData,
          UINT                              DataSize,
          UINT                              GetDataFlags) {
    if (!pAsync || (DataSize && !pData))
      return E_INVALIDARG;
    
    // Check whether the data size is actually correct
    if (DataSize && DataSize != pAsync->GetDataSize())
      return E_INVALIDARG;
    
    // Passing a non-null pData is actually allowed if
    // DataSize is 0, but we should ignore that pointer
    pData = DataSize ? pData : nullptr;

    // Get query status directly from the query object
    auto query = static_cast<D3D11Query*>(pAsync);
    HRESULT hr = query->GetData(pData, GetDataFlags);
    
    // If we're likely going to spin on the asynchronous object,
    // flush the context so that we're keeping the GPU busy.
    if (hr == S_FALSE) {
      // Don't mark the event query as stalling if the app does
      // not intend to spin on it. This reduces flushes on End.
      if (!(GetDataFlags & D3D11_ASYNC_GETDATA_DONOTFLUSH))
        query->NotifyStall();

      // Ignore the DONOTFLUSH flag here as some games will spin
      // on queries without ever flushing the context otherwise.
      D3D10DeviceLock lock = LockContext();

      if (unlikely(m_device->debugFlags().test(DxvkDebugFlag::Capture)))
        m_flushReason = "Query read-back";

      ConsiderFlush(GpuFlushType::ImplicitSynchronization);
    }
    
    return hr;
  }
  
  
  void STDMETHODCALLTYPE D3D11ImmediateContext::Begin(ID3D11Asynchronous* pAsync) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(!pAsync))
      return;
    
    auto query = static_cast<D3D11Query*>(pAsync);

    if (unlikely(!query->DoBegin()))
      return;

    EmitCs([cQuery = Com<D3D11Query, false>(query)]
    (DxvkContext* ctx) {
      cQuery->Begin(ctx);
    });
  }


  void STDMETHODCALLTYPE D3D11ImmediateContext::End(ID3D11Asynchronous* pAsync) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(!pAsync))
      return;
    
    auto query = static_cast<D3D11Query*>(pAsync);

    if (unlikely(!query->DoEnd())) {
      EmitCs([cQuery = Com<D3D11Query, false>(query)]
      (DxvkContext* ctx) {
        cQuery->Begin(ctx);
      });
    }

    EmitCs([cQuery = Com<D3D11Query, false>(query)]
    (DxvkContext* ctx) {
      cQuery->End(ctx);
    });

    if (unlikely(query->TrackStalls())) {
      query->NotifyEnd();

      if (query->IsStalling())
        ExecuteFlush(GpuFlushType::ImplicitSynchronization, nullptr, false);
      else if (query->IsEvent())
        ConsiderFlush(GpuFlushType::ImplicitStrongHint);
    }
  }


  void STDMETHODCALLTYPE D3D11ImmediateContext::Flush() {
    RequestFlush(D3D11_CONTEXT_TYPE_ALL, nullptr);
  }


  void STDMETHODCALLTYPE D3D11ImmediateContext::Flush1(
          D3D11_CONTEXT_TYPE          ContextType,
          HANDLE                      hEvent) {
    RequestFlush(ContextType, hEvent);
  }


  HRESULT STDMETHODCALLTYPE D3D11ImmediateContext::Signal(
          ID3D11Fence*                pFence,
          UINT64                      Value) {
    D3D10DeviceLock lock = LockContext();
    auto fence = static_cast<D3D11Fence*>(pFence);

    if (!fence)
      return E_INVALIDARG;

    EmitCs([
      cFence = fence->GetFence(),
      cValue = Value
    ] (DxvkContext* ctx) {
      ctx->signalFence(cFence, cValue);
    });

    if (unlikely(m_device->debugFlags().test(DxvkDebugFlag::Capture)))
      m_flushReason = "Fence signal";

    ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, true);
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11ImmediateContext::Wait(
          ID3D11Fence*                pFence,
          UINT64                      Value) {
    D3D10DeviceLock lock = LockContext();
    auto fence = static_cast<D3D11Fence*>(pFence);

    if (!fence)
      return E_INVALIDARG;

    if (unlikely(m_device->debugFlags().test(DxvkDebugFlag::Capture)))
      m_flushReason = "Fence wait";

    ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, true);

    EmitCs([
      cFence = fence->GetFence(),
      cValue = Value
    ] (DxvkContext* ctx) {
      ctx->waitFence(cFence, cValue);
    });

    return S_OK;
  }


  void STDMETHODCALLTYPE D3D11ImmediateContext::ExecuteCommandList(
          ID3D11CommandList*  pCommandList,
          BOOL                RestoreContextState) {
    D3D10DeviceLock lock = LockContext();

    auto commandList = static_cast<D3D11CommandList*>(pCommandList);

    // Reset dirty binding tracking before submitting any CS chunks.
    // This is needed so that any submission that might occur during
    // this call does not disrupt bindings set by the deferred context.
    ResetDirtyTracking();

    // Clear state so that the command list can't observe any
    // current context state. The command list itself will clean
    // up after execution to ensure that no state changes done
    // by the command list are visible to the immediate context.
    ResetCommandListState();

    // Flush any outstanding commands so that
    // we don't mess up the execution order
    FlushCsChunk();
    
    // As an optimization, flush everything if the
    // number of pending draw calls is high enough.
    ConsiderFlush(GpuFlushType::ImplicitWeakHint);

    // Dispatch command list to the CS thread
    commandList->EmitToCsThread([this] (DxvkCsChunkRef&& chunk, uint64_t cost, GpuFlushType flushType) {
      EmitCsChunk(std::move(chunk));

      // Return the sequence number from before the flush since
      // that is actually going to be needed for resource tracking
      uint64_t csSeqNum = m_csSeqNum;

      // Consider a flush after every chunk in case the app
      // submits a very large command list or the GPU is idle
      AddCost(cost);
      ConsiderFlush(flushType);
      return csSeqNum;
    });

    // Replay the worker side of the command list. The immediate
    // context's own worker chunk is flushed first so the deferred
    // work lands after whatever was mirrored before this call.
    if (m_workerCsThread) {
      FlushWorkerCsChunk();
      static std::atomic<uint64_t> s_lists = { 0u };
      static std::atomic<uint64_t> s_chunks = { 0u };
      uint64_t lists = ++s_lists;
      commandList->EmitWorkerToCsThread([this] (DxvkCsChunkRef&& chunk) {
        ++s_chunks;
        InjectWorkerCsChunk(std::move(chunk));
      });
      if (lists <= 2u || (lists % 1000u) == 0u) {
        Logger::warn(str::format("DEFERREDMIRROR lists=", lists,
          " workerChunks=", s_chunks.load()));
      }
    }

    // Restore the immediate context's state
    if (RestoreContextState)
      RestoreCommandListState();
    else
      ResetContextState();
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11ImmediateContext::FinishCommandList(
          BOOL                RestoreDeferredContextState,
          ID3D11CommandList   **ppCommandList) {
    InitReturnPtr(ppCommandList);
    
    Logger::err("D3D11: FinishCommandList called on immediate context");
    return DXGI_ERROR_INVALID_CALL;
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11ImmediateContext::Map(
          ID3D11Resource*             pResource,
          UINT                        Subresource,
          D3D11_MAP                   MapType,
          UINT                        MapFlags,
          D3D11_MAPPED_SUBRESOURCE*   pMappedResource) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(!pResource))
      return E_INVALIDARG;
    
    D3D11_RESOURCE_DIMENSION resourceDim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    pResource->GetType(&resourceDim);

    if (likely(resourceDim == D3D11_RESOURCE_DIMENSION_BUFFER)) {
      return MapBuffer(
        static_cast<D3D11Buffer*>(pResource),
        MapType, MapFlags, pMappedResource);
    } else {
      return MapImage(GetCommonTexture(pResource),
        Subresource, MapType, MapFlags, pMappedResource);
    }
  }
  
  
  void STDMETHODCALLTYPE D3D11ImmediateContext::Unmap(
          ID3D11Resource*             pResource,
          UINT                        Subresource) {
    // Since it is very uncommon for images to be mapped compared
    // to buffers, we count the currently mapped images in order
    // to avoid a virtual method call in the common case.
    if (unlikely(m_mappedImageCount > 0)) {
      D3D11_RESOURCE_DIMENSION resourceDim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
      pResource->GetType(&resourceDim);

      if (resourceDim != D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D10DeviceLock lock = LockContext();
        UnmapImage(GetCommonTexture(pResource), Subresource);
      }
    }

    // Push the just-written contents to the worker's copy of the
    // buffer. Upstream Unmap is a no-op for buffers because the GPU
    // reads the host-visible allocation directly; the worker has its
    // own device-local buffer, so the bytes have to be staged and
    // uploaded. This is the straightforward version, kept deliberately
    // simple to measure before deciding whether shared host memory is
    // worth the complexity.
    if (unlikely(m_mappedWorkerBuffer != nullptr
              && reinterpret_cast<ID3D11Resource*>(m_mappedWorkerBuffer) == pResource)) {
      D3D10DeviceLock lock = LockContext();

      Rc<DxvkBuffer> workerBuffer = m_mappedWorkerBuffer->GetWorkerBuffer();

      if (workerBuffer != nullptr) {
        DxvkBufferSlice staging = m_workerStagingBuffer->alloc(m_mappedWorkerSize);
        auto tc0 = dxvk::high_resolution_clock::now();
        std::memcpy(staging.mapPtr(0), m_mappedWorkerPtr, m_mappedWorkerSize);
        auto tc1 = dxvk::high_resolution_clock::now();
        if (m_mappedWorkerSize >= 65536u) {
          static uint32_t s_cpyLog = 0;
          if ((s_cpyLog++ % 200u) == 0u) {
            Logger::warn(str::format("MAPMIRROR memcpy ", m_mappedWorkerSize, " bytes in ",
              std::chrono::duration_cast<std::chrono::microseconds>(tc1 - tc0).count(), " us"));
          }
        }

        {
          static std::atomic<uint64_t> s_discard = { 0u };
          static std::atomic<uint64_t> s_noOvw   = { 0u };
          uint64_t n = m_mappedWorkerNoOverwrite ? ++s_noOvw : ++s_discard;
          uint64_t period = m_mappedWorkerNoOverwrite ? 500u : 100000u;
          if (n <= 2u || (n % period) == 0u) {
            Logger::warn(str::format("MAPMIRROR ",
              m_mappedWorkerNoOverwrite ? "nooverwrite=" : "discard=", n,
              " bytes=", m_mappedWorkerSize));
          }
        }
        EmitWorkerCs([
          cBuffer  = std::move(workerBuffer),
          cStaging = std::move(staging),
          cSize    = m_mappedWorkerSize,
          cCopy    = m_mappedWorkerNoOverwrite
        ] (DxvkContext* ctx) {
          if (cCopy) {
            ctx->copyBuffer(cBuffer, 0u,
              cStaging.buffer(), cStaging.offset(), cSize);
          } else {
            ctx->uploadBuffer(cBuffer, 0u,
              cStaging.buffer(), cStaging.offset(), cSize);
          }
        });
      }
      m_mappedWorkerBuffer      = nullptr;
      m_mappedWorkerPtr         = nullptr;
      m_mappedWorkerSize        = 0u;
      m_mappedWorkerNoOverwrite = false;
    }
  }


  HRESULT D3D11ImmediateContext::MapBuffer(
          D3D11Buffer*                pResource,
          D3D11_MAP                   MapType,
          UINT                        MapFlags,
          D3D11_MAPPED_SUBRESOURCE*   pMappedResource) {
    if (unlikely(!pMappedResource))
      return E_INVALIDARG;

    if (unlikely(pResource->GetMapMode() == D3D11_COMMON_BUFFER_MAP_MODE_NONE)) {
      Logger::err("D3D11: Cannot map a device-local buffer");
      pMappedResource->pData = nullptr;
      return E_INVALIDARG;
    }

    VkDeviceSize bufferSize = pResource->Desc()->ByteWidth;

    if (unlikely(m_workerMapDebug)) {
      switch (MapType) {
        case D3D11_MAP_WRITE_DISCARD:
          m_mapDiscardCount++;
          m_mapDiscardBytes += bufferSize;
          break;

        case D3D11_MAP_WRITE_NO_OVERWRITE:
          m_mapNoOverwriteCount++;
          m_mapNoOverwriteBytes += bufferSize;
          break;

        default:
          m_mapOtherCount++;
          m_mapOtherBytes += bufferSize;
          break;
      }
    }

    if (likely(MapType == D3D11_MAP_WRITE_DISCARD)) {
      // Allocate a new backing slice for the buffer and set
      // it as the 'new' mapped slice. This assumes that the
      // only way to invalidate a buffer is by mapping it.
      auto bufferSlice = pResource->DiscardSlice(&m_allocationCache);
      pMappedResource->pData      = bufferSlice->mapPtr();
      pMappedResource->RowPitch   = bufferSize;
      pMappedResource->DepthPitch = bufferSize;

      if (m_workerCsThread && pResource->GetWorkerBuffer() != nullptr) {
        if (m_mappedWorkerBuffer != nullptr) {
          static std::atomic<uint64_t> s_clobber = { 0u };
          uint64_t n = ++s_clobber;
          if (n <= 4u || (n % 10000u) == 0u)
            Logger::warn(str::format("MAPMIRROR clobbered=", n));
        }
        m_mappedWorkerBuffer      = pResource;
        m_mappedWorkerPtr         = bufferSlice->mapPtr();
        m_mappedWorkerSize        = bufferSize;
        m_mappedWorkerNoOverwrite = false;
      }
      
      EmitCs([
        cBuffer      = pResource->GetBuffer(),
        cBufferSlice = std::move(bufferSlice)
      ] (DxvkContext* ctx) mutable {
        ctx->invalidateBuffer(cBuffer, std::move(cBufferSlice));
      });

      // Ignore small buffers here. These are often updated per
      // draw and won't contribute much to memory waste anyway.
      if (unlikely(bufferSize > DxvkPageAllocator::PageSize))
        ThrottleDiscard(bufferSize);

      return S_OK;
    } else if (likely(MapType == D3D11_MAP_WRITE_NO_OVERWRITE)) {
      // Put this on a fast path without any extra checks since it's
      // a somewhat desired method to partially update large buffers
      pMappedResource->pData      = pResource->GetMapPtr();
      pMappedResource->RowPitch   = bufferSize;
      pMappedResource->DepthPitch = bufferSize;
      if (m_workerCsThread && m_mirrorNoOverwrite && pResource->GetWorkerBuffer() != nullptr) {
        if (m_mappedWorkerBuffer != nullptr) {
          static std::atomic<uint64_t> s_clobber = { 0u };
          uint64_t n = ++s_clobber;
          if (n <= 4u || (n % 10000u) == 0u)
            Logger::warn(str::format("MAPMIRROR clobbered=", n));
        }
        m_mappedWorkerBuffer      = pResource;
        m_mappedWorkerPtr         = pResource->GetMapPtr();
        m_mappedWorkerSize        = bufferSize;
        m_mappedWorkerNoOverwrite = true;
      }
      return S_OK;
    } else {
      // Quantum Break likes using MAP_WRITE on resources which would force
      // us to synchronize with the GPU multiple times per frame. In those
      // situations, if there are no pending GPU writes to the resource, we
      // can promote it to MAP_WRITE_DISCARD, but preserve the data by doing
      // a CPU copy from the previous buffer slice, to avoid the sync point.
      bool doInvalidatePreserve = false;

      auto buffer = pResource->GetBuffer();
      auto sequenceNumber = pResource->GetSequenceNumber();

      if (MapType != D3D11_MAP_READ && !MapFlags && bufferSize <= D3D11Initializer::MaxMemoryPerSubmission) {
        SynchronizeCsThread(sequenceNumber);

        bool hasWoAccess = buffer->isInUse(DxvkAccess::Write);
        bool hasRwAccess = buffer->isInUse(DxvkAccess::Read);

        if (hasRwAccess && !hasWoAccess) {
          // Uncached reads can be so slow that a GPU sync may actually be faster
          doInvalidatePreserve = buffer->memFlags() & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        }
      }

      if (doInvalidatePreserve) {
        auto srcPtr = pResource->GetMapPtr();

        auto dstSlice = pResource->DiscardSlice(nullptr);
        auto dstPtr = dstSlice->mapPtr();

        EmitCs([
          cBuffer      = std::move(buffer),
          cBufferSlice = std::move(dstSlice)
        ] (DxvkContext* ctx) mutable {
          ctx->invalidateBuffer(cBuffer, std::move(cBufferSlice));
        });

        std::memcpy(dstPtr, srcPtr, bufferSize);
        pMappedResource->pData      = dstPtr;
        pMappedResource->RowPitch   = bufferSize;
        pMappedResource->DepthPitch = bufferSize;

        ThrottleDiscard(bufferSize);
        return S_OK;
      } else {
        if (!WaitForResource(*buffer, sequenceNumber, MapType, MapFlags)) {
          pMappedResource->pData = nullptr;
          return DXGI_ERROR_WAS_STILL_DRAWING;
        }

        pMappedResource->pData      = pResource->GetMapPtr();
        pMappedResource->RowPitch   = bufferSize;
        pMappedResource->DepthPitch = bufferSize;
        return S_OK;
      }
    }
  }
  
  
  HRESULT D3D11ImmediateContext::MapImage(
          D3D11CommonTexture*         pResource,
          UINT                        Subresource,
          D3D11_MAP                   MapType,
          UINT                        MapFlags,
          D3D11_MAPPED_SUBRESOURCE*   pMappedResource) {
    auto mapMode = pResource->GetMapMode();

    // Counterpart to UPDATETEX. This path has no worker mirror either, so
    // a texture written through Map/Unmap at runtime exists on GPU1 but
    // holds stale contents from load time. Counting write maps separately
    // from reads, since only writes leave the two devices out of step.
    {
      static std::atomic<uint64_t> s_mapImage = { 0u };

      bool isWrite = MapType == D3D11_MAP_WRITE
                  || MapType == D3D11_MAP_WRITE_DISCARD
                  || MapType == D3D11_MAP_WRITE_NO_OVERWRITE
                  || MapType == D3D11_MAP_READ_WRITE;

      if (isWrite) {
        uint64_t n = ++s_mapImage;

        if (n <= 4u || (n % 2000u) == 0u)
          Logger::warn(str::format("MAPIMAGEWRITE count=", n));
      }
    }

    if (pMappedResource)
      pMappedResource->pData = nullptr;

    if (unlikely(Subresource >= pResource->CountSubresources()))
      return E_INVALIDARG;

    switch (MapType) {
      case D3D11_MAP_READ: {
        if (!(pResource->Desc()->CPUAccessFlags & D3D11_CPU_ACCESS_READ))
          return E_INVALIDARG;
      } break;

      case D3D11_MAP_READ_WRITE: {
        if (!(pResource->Desc()->CPUAccessFlags & D3D11_CPU_ACCESS_READ)
         || !(pResource->Desc()->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE))
          return E_INVALIDARG;
      } break;

      case D3D11_MAP_WRITE: {
        if (!(pResource->Desc()->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)
         || (pResource->Desc()->Usage == D3D11_USAGE_DYNAMIC))
          return E_INVALIDARG;
      } break;

      case D3D11_MAP_WRITE_DISCARD: {
        if (!(pResource->Desc()->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)
         || pResource->Desc()->Usage != D3D11_USAGE_DYNAMIC)
          return E_INVALIDARG;
      } break;

      case D3D11_MAP_WRITE_NO_OVERWRITE: {
        // NO_OVERWRITE is explcitly banned for dynamic images
        if (!(pResource->Desc()->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)
         || (pResource->Desc()->Usage != D3D11_USAGE_DEFAULT))
          return E_INVALIDARG;
      } break;
    }

    if (likely(pMappedResource != nullptr)) {
      // Resources with an unknown memory layout cannot return a pointer
      if (pResource->Desc()->Usage         == D3D11_USAGE_DEFAULT
       && pResource->Desc()->TextureLayout == D3D11_TEXTURE_LAYOUT_UNDEFINED)
        return E_INVALIDARG;
    } else {
      if (pResource->Desc()->Usage != D3D11_USAGE_DEFAULT)
        return E_INVALIDARG;
    }

    VkFormat packedFormat = m_parent->LookupPackedFormat(
      pResource->Desc()->Format, pResource->GetFormatMode()).Format;
    
    uint64_t sequenceNumber = pResource->GetSequenceNumber(Subresource);

    auto formatInfo = lookupFormatInfo(packedFormat);
    auto layout = pResource->GetSubresourceLayout(formatInfo->aspectMask, Subresource);

    if (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_DIRECT) {
      Rc<DxvkImage> mappedImage = pResource->GetImage();

      if (MapType == D3D11_MAP_WRITE_DISCARD) {
        EmitCs([
          cImage = std::move(mappedImage),
          cStorage = pResource->DiscardStorage()
        ] (DxvkContext* ctx) {
          ctx->invalidateImage(cImage, Rc<DxvkResourceAllocation>(cStorage), VK_IMAGE_LAYOUT_PREINITIALIZED);
        });

        ThrottleDiscard(layout.Size);
      } else if (MapType != D3D11_MAP_WRITE_NO_OVERWRITE) {
        if (!WaitForResource(*mappedImage, sequenceNumber, MapType, MapFlags))
          return DXGI_ERROR_WAS_STILL_DRAWING;
      }
    } else if (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_DYNAMIC) {
      // Nothing else to really do here, NotifyMap will ensure that we
      // actually get a staging buffer that isn't currently in use.
      ThrottleDiscard(layout.Size);
    } else {
      Rc<DxvkBuffer> mappedBuffer = pResource->GetMappedBuffer(Subresource);

      constexpr uint32_t DoInvalidate = (1u << 0);
      constexpr uint32_t DoPreserve   = (1u << 1);
      constexpr uint32_t DoWait       = (1u << 2);
      uint32_t doFlags;

      if (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_BUFFER) {
        // If the image can be written by the GPU, we need to update the
        // mapped staging buffer to reflect the current image contents.
        if (pResource->Desc()->Usage == D3D11_USAGE_DEFAULT) {
          bool needsReadback = !pResource->NeedsDirtyRegionTracking();

          needsReadback |= MapType == D3D11_MAP_READ
                        || MapType == D3D11_MAP_READ_WRITE;

          if (needsReadback)
            ReadbackImageBuffer(pResource, Subresource);
        }
      }

      if (MapType == D3D11_MAP_READ) {
        // Reads will not change the image content, so we only need
        // to wait for the GPU to finish writing to the mapped buffer.
        doFlags = DoWait;
      } else if (MapType == D3D11_MAP_WRITE_DISCARD) {
        doFlags = DoInvalidate;

        // If we know for sure that the mapped buffer is currently not
        // in use by the GPU, we don't have to allocate a new slice.
        if (m_csThread.lastSequenceNumber() >= sequenceNumber && !mappedBuffer->isInUse(DxvkAccess::Read))
          doFlags = 0;
      } else if (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_STAGING && (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT)) {
        // Always respect DO_NOT_WAIT for mapped staging images
        doFlags = DoWait;
      } else if (MapType != D3D11_MAP_WRITE_NO_OVERWRITE || mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_BUFFER) {
        // Need to synchronize thread to determine pending GPU accesses
        SynchronizeCsThread(sequenceNumber);

        // Don't implicitly discard large very large resources
        // since that might lead to memory issues.
        VkDeviceSize bufferSize = mappedBuffer->info().size;

        if (bufferSize > D3D11Initializer::MaxMemoryPerSubmission) {
          // Don't check access flags, WaitForResource will return
          // early anyway if the resource is currently in use
          doFlags = DoWait;
        } else if (mappedBuffer->isInUse(DxvkAccess::Write)) {
          // There are pending GPU writes, need to wait for those
          doFlags = DoWait;
        } else if (mappedBuffer->isInUse(DxvkAccess::Read)) {
          // All pending GPU accesses are reads, so the buffer data
          // is still current, and we can prevent GPU synchronization
          // by creating a new slice with an exact copy of the data.
          doFlags = DoInvalidate | DoPreserve;
        } else {
          // There are no pending accesses, so we don't need to wait
          doFlags = 0;
        }
      } else {
        // No need to synchronize staging resources with NO_OVERWRITE
        // since the buffer will be used directly.
        doFlags = 0;
      }

      if (doFlags & DoInvalidate) {
        VkDeviceSize bufferSize = mappedBuffer->info().size;

        auto srcSlice = pResource->GetMappedSlice(Subresource);
        auto dstSlice = pResource->DiscardSlice(Subresource);

        auto srcPtr = srcSlice->mapPtr();
        auto dstPtr = dstSlice->mapPtr();

        EmitCs([
          cImageBuffer      = std::move(mappedBuffer),
          cImageBufferSlice = std::move(dstSlice)
        ] (DxvkContext* ctx) mutable {
          ctx->invalidateBuffer(cImageBuffer, std::move(cImageBufferSlice));
        });

        if (doFlags & DoPreserve)
          std::memcpy(dstPtr, srcPtr, bufferSize);

        ThrottleDiscard(bufferSize);
      } else {
        if (doFlags & DoWait) {
          // We cannot respect DO_NOT_WAIT for buffer-mapped resources since
          // our internal copies need to be transparent to the application.
          if (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_BUFFER)
            MapFlags &= ~D3D11_MAP_FLAG_DO_NOT_WAIT;

          // Wait for mapped buffer to become available
          if (!WaitForResource(*mappedBuffer, sequenceNumber, MapType, MapFlags))
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
      }
    }

    // Mark the subresource as successfully mapped
    pResource->NotifyMap(Subresource, MapType);

    if (pMappedResource) {
      pMappedResource->pData      = pResource->GetMapPtr(Subresource, layout.Offset);
      pMappedResource->RowPitch   = layout.RowPitch;
      pMappedResource->DepthPitch = layout.DepthPitch;
    }

    m_mappedImageCount += 1;
    return S_OK;
  }
  
  
  void D3D11ImmediateContext::UnmapImage(
          D3D11CommonTexture*         pResource,
          UINT                        Subresource) {
    auto mapType = pResource->GetMapType(Subresource);
    auto mapMode = pResource->GetMapMode();

    if (mapType == D3D11CommonTexture::UnmappedSubresource)
      return;

    // Decrement mapped image counter only after making sure
    // the given subresource is actually mapped right now
    m_mappedImageCount -= 1;

    // If the texture has an image as well as a staging buffer,
    // upload the written buffer data to the image
    bool needsUpload = mapType != uint32_t(D3D11_MAP_READ)
      && (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_BUFFER || mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_DYNAMIC);

    if (needsUpload) {
      if (pResource->NeedsDirtyRegionTracking()) {
        for (uint32_t i = 0; i < pResource->GetDirtyRegionCount(Subresource); i++) {
          D3D11_COMMON_TEXTURE_REGION region = pResource->GetDirtyRegion(Subresource, i);
          UpdateDirtyImageRegion(pResource, Subresource, &region);
        }
      } else {
        UpdateDirtyImageRegion(pResource, Subresource, nullptr);
      }
    }

    // Unmap the subresource. This will implicitly destroy the
    // staging buffer for dynamically mapped images.
    pResource->NotifyUnmap(Subresource);
  }
  
  
  void D3D11ImmediateContext::ReadbackImageBuffer(
          D3D11CommonTexture*         pResource,
          UINT                        Subresource) {
    VkImageAspectFlags aspectMask = lookupFormatInfo(pResource->GetPackedFormat())->aspectMask;
    VkImageSubresource subresource = pResource->GetSubresourceFromIndex(aspectMask, Subresource);

    EmitCs([
      cSrcImage           = pResource->GetImage(),
      cSrcSubresource     = vk::makeSubresourceLayers(subresource),
      cDstBuffer          = pResource->GetMappedBuffer(Subresource),
      cPackedFormat       = pResource->GetPackedFormat()
    ] (DxvkContext* ctx) {
      VkOffset3D offset = { 0, 0, 0 };
      VkExtent3D extent = cSrcImage->mipLevelExtent(cSrcSubresource.mipLevel);

      ctx->copyImageToBuffer(cDstBuffer, 0, 0, 0, cPackedFormat,
        cSrcImage, cSrcSubresource, offset, extent);
    });

    if (pResource->HasSequenceNumber())
      TrackTextureSequenceNumber(pResource, Subresource);
  }


  void D3D11ImmediateContext::UpdateDirtyImageRegion(
          D3D11CommonTexture*         pResource,
          UINT                        Subresource,
    const D3D11_COMMON_TEXTURE_REGION* pRegion) {
    auto formatInfo = lookupFormatInfo(pResource->GetPackedFormat());
    auto subresource = vk::makeSubresourceLayers(
      pResource->GetSubresourceFromIndex(formatInfo->aspectMask, Subresource));

    // Update the entire image if no dirty region was specified
    D3D11_COMMON_TEXTURE_REGION region;

    if (pRegion) {
      region = *pRegion;
    } else {
      region.Offset = VkOffset3D { 0, 0, 0 };
      region.Extent = pResource->MipLevelExtent(subresource.mipLevel);
    }

    auto subresourceLayout = pResource->GetSubresourceLayout(formatInfo->aspectMask, Subresource);

    // Update dirty region one aspect at a time, due to
    // how the data is laid out in the staging buffer.
    for (uint32_t i = 0; i < pResource->GetPlaneCount(); i++) {
      subresource.aspectMask = formatInfo->aspectMask;

      if (formatInfo->flags.test(DxvkFormatFlag::MultiPlane))
        subresource.aspectMask = vk::getPlaneAspect(i);

      EmitCs([
        cDstImage       = pResource->GetImage(),
        cDstSubresource = subresource,
        cDstOffset      = region.Offset,
        cDstExtent      = region.Extent,
        cSrcBuffer      = pResource->GetMappedBuffer(Subresource),
        cSrcOffset      = pResource->ComputeMappedOffset(Subresource, i, region.Offset),
        cSrcRowPitch    = subresourceLayout.RowPitch,
        cSrcDepthPitch  = subresourceLayout.DepthPitch,
        cPackedFormat   = pResource->GetPackedFormat()
      ] (DxvkContext* ctx) {
        ctx->copyBufferToImage(
          cDstImage, cDstSubresource, cDstOffset, cDstExtent,
          cSrcBuffer, cSrcOffset, cSrcRowPitch, cSrcDepthPitch,
          cPackedFormat);
      });
    }

    if (pResource->HasSequenceNumber())
      TrackTextureSequenceNumber(pResource, Subresource);
  }


  void D3D11ImmediateContext::UpdateMappedBuffer(
          D3D11Buffer*                  pDstBuffer,
          UINT                          Offset,
          UINT                          Length,
    const void*                         pSrcData,
          UINT                          CopyFlags) {
    void* mapPtr = nullptr;

    if (likely(CopyFlags != D3D11_COPY_NO_OVERWRITE)) {
      auto bufferSlice = pDstBuffer->DiscardSlice(&m_allocationCache);
      mapPtr = bufferSlice->mapPtr();

      EmitCs([
        cBuffer      = pDstBuffer->GetBuffer(),
        cBufferSlice = std::move(bufferSlice)
      ] (DxvkContext* ctx) mutable {
        ctx->invalidateBuffer(cBuffer, std::move(cBufferSlice));
      });
    } else {
      mapPtr = pDstBuffer->GetMapPtr();
    }

    std::memcpy(reinterpret_cast<char*>(mapPtr) + Offset, pSrcData, Length);

    // Mirror to the worker's copy of the buffer. This is the direct-map
    // fast path UpdateSubresource takes for constant buffers, and the
    // single most-called D3D11 entry point in Superposition by a wide
    // margin (4.6M calls in one run) -- unmirrored, every constant
    // buffer on the worker device held whatever it was created with.
    // Zeroed view-projection matrices collapse every vertex to a point,
    // the pipeline runs and matches on both devices, and no fragment is
    // ever produced. That is the actual cause of the black composite.
    //
    // copyBuffer rather than uploadBuffer even on the DISCARD branch:
    // uploadBuffer requires the region not be in use by the GPU, which
    // the NO_OVERWRITE branch explicitly violates by design, and using
    // one path for both keeps this simple until it is measured.
    {
      DxvkBufferSlice workerSlice = pDstBuffer->GetWorkerBufferSlice(Offset, Length);

      {
        static std::atomic<uint64_t> s_hit  = { 0u };
        static std::atomic<uint64_t> s_miss = { 0u };

        uint64_t n = workerSlice.defined() ? ++s_hit : ++s_miss;

        if (n <= 2u || (n % 500000u) == 0u) {
          Logger::warn(str::format("UMBMIRROR ",
            workerSlice.defined() ? "hit=" : "miss=", n,
            " offset=", Offset, " length=", Length));
        }
      }

      if (workerSlice.defined()) {
        DxvkBufferSlice staging = m_workerStagingBuffer->alloc(Length);
        std::memcpy(staging.mapPtr(0), pSrcData, Length);
        if (m_cbReadEnabled && !m_cbReadValid && Length >= 16u) {
          m_cbReadBuffer = workerSlice.buffer();
          m_cbReadOffset = workerSlice.offset();
          DxvkBufferSlice primarySlice = pDstBuffer->GetBufferSlice(Offset, Length);
          m_cbReadPrimary       = primarySlice.buffer();
          m_cbReadPrimaryOffset = primarySlice.offset();
          std::memcpy(m_cbReadExpect, pSrcData, 16u);
          m_cbReadValid  = true;
          Logger::warn(str::format("CBREAD latched offset=", m_cbReadOffset,
            " length=", Length,
            " workerMap=", m_cbReadBuffer->mapPtr(0),
            " primaryMap=", m_cbReadPrimary->mapPtr(0)));
        }
        EmitWorkerCs([
          cDst     = std::move(workerSlice),
          cStaging = std::move(staging),
          cSize    = VkDeviceSize(Length)
        ] (DxvkContext* ctx) {
          ctx->copyBuffer(
            cDst.buffer(), cDst.offset(),
            cStaging.buffer(), cStaging.offset(),
            cSize);
        });
      }
    }
  }


  void STDMETHODCALLTYPE D3D11ImmediateContext::SwapDeviceContextState(
          ID3DDeviceContextState*           pState,
          ID3DDeviceContextState**          ppPreviousState) {
    InitReturnPtr(ppPreviousState);

    if (!pState)
      return;

    // Clear dirty tracking here since all context state will be
    // re-applied anyway when the context state is swapped in again.
    ResetDirtyTracking();

    // Reset all state affected by the current context state.
    ResetCommandListState();

    Com<D3D11DeviceContextState, false> oldState = std::move(m_stateObject);
    Com<D3D11DeviceContextState, false> newState = static_cast<D3D11DeviceContextState*>(pState);

    if (oldState == nullptr)
      oldState = new D3D11DeviceContextState(m_parent);
    
    if (ppPreviousState)
      *ppPreviousState = oldState.ref();
    
    m_stateObject = newState;

    oldState->SetState(m_state);
    newState->GetState(m_state);

    // Restore all state affected by the new context state
    RestoreCommandListState();
  }


  void D3D11ImmediateContext::Acquire11on12Resource(
          ID3D11Resource*             pResource,
          VkImageLayout               SrcLayout) {
    D3D10DeviceLock lock = LockContext();

    auto texture = GetCommonTexture(pResource);
    auto buffer = GetCommonBuffer(pResource);

    Rc<DxvkPagedResource> resource;

    if (texture)
      resource = texture->GetImage();
    else if (buffer)
      resource = buffer->GetBuffer();

    if (resource) {
      EmitCs([
        cResource = std::move(resource),
        cLayout   = SrcLayout
      ] (DxvkContext* ctx) {
        ctx->acquireExternalResource(cResource, cLayout);
      });
    }
  }


  void D3D11ImmediateContext::Release11on12Resource(
          ID3D11Resource*             pResource,
          VkImageLayout               DstLayout) {
    D3D10DeviceLock lock = LockContext();

    auto texture = GetCommonTexture(pResource);
    auto buffer = GetCommonBuffer(pResource);

    Rc<DxvkPagedResource> resource;

    if (texture)
      resource = texture->GetImage();
    else if (buffer)
      resource = buffer->GetBuffer();

    if (resource) {
      EmitCs([
        cResource = std::move(resource),
        cLayout   = DstLayout
      ] (DxvkContext* ctx) {
        ctx->releaseExternalResource(cResource, cLayout);
      });
    }
  }


  void D3D11ImmediateContext::SynchronizeCsThread(uint64_t SequenceNumber) {
    D3D10DeviceLock lock = LockContext();

    // Dispatch current chunk so that all commands
    // recorded prior to this function will be run
    if (SequenceNumber > m_csSeqNum)
      FlushCsChunk();
    
    m_csThread.synchronize(SequenceNumber);
  }
  
  
  void D3D11ImmediateContext::SynchronizeDevice() {
    m_device->waitForIdle();
  }
  
  
  void D3D11ImmediateContext::EndFrame(
          Rc<DxvkLatencyTracker>      LatencyTracker) {
    D3D10DeviceLock lock = LockContext();

    // Don't keep draw buffers alive indefinitely. This cannot be
    // done in ExecuteFlush because command recording itself might
    // flush, so no state changes are allowed to happen there.
    SetDrawBuffers(nullptr, nullptr);

    EmitCs<false>([
      cTracker = std::move(LatencyTracker)
    ] (DxvkContext* ctx) {
      ctx->endFrame();

      if (cTracker && cTracker->needsAutoMarkers())
        ctx->endLatencyTracking(cTracker);
    });
  }


  bool D3D11ImmediateContext::WaitForResource(
    const DxvkPagedResource&                Resource,
          uint64_t                          SequenceNumber,
          D3D11_MAP                         MapType,
          UINT                              MapFlags) {
    // Determine access type to wait for based on map mode
    DxvkAccess access = MapType == D3D11_MAP_READ
      ? DxvkAccess::Write
      : DxvkAccess::Read;
    
    // Wait for any CS chunk using the resource to execute, since
    // otherwise we cannot accurately determine if the resource is
    // actually being used by the GPU right now.
    if (!Resource.isInUse(access)) {
      SynchronizeCsThread(SequenceNumber);

      if (!Resource.isInUse(access))
        return true;
    }

    if (unlikely(m_device->debugFlags().test(DxvkDebugFlag::Capture))) {
      m_flushReason = str::format("Map ", Resource.getDebugName(), " (MAP",
        MapType != D3D11_MAP_WRITE ? "_READ" : "",
        MapType != D3D11_MAP_READ ? "_WRITE" : "", ")");
    }

    if (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT) {
      // We don't have to wait, but misbehaving games may
      // still try to spin on `Map` until the resource is
      // idle, so we should flush pending commands
      ConsiderFlush(GpuFlushType::ImplicitSynchronization);
      return false;
    } else {
      // Make sure pending commands using the resource get
      // executed on the the GPU if we have to wait for it
      ExecuteFlush(GpuFlushType::ImplicitSynchronization, nullptr, false);
      SynchronizeCsThread(SequenceNumber);

      m_device->waitForResource(Resource, access);
      return true;
    }
  }


  void D3D11ImmediateContext::InjectCsChunk(
          DxvkCsQueue                 Queue,
          DxvkCsChunkRef&&            Chunk,
          bool                        Synchronize) {
    // Do not update the sequence number when emitting a chunk
    // from an external source since that would break tracking
    m_csThread.injectChunk(Queue, std::move(Chunk), Synchronize);
  }


  bool D3D11ImmediateContext::InjectWorkerCsChunk(
          DxvkCsChunkRef&&            Chunk) {
    if (!m_workerCsThread)
      return false;

    m_workerCsThread->dispatchChunk(std::move(Chunk));
    return true;
  }


  void D3D11ImmediateContext::EmitCsChunk(DxvkCsChunkRef&& chunk) {
    // Flush init commands so that the CS thread
    // can processe them before the first use.
    m_parent->FlushInitCommands();

    m_csSeqNum = m_csThread.dispatchChunk(std::move(chunk));
  }

  

  void D3D11ImmediateContext::FlushWorkerCsChunk() {
    if (!m_workerCsThread)
      return;

    bool isEmpty = m_workerCsChunk->empty();

    // Opt-in diagnostic. An empty worker chunk means the composite is
    // reading a target nothing was dispatched to, which looks like a
    // very fast composite rather than like a bug -- the same shape of
    // false success as the deferred clears that never reached GPU1 and
    // the submission wait that never waited. Off by default; enable
    // with DXVK_MGPU_DEBUG_FLUSH=1.
    if (unlikely(m_workerFlushDebug)) {
      m_workerFlushCalls++;
      m_workerFlushEmpty += isEmpty ? 1u : 0u;

      if ((m_workerFlushCalls % 60u) == 0u) {
        Logger::info(str::format("workerflush: calls ", m_workerFlushCalls,
          " empty ", m_workerFlushEmpty));
      }
    }

    if (isEmpty)
      return;

    m_lastWorkerSeq = m_workerCsThread->dispatchChunk(std::move(m_workerCsChunk));
    m_workerCsChunk = m_parent->AllocCsChunk(DxvkCsChunkFlag::SingleUse);
  }

    void D3D11ImmediateContext::EmitWorkerClearRenderTarget(
    const DxvkAttachment&       Attachment,
    const VkClearValue&         ClearValue) {
    if (!m_workerCsThread)
      return;
    // Use the application's own clear colour. The green override made
    // GPU1's output obvious while only clears were mirrored, but now
    // that draws are mirrored too it hides whether they wrote anything.
    VkClearValue workerClear = ClearValue;
    EmitWorkerCs([
      cAttachment = Attachment,
      cClearValue = workerClear
    ] (DxvkContext* ctx) {
      ctx->clearRenderTarget(cAttachment,
        VK_IMAGE_ASPECT_COLOR_BIT, cClearValue, 0u);
    });

    // Fall back to updating the frame image here too: -clearonly style
    // paths call ClearRenderTargetView on an already-bound RTV without
    // an intervening OMSetRenderTargets, so BindFramebuffer() never
    // fires and EmitWorkerBindFramebuffer() never runs. Once real draws
    // exist this becomes redundant with the bind-path update, which is
    // harmless -- both agree on the same image.
    m_workerFrameImage = Attachment.view->image();
  }


  void D3D11ImmediateContext::EmitWorkerBindFramebuffer(
          DxvkRenderTargets&&         Targets) {
    if (!m_workerCsThread)
      return;

    // Remember which worker image holds this frame's output so that
    // Present knows what to composite. Only updated when this bind
    // actually provides a color target -- an unbind (Targets.color[0]
    // null) must not erase the last known frame image, or a mid-frame
    // unbind would blank the composite.
    // Opt-in: confirm the worker receives usable attachments. A null
    // colour view here would explain draws that execute but produce no
    // pixels. Enable with DXVK_MGPU_DEBUG_DRAW=1.
    if (unlikely(m_workerDrawDebug)) {
      static uint32_t s_bindCount = 0;

      if (s_bindCount++ < 6u) {
        auto& c0 = Targets.color[0];

        if (c0.view != nullptr) {
          VkExtent3D e = c0.view->image()->info().extent;
          Logger::info(str::format("worker bindFB #", s_bindCount,
            ": colour ", e.width, "x", e.height,
            " depth ", (Targets.depth.view != nullptr ? "yes" : "no")));
        } else {
          Logger::info(str::format("worker bindFB #", s_bindCount,
            ": colour NULL depth ",
            (Targets.depth.view != nullptr ? "yes" : "no")));
        }
      }
    }

    if (Targets.color[0].view != nullptr) {
      // Diagnostic: keep the biggest colour target the worker has ever
      // bound. Superposition renders its scene at full resolution and
      // presents into a smaller window, so the back buffer only ever
      // receives a final scale-and-present blit. If the worker's scene
      // targets are empty then that blit correctly copies nothing, and
      // reading one directly says where the emptiness starts.
      {
        Rc<DxvkImage> candidate = Targets.color[0].view->image();

        VkExtent3D ce = candidate->info().extent;
        uint64_t   ca = uint64_t(ce.width) * ce.height;

        uint64_t best = 0;

        if (m_workerLargestImage != nullptr) {
          VkExtent3D be = m_workerLargestImage->info().extent;
          best = uint64_t(be.width) * be.height;
        }

        if (ca > best)
          m_workerLargestImage = candidate;
      }

      m_workerFrameImage = Targets.color[0].view->image();
      m_workerFrameView  = Targets.color[0].view;
      static uint32_t s_b = 0;

      if (s_b++ < 8u) {
        Logger::warn(str::format("BIND frame image = ",
          reinterpret_cast<uintptr_t>(m_workerFrameImage.ptr()),
          "  view = ", reinterpret_cast<uintptr_t>(Targets.color[0].view.ptr())));
      }
    }

    EmitWorkerCs([
      cTargets = std::move(Targets)
    ] (DxvkContext* ctx) mutable {
      ctx->bindRenderTargets(std::move(cTargets), 0u);
    });
  }

  bool D3D11ImmediateContext::CreateSharedHostStaging(VkDeviceSize Size) {
    Rc<DxvkDevice> workerDevice = m_parent->GetWorkerDXVKDevice();
    if (workerDevice == nullptr)
      return false;

    if (workerDevice->vkd()->vkGetMemoryHostPointerPropertiesEXT == nullptr
     || m_device->vkd()->vkGetMemoryHostPointerPropertiesEXT == nullptr) {
      static bool s_warned = false;

      if (!s_warned) {
        s_warned = true;
        Logger::warn("MGPU: external_memory_host unavailable, composite disabled");
      }

      return false;
    }

    DestroySharedHostStaging();

    auto workerVk  = workerDevice->vkd();
    auto primaryVk = m_device->vkd();
    auto vki       = m_device->vki();

    // Both devices impose their own alignment requirement on an imported host
    // pointer; the allocation has to satisfy the stricter of the two.
    VkDeviceSize alignment = 1u;

    for (VkPhysicalDevice phys : { m_device->adapter()->handle(),
                                   workerDevice->adapter()->handle() }) {
      VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
      VkPhysicalDeviceProperties2 props =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
      props.pNext = &hostProps;

      vki->vkGetPhysicalDeviceProperties2(phys, &props);
      alignment = std::max(alignment, hostProps.minImportedHostPointerAlignment);
    }

    // The imported range must be a whole number of alignment units.
    VkDeviceSize allocSize = align(Size, alignment);

    m_sharedHostMem = _aligned_malloc(allocSize, alignment);

    if (!m_sharedHostMem) {
      Logger::err("D3D11: failed to allocate shared host staging memory");
      return false;
    }

    m_sharedHostSize = allocSize;

    // Import the same pages into each device in turn.
    struct { Rc<DxvkDevice> dev; VkBuffer* buf; VkDeviceMemory* mem; } targets[] = {
      { workerDevice, &m_workerVkBuffer,  &m_workerVkMemory  },
      { m_device,     &m_primaryVkBuffer, &m_primaryVkMemory },
    };

    for (auto& t : targets) {
      auto vk = t.dev->vkd();

      VkExternalMemoryBufferCreateInfo extBufferInfo =
        { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
      extBufferInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

      VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
      bufferInfo.pNext       = &extBufferInfo;
      bufferInfo.size        = allocSize;
      bufferInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      if (vk->vkCreateBuffer(t.dev->handle(), &bufferInfo, nullptr, t.buf) != VK_SUCCESS) {
        Logger::err("D3D11: vkCreateBuffer failed for shared host staging");
        DestroySharedHostStaging();
        return false;
      }

      // Ask which memory types can accept this particular host pointer.
      VkMemoryHostPointerPropertiesEXT ptrProps =
        { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };

      if (vk->vkGetMemoryHostPointerPropertiesEXT(t.dev->handle(),
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            m_sharedHostMem, &ptrProps) != VK_SUCCESS) {
        Logger::err("D3D11: vkGetMemoryHostPointerPropertiesEXT failed");
        DestroySharedHostStaging();
        return false;
      }

      if (!ptrProps.memoryTypeBits) {
        Logger::err("D3D11: no memory type accepts the host pointer");
        DestroySharedHostStaging();
        return false;
      }

      // Any type in the mask is host memory by construction, so the lowest
      // set bit is as good as any.
      uint32_t typeIndex = 0u;
      while (!(ptrProps.memoryTypeBits & (1u << typeIndex)))
        typeIndex++;

      // That assumption is not what the spec promises. The mask may
      // include types from heaps otherwise unusable for allocation,
      // and nothing guarantees the one picked here is HOST_COHERENT.
      // A non-coherent type needs vkInvalidateMappedMemoryRanges
      // before every read; without it the CPU sees stale contents
      // even though the GPU wrote, which is the symptom being chased.
      {
        const auto& memProps = t.dev->adapter()->memoryProperties();

        VkMemoryPropertyFlags flags =
          memProps.memoryTypes[typeIndex].propertyFlags;

        Logger::warn(str::format("MEMTYPE dev=",
          reinterpret_cast<uintptr_t>(t.dev->handle()),
          " mask=", ptrProps.memoryTypeBits,
          " chosen=", typeIndex,
          " flags=", flags,
          " coherent=", bool(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
          " cached=", bool(flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)));
      }

      // That assumption is not what the spec promises. The mask may
      // include types from heaps that are otherwise unusable, and
      // nothing guarantees the one picked here is HOST_COHERENT. A
      // non-coherent type would need vkInvalidateMappedMemoryRanges
      // before every read, without which the CPU sees stale contents
      // even though the GPU wrote -- which is exactly the symptom
      // being chased. Log what was actually selected.
      {
        VkPhysicalDeviceMemoryProperties memProps = { };
        t.dev->adapter()->vki()->vkGetPhysicalDeviceMemoryProperties(
          t.dev->adapter()->handle(), &memProps);

        VkMemoryPropertyFlags flags =
          memProps.memoryTypes[typeIndex].propertyFlags;

        Logger::warn(str::format("MEMTYPE dev=",
          reinterpret_cast<uintptr_t>(t.dev->handle()),
          " mask=", ptrProps.memoryTypeBits,
          " chosen=", typeIndex,
          " flags=", flags,
          " coherent=", bool(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
          " cached=", bool(flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)));
      }

      VkImportMemoryHostPointerInfoEXT importInfo =
        { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
      importInfo.handleType   = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
      importInfo.pHostPointer = m_sharedHostMem;

      VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
      allocInfo.pNext           = &importInfo;
      allocInfo.allocationSize  = allocSize;
      allocInfo.memoryTypeIndex = typeIndex;

      if (vk->vkAllocateMemory(t.dev->handle(), &allocInfo, nullptr, t.mem) != VK_SUCCESS) {
        Logger::err("D3D11: vkAllocateMemory failed importing host pointer");
        DestroySharedHostStaging();
        return false;
      }

      if (vk->vkBindBufferMemory(t.dev->handle(), *t.buf, *t.mem, 0) != VK_SUCCESS) {
        Logger::err("D3D11: vkBindBufferMemory failed for shared host staging");
        DestroySharedHostStaging();
        return false;
      }
    }

    // Wrap both raw buffers so the existing copy helpers can take them.
    DxvkBufferCreateInfo dxvkInfo = { };
    dxvkInfo.size      = allocSize;
    dxvkInfo.usage     = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                       | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    dxvkInfo.stages    = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dxvkInfo.access    = VK_ACCESS_TRANSFER_READ_BIT
                       | VK_ACCESS_TRANSFER_WRITE_BIT;
    dxvkInfo.debugName = "mgpu shared host staging";

    const VkMemoryPropertyFlags hostFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    DxvkBufferImportInfo workerImport = { };
    workerImport.buffer = m_workerVkBuffer;
    workerImport.offset = 0;
    workerImport.mapPtr = m_sharedHostMem;

    DxvkBufferImportInfo primaryImport = { };
    primaryImport.buffer = m_primaryVkBuffer;
    primaryImport.offset = 0;
    primaryImport.mapPtr = m_sharedHostMem;

    m_workerStaging  = workerDevice->importBuffer(dxvkInfo, workerImport,  hostFlags);
    m_primaryStaging = m_device->importBuffer(dxvkInfo, primaryImport, hostFlags);

    Logger::info(str::format("D3D11: shared host staging ", allocSize,
      " bytes, alignment ", alignment, " -- memcpy eliminated"));

    return true;
  }


  void D3D11ImmediateContext::DestroySharedHostStaging() {
    m_workerStaging  = nullptr;
    m_primaryStaging = nullptr;

    Rc<DxvkDevice> workerDevice = m_parent->GetWorkerDXVKDevice();

    if (workerDevice != nullptr) {
      // The worker device has its own CS thread and is not covered by the
      // primary device's synchronisation, so make sure GPU1 is done before
      // freeing memory it may still be reading from or writing to.
      workerDevice->waitForIdle();

      auto vk = workerDevice->vkd();

      if (m_workerVkBuffer)
        vk->vkDestroyBuffer(workerDevice->handle(), m_workerVkBuffer, nullptr);
      if (m_workerVkMemory)
        vk->vkFreeMemory(workerDevice->handle(), m_workerVkMemory, nullptr);
    }

    auto vk = m_device->vkd();

    if (m_primaryVkBuffer)
      vk->vkDestroyBuffer(m_device->handle(), m_primaryVkBuffer, nullptr);
    if (m_primaryVkMemory)
      vk->vkFreeMemory(m_device->handle(), m_primaryVkMemory, nullptr);

    m_workerVkBuffer  = VK_NULL_HANDLE;
    m_workerVkMemory  = VK_NULL_HANDLE;
    m_primaryVkBuffer = VK_NULL_HANDLE;
    m_primaryVkMemory = VK_NULL_HANDLE;

    if (m_sharedHostMem) {
      _aligned_free(m_sharedHostMem);
      m_sharedHostMem  = nullptr;
      m_sharedHostSize = 0u;
    }
  }
  bool D3D11ImmediateContext::StealSharedBlock(SharedBlock& Block) {
    Block.mem             = m_sharedHostMem;
    Block.size            = m_sharedHostSize;
    Block.workerVkBuffer  = m_workerVkBuffer;
    Block.workerVkMemory  = m_workerVkMemory;
    Block.primaryVkBuffer = m_primaryVkBuffer;
    Block.primaryVkMemory = m_primaryVkMemory;
    Block.worker          = std::move(m_workerStaging);
    Block.primary         = std::move(m_primaryStaging);
    m_sharedHostMem   = nullptr;
    m_sharedHostSize  = 0u;
    m_workerVkBuffer  = VK_NULL_HANDLE;
    m_workerVkMemory  = VK_NULL_HANDLE;
    m_primaryVkBuffer = VK_NULL_HANDLE;
    m_primaryVkMemory = VK_NULL_HANDLE;
    m_workerStaging   = nullptr;
    m_primaryStaging  = nullptr;
    return Block.mem != nullptr && Block.worker != nullptr && Block.primary != nullptr;
  }


  void D3D11ImmediateContext::ReleaseSharedBlock(SharedBlock& Block) {
    if (Block.mem == nullptr)
      return;
    m_sharedHostMem   = Block.mem;
    m_sharedHostSize  = Block.size;
    m_workerVkBuffer  = Block.workerVkBuffer;
    m_workerVkMemory  = Block.workerVkMemory;
    m_primaryVkBuffer = Block.primaryVkBuffer;
    m_primaryVkMemory = Block.primaryVkMemory;
    m_workerStaging   = std::move(Block.worker);
    m_primaryStaging  = std::move(Block.primary);
    DestroySharedHostStaging();
    Block = SharedBlock();
  }


  void D3D11ImmediateContext::ReleasePipeline() {
    for (uint32_t i = 0; i < PipeBlocks; i++)
      ReleaseSharedBlock(m_pipeBlocks[i]);
    for (uint32_t i = 0; i < 2u; i++)
      m_pipeImages[i] = nullptr;
    m_pipeExtent = VkExtent3D { 0u, 0u, 0u };
    m_pipeFrame  = 0u;
    m_pipeValid  = false;
  }


  void D3D11ImmediateContext::CompositeWorkerOutputPipelined(
    const Rc<DxvkImage>&        DstImage,
    const Rc<DxvkImage>&        SrcImage) {
    FlushWorkerCsChunk();

    if (!m_workerCsThread || DstImage == nullptr || SrcImage == nullptr)
      return;

    Rc<DxvkDevice> workerDevice = m_parent->GetWorkerDXVKDevice();
    VkExtent3D extent = SrcImage->info().extent;

    if (extent != DstImage->info().extent)
      return;

    VkDeviceSize imageBytes = VkDeviceSize(extent.width) * extent.height * 4u;

    if (extent != m_pipeExtent) {
      ReleasePipeline();

      for (uint32_t i = 0; i < PipeBlocks; i++) {
        if (!CreateSharedHostStaging(imageBytes) || !StealSharedBlock(m_pipeBlocks[i])) {
          Logger::err("D3D11: pipelined composite: shared block allocation failed");
          ReleasePipeline();
          return;
        }
      }

      DxvkImageCreateInfo info = DstImage->info();
      info.usage  = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      info.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      info.viewFormatCount = 0u;
      info.debugName = "mgpu pipeline";

      for (uint32_t i = 0; i < 2u; i++)
        m_pipeImages[i] = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      m_pipeExtent = extent;
      m_presentExtent = VkExtent2D { extent.width, extent.height };
      ApplyViewportState();
      Logger::info(str::format("D3D11: pipelined composite ready, ",
        extent.width, "x", extent.height, ", ", PipeBlocks, " blocks"));
    }

    uint32_t cur     = m_pipeFrame % PipeBlocks;
    uint32_t prev    = (m_pipeFrame + PipeBlocks - 1u) % PipeBlocks;
    uint32_t curImg  = m_pipeFrame & 1u;
    uint32_t prevImg = curImg ^ 1u;

    VkImageSubresourceLayers sub = { };
    sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sub.layerCount = 1;

    // 1. GPU0 saves the application's finished frame N before it is
    //    overwritten below with frame N-1.
    EmitCs([
      cDst    = m_pipeImages[curImg],
      cSrc    = DstImage,
      cSub    = sub,
      cExtent = extent
    ] (DxvkContext* ctx) {
      ctx->copyImage(cDst, cSub, VkOffset3D { 0, 0, 0 },
        cSrc, cSub, VkOffset3D { 0, 0, 0 }, cExtent);
    });

    // 2. GPU0 last read block[cur] three frames ago. Cross-device, so
    //    the ordering is a CPU wait; with three blocks it is normally
    //    already satisfied.
    m_device->waitForResource(*m_pipeBlocks[cur].primary, DxvkAccess::Read);

    // 3. GPU1 copies its half of frame N into block[cur] and submits.
    //    Nothing waits for it here.
    EmitWorkerCs([
      cBuffer = m_pipeBlocks[cur].worker,
      cImage  = SrcImage,
      cSub    = sub,
      cExtent = extent
    ] (DxvkContext* ctx) {
      ctx->copyImageToBuffer(cBuffer, 0, 0, 0,
        VK_FORMAT_R8G8B8A8_UNORM, cImage, cSub,
        VkOffset3D { 0, 0, 0 }, cExtent);
      ctx->flushCommandList(nullptr, nullptr);
    });
    FlushWorkerCsChunk();
    m_pipeBlocks[cur].workerSeq = m_lastWorkerSeq;

    // 4. Present frame N-1: restore GPU0's saved half, composite GPU1's
    //    half from block[prev]. That copy was dispatched a frame ago;
    //    make sure the worker CS thread has executed it, then wait for
    //    the GPU write, which should already be complete.
    if (m_pipeValid) {
      auto t0 = dxvk::high_resolution_clock::now();
      m_workerCsThread->synchronize(m_pipeBlocks[prev].workerSeq);
      workerDevice->waitForResource(*m_pipeBlocks[prev].worker, DxvkAccess::Write);
      auto t1 = dxvk::high_resolution_clock::now();
      {
        static uint64_t s_acc = 0; static uint32_t s_n = 0;
        s_acc += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if ((++s_n % 600u) == 0u) {
          Logger::warn(str::format("PIPEWAIT avg over 600: ", s_acc / 600u, " us"));
          s_acc = 0;
        }
      }

      EmitCs([
        cDst    = DstImage,
        cSaved  = m_pipeImages[prevImg],
        cBuffer = m_pipeBlocks[prev].primary,
        cSub    = sub,
        cExtent = extent
      ] (DxvkContext* ctx) {
        ctx->copyImage(cDst, cSub, VkOffset3D { 0, 0, 0 },
          cSaved, cSub, VkOffset3D { 0, 0, 0 }, cExtent);
        uint32_t splitY = cExtent.height / 2u;
        VkExtent3D half = { cExtent.width, cExtent.height - splitY, 1u };
        VkDeviceSize srcOffset = VkDeviceSize(splitY) * cExtent.width * 4u;
        ctx->copyBufferToImage(cDst, cSub,
          VkOffset3D { 0, int32_t(splitY), 0 }, half,
          cBuffer, srcOffset, 0, 0,
          VK_FORMAT_R8G8B8A8_UNORM);
      });

      if (unlikely(m_workerDrawDebug)) {
        static uint32_t s_rb = 0;
        if ((s_rb++ % 30u) == 0u) {
          const uint8_t* px = reinterpret_cast<const uint8_t*>(m_pipeBlocks[prev].mem);
          uint32_t w = extent.width;
          uint32_t h = extent.height;
          auto sample = [px, w] (uint32_t x, uint32_t y) {
            size_t o = (size_t(y) * w + x) * 4u;
            return str::format("[", uint32_t(px[o]), ",", uint32_t(px[o + 1]),
              ",", uint32_t(px[o + 2]), ",", uint32_t(px[o + 3]), "]");
          };
          uint32_t litBottom = 0u, totalBottom = 0u, litTop = 0u;
          const uint8_t* ref = px + (size_t(h - 1u) * w + (w - 1u)) * 4u;
          for (uint32_t y = 0; y < h; y += 4u) {
            for (uint32_t x = 0; x < w; x += 4u) {
              const uint8_t* p = px + (size_t(y) * w + x) * 4u;
              bool differs = p[0] != ref[0] || p[1] != ref[1] || p[2] != ref[2];
              if (y >= h / 2u) { totalBottom++; if (differs) litBottom++; }
              else if (differs) litTop++;
            }
          }
          Logger::info(str::format("worker image ", w, "x", h,
            " centre ", sample(w / 2u, h / 2u),
            " topleft ", sample(w / 8u, h / 8u),
            " corner ", sample(2u, 2u),
            " bottomLit=", litBottom, "/", totalBottom, " topLit=", litTop));
        }
      }
    }

    m_pipeValid = true;
    m_pipeFrame++;
  }


  void D3D11ImmediateContext::LogMapStats() {
    if (!m_workerMapDebug || !m_mapFrames)
      return;

    uint32_t f = m_mapFrames;
    uint64_t totalBytes = m_mapDiscardBytes + m_mapNoOverwriteBytes + m_mapOtherBytes;
    uint32_t totalCount = m_mapDiscardCount + m_mapNoOverwriteCount + m_mapOtherCount;

    Logger::info(str::format("buffer map traffic over ", f, " frames:"));
    Logger::info(str::format("  discard:      ", m_mapDiscardCount,
      " (", m_mapDiscardCount / f, "/frame)  ",
      m_mapDiscardBytes / 1024u, " KiB  (",
      m_mapDiscardBytes / f / 1024u, " KiB/frame)"));
    Logger::info(str::format("  no_overwrite: ", m_mapNoOverwriteCount,
      " (", m_mapNoOverwriteCount / f, "/frame)  ",
      m_mapNoOverwriteBytes / 1024u, " KiB  (",
      m_mapNoOverwriteBytes / f / 1024u, " KiB/frame)"));
    Logger::info(str::format("  other:        ", m_mapOtherCount,
      " (", m_mapOtherCount / f, "/frame)  ",
      m_mapOtherBytes / 1024u, " KiB  (",
      m_mapOtherBytes / f / 1024u, " KiB/frame)"));
    Logger::info(str::format("  total:        ", totalCount,
      " (", totalCount / f, "/frame)  ",
      totalBytes / 1024u, " KiB  (",
      totalBytes / f / 1024u, " KiB/frame)"));

    // At PCIe 3.0 x8 (~7.9 GB/s) and x16 (~15.8 GB/s), what a full
    // duplicate of this traffic would cost per frame if it had to
    // cross the bus rather than being written once into shared pages.
    uint64_t perFrame = totalBytes / f;
    Logger::info(str::format("  duplicating this per frame: ",
      (perFrame * 1000000ull) / (7900ull * 1024ull * 1024ull), " us at x8, ",
      (perFrame * 1000000ull) / (15800ull * 1024ull * 1024ull), " us at x16"));
  }
  void D3D11ImmediateContext::LogCompositeStats() {
    if (!m_compositeCount)
      return;

    auto report = [] (const char* name, const std::vector<uint32_t>& samples) {
      if (samples.empty())
        return;

      // Where the worst sample happened separates warm-up from steady
      // state: an early index means compilation or first-touch cost,
      // a late or evenly spread one means something recurring.
      size_t maxIndex = 0;
      uint64_t sum = 0;

      for (size_t i = 0; i < samples.size(); i++) {
        sum += samples[i];

        if (samples[i] > samples[maxIndex])
          maxIndex = i;
      }

      std::vector<uint32_t> sorted = samples;
      std::sort(sorted.begin(), sorted.end());

      auto pct = [&sorted] (double p) {
        size_t idx = size_t(p * double(sorted.size() - 1));
        return sorted[idx];
      };

      // Count how much of the total time lives in the worst 1%. If the
      // tail dominates the sum, the average is describing a workload
      // that mostly does not happen.
      uint64_t tailSum = 0;
      size_t tailStart = sorted.size() - std::max<size_t>(1u, sorted.size() / 100u);

      for (size_t i = tailStart; i < sorted.size(); i++)
        tailSum += sorted[i];

      Logger::info(str::format("  ", name,
        ": p50 ", pct(0.50),
        "us  p90 ", pct(0.90),
        "us  p99 ", pct(0.99),
        "us  max ", sorted.back(),
        "us @sample ", maxIndex,
        "  top1% holds ", sum ? (tailSum * 100u) / sum : 0u, "% of total"));
    };

    Logger::info(str::format("composite stats over ", m_compositeCount, " composites:"));
    Logger::info(str::format("  cs sync:  min ", m_compositeSyncMin,
      "us  avg ", m_compositeSyncSum / m_compositeCount,
      "us  max ", m_compositeSyncMax, "us"));
    Logger::info(str::format("  gpu wait: min ", m_compositeSubMin,
      "us  avg ", m_compositeSubSum / m_compositeCount,
      "us  max ", m_compositeSubMax, "us"));
    report("cs sync ", m_compositeSyncLog);
    report("gpu wait", m_compositeSubLog);
  }


  void D3D11ImmediateContext::CompositeWorkerOutput(
    const Rc<DxvkImage>&        DstImage,
    const Rc<DxvkImage>&        SrcImage) {
    // Anything still batched must reach GPU1 before we read its image
    // back, or the composite copies a target whose mirrored work is
    // still sitting in an un-dispatched chunk. Doing this here rather
    // than at the call site keeps the guarantee next to the read that
    // depends on it.
    static uint32_t s_entry = 0;
    bool tr = (s_entry++ < 12u);

    if (tr) Logger::warn(str::format("composite entry ", s_entry, ": before flush"));

    FlushWorkerCsChunk();

    if (tr) Logger::warn(str::format("composite entry ", s_entry, ": after flush"));

    if (!m_workerCsThread || DstImage == nullptr)
      return;

    if (SrcImage == nullptr) {
      static uint32_t s_noSrc = 0;

      if (s_noSrc++ < 4u) {
        Logger::warn("D3D11: back buffer has no worker image, skipping composite");
      }

      return;
    }

    // Diagnostic gate: read the worker's largest render target instead
    // of the back buffer mirror. Superposition renders its scene at
    // full resolution and presents into a smaller window, so the back
    // buffer only receives a final scale-and-present blit. If the
    // scene targets are empty too, the problem is upstream of the
    // blit. The extent check below rejects this whenever the sizes
    // differ, so the point is the readback, not the screen.
    bool readLargest = env::getEnvVar("DXVK_MGPU_READ_LARGEST") == "1"
                    && m_workerLargestImage != nullptr;

    Rc<DxvkImage> readSource = readLargest
      ? m_workerLargestImage
      : SrcImage;

    Rc<DxvkDevice> workerDevice = m_parent->GetWorkerDXVKDevice();

    static bool s_checked = false;
    if (!s_checked) {
      s_checked = true;
      Logger::info(str::format("extmem_host fn ptr: worker=",
        workerDevice->vkd()->vkGetMemoryHostPointerPropertiesEXT != nullptr,
        " primary=", m_device->vkd()->vkGetMemoryHostPointerPropertiesEXT != nullptr));
    }    

    {
      static uint32_t s_c = 0;

      if (s_c++ < 8u) {
        Logger::warn(str::format("COMPOSITE reading image = ",
          reinterpret_cast<uintptr_t>(readSource.ptr()),
          "  lastBind = ",
          reinterpret_cast<uintptr_t>(m_workerFrameImage.ptr())));
      }
    }

    VkExtent3D   extent     = readSource->info().extent;
    VkDeviceSize imageBytes = VkDeviceSize(extent.width) * extent.height * 4u;
    if (m_presentExtent.width != extent.width || m_presentExtent.height != extent.height) {
      m_presentExtent = VkExtent2D { extent.width, extent.height };
      ApplyViewportState();
    }

    // With the diagnostic gate on, the source is deliberately a
    // different size from the back buffer, so the mismatch is expected
    // and the final copy to the swap chain is skipped instead. The
    // readback still runs, which is the entire point.
    bool extentMismatch = extent != DstImage->info().extent;

    if (extentMismatch && !readLargest) {
      Logger::warn("D3D11: worker/primary extent mismatch, skipping composite");
      return;
    }

    if (m_stagingSize < imageBytes) {
      if (tr) Logger::warn(str::format("composite entry ", s_entry,
        ": allocating staging ", imageBytes, " bytes for ",
        extent.width, "x", extent.height));

      if (!CreateSharedHostStaging(imageBytes))
        return;

      m_stagingSize = imageBytes;

      if (tr) Logger::warn(str::format("composite entry ", s_entry, ": staging ready"));
    }

    // GPU1: worker image -> worker host buffer, then flush and wait on
    // that specific submission rather than stalling the whole device.
    DxvkSubmitStatus submitStatus = { };
    submitStatus.result.store(VK_NOT_READY);

    DxvkCsChunkRef download = m_parent->AllocCsChunk(DxvkCsChunkFlag::SingleUse);

    download->push([
      cBuffer = m_workerStaging,
      cImage  = readSource,
      cClearView = (env::getEnvVar("DXVK_MGPU_TEST_CLEAR") == "1"
        ? m_workerFrameView : Rc<DxvkImageView>()),
      cExtent = extent,
      cStatus = &submitStatus
    ] (DxvkContext* ctx) {
      static uint32_t s_exec = 0;
      bool t = (s_exec++ < 10u);

      if (t) Logger::warn(str::format("  worker chunk ", s_exec, ": entered"));

      VkImageSubresourceLayers sub = { };
      sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      sub.layerCount = 1;

      // Proven technique from 6d49763a: clearImageView opens a render
      // pass and draws a triangle rather than deferring to loadOp, so
      // a magenta result means GPU1 rasterises, produces fragments,
      // writes them to the bound attachment, and the transfer carries
      // them home. Only mirrored draws would then be at fault. Zeroes
      // mean something more fundamental is wrong here than in the
      // testbed, where this same path works.
      if (cClearView != nullptr) {
        VkClearValue magenta = { };
        magenta.color.float32[0] = 1.0f;
        magenta.color.float32[1] = 0.0f;
        magenta.color.float32[2] = 1.0f;
        magenta.color.float32[3] = 1.0f;

        ctx->clearImageView(cClearView,
          VkOffset3D{ 0, 0, 0 }, cExtent,
          VK_IMAGE_ASPECT_COLOR_BIT, magenta);
      }

      ctx->copyImageToBuffer(cBuffer, 0, 0, 0,
        VK_FORMAT_R8G8B8A8_UNORM, cImage, sub,
        VkOffset3D{ 0, 0, 0 }, cExtent);

      if (t) Logger::warn(str::format("  worker chunk ", s_exec, ": copy recorded"));

      ctx->flushCommandList(nullptr, cStatus);

      if (t) Logger::warn(str::format("  worker chunk ", s_exec, ": flushed"));
    });

    // Transport test, temporary. Fill the shared pages with a byte the
    // GPU would never produce, then read them back after the copy. If
    // the poison survives, nothing wrote to the memory being read and
    // the copy is landing elsewhere. If it comes back zeroed, the GPU
    // did write, and it wrote zeroes.
    bool poison = env::getEnvVar("DXVK_MGPU_TEST_POISON") == "1";

    if (poison && m_sharedHostMem) {
      static uint32_t s_poisonOnce = 0;

      if (s_poisonOnce++ < 3u)
        std::memset(m_sharedHostMem, 0xAB, size_t(imageBytes));
    }

    uint64_t seq = m_workerCsThread->dispatchChunk(std::move(download));

    static uint32_t s_phase = 0;
    bool trace = (s_phase++ < 10u);

    if (trace) Logger::warn(str::format("composite ", s_phase, ": dispatched seq=", seq));

    auto t0 = dxvk::high_resolution_clock::now();
    m_workerCsThread->synchronize(seq);
    auto t1 = dxvk::high_resolution_clock::now();

    if (trace) Logger::warn(str::format("composite ", s_phase, ": synchronized"));

    workerDevice->waitForSubmission(&submitStatus);
    auto t2 = dxvk::high_resolution_clock::now();
    if (trace) Logger::warn(str::format("composite ", s_phase, ": submission done"));
    // waitForSubmission only waits for the submission thread to hand
    // the command list to the queue, not for GPU1 to execute it. The
    // shared block is read on the CPU and by GPU0 immediately after
    // this point, so the copy into it has to have actually completed.
    // Wait on the buffer rather than the device: waitForIdle would
    // also stall on every unrelated worker submission queued behind it.
    if (!m_noCompletionWait)
      workerDevice->waitForResource(*m_workerStaging, DxvkAccess::Write);
    auto t2b = dxvk::high_resolution_clock::now();
    if (trace) Logger::warn(str::format("composite ", s_phase, ": gpu complete"));
    {
      static uint32_t s_waitLog = 0;
      uint64_t waitUs = std::chrono::duration_cast<std::chrono::microseconds>(t2b - t2).count();
      if ((s_waitLog++ % 600u) == 0u)
        Logger::warn(str::format("COMPLETIONWAIT ", waitUs, " us"));
    }
    uint32_t cbReadIndex = m_cbReadValid ? m_cbReadCount++ : 0u;
    if (m_cbReadEnabled && m_cbReadValid
     && (cbReadIndex < 12u || (cbReadIndex % 30u) == 0u)) {
      const uint8_t* wp = reinterpret_cast<const uint8_t*>(
        m_cbReadBuffer->mapPtr(0));
      const uint8_t* pp = reinterpret_cast<const uint8_t*>(
        m_cbReadPrimary->mapPtr(0));
      if (wp && pp) {
        wp += m_cbReadOffset;
        pp += m_cbReadPrimaryOffset;
        std::string got;
        std::string want;
        for (uint32_t i = 0; i < 16u; i++) {
          got  += str::format(uint32_t(wp[i]), " ");
          want += str::format(uint32_t(pp[i]), " ");
        }
        bool ok = std::memcmp(wp, pp, 16u) == 0;
        Logger::warn(str::format("CBREAD ", ok ? "match" : "MISMATCH",
          " n=", cbReadIndex,
          " workerMap=", m_cbReadBuffer->mapPtr(0),
          " primaryMap=", m_cbReadPrimary->mapPtr(0),
          " worker=[ ", got, "] primary=[ ", want, "]"));
      } else {
        Logger::warn(str::format("CBREAD skipped: mapPtr worker=",
          wp != nullptr, " primary=", pp != nullptr));
      }
    }
    if (m_cbPoisonEnabled && m_cbReadValid) {
      uint8_t* poisonPtr = reinterpret_cast<uint8_t*>(m_cbReadBuffer->mapPtr(0));
      if (poisonPtr)
        std::memset(poisonPtr + m_cbReadOffset, 0xFF, 16u);
    }

    // Opt-in: sample a few pixels of what GPU1 actually produced. The
    // data is already in shared host memory here, so the only cost is
    // the read and the log line. Several positions rather than one, so
    // a cleared background can be told apart from geometry covering
    // part of the frame. Enable with DXVK_MGPU_DEBUG_DRAW=1.
    if (poison && m_sharedHostMem) {
      static uint32_t s_poison = 0;

      if (s_poison++ < 8u) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(m_sharedHostMem);
        size_t mid = size_t(imageBytes) / 2u;

        Logger::warn(str::format("POISON first=", uint32_t(p[0]),
          " mid=", uint32_t(p[mid]),
          " last=", uint32_t(p[size_t(imageBytes) - 1u])));
      }
    }

    if (unlikely(m_workerDrawDebug)) {
      static uint32_t s_readbackCount = 0;

      if ((s_readbackCount++ % 30u) == 0u && m_sharedHostMem) {
        const uint8_t* px = reinterpret_cast<const uint8_t*>(m_sharedHostMem);
        uint32_t w = extent.width;
        uint32_t h = extent.height;

        auto sample = [px, w] (uint32_t x, uint32_t y) {
          size_t o = (size_t(y) * w + x) * 4u;
          return str::format("[", uint32_t(px[o]), ",", uint32_t(px[o + 1]),
            ",", uint32_t(px[o + 2]), ",", uint32_t(px[o + 3]), "]");
        };

        Logger::info(str::format("worker image ", w, "x", h,
          " centre ", sample(w / 2u, h / 2u),
          " topleft ", sample(w / 8u, h / 8u),
          " corner ", sample(2u, 2u)));
      }
    }

    if (m_sharedHostMem && env::getEnvVar("DXVK_MGPU_TEST_IMGPOISON") == "1")
      std::memset(m_sharedHostMem, 0xAB, size_t(imageBytes));
    {
      void MgpuDumpCallCounters();
      static uint32_t s_censusTick = 0u;
      if ((++s_censusTick % 3000u) == 0u)
        MgpuDumpCallCounters();
    }
    // No copy here any more: both devices are bound to the same host pages,
    // so whatever GPU1 wrote is already visible to GPU0.
    uint64_t syncUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    uint64_t subUs  = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
    uint64_t waitUs = std::chrono::duration_cast<std::chrono::microseconds>(t2b - t2).count();
    {
      static uint64_t s_accSync = 0, s_accSub = 0, s_accWait = 0;
      static uint32_t s_accN = 0;
      s_accSync += syncUs; s_accSub += subUs; s_accWait += waitUs;
      if ((++s_accN % 600u) == 0u) {
        Logger::warn(str::format("COMPOSITEPHASES avg over 600: cs sync ", s_accSync / 600u,
          " us, submit ", s_accSub / 600u, " us, gpu wait ", s_accWait / 600u, " us"));
        s_accSync = 0; s_accSub = 0; s_accWait = 0;
      }
    }
    m_compositeCount++;
    m_mapFrames++;
    m_compositeSyncSum += syncUs;
    m_compositeSyncMin  = std::min(m_compositeSyncMin, syncUs);
    m_compositeSyncMax  = std::max(m_compositeSyncMax, syncUs);
    m_compositeSubSum  += subUs;
    m_compositeSubMin   = std::min(m_compositeSubMin, subUs);
    m_compositeSubMax   = std::max(m_compositeSubMax, subUs);

    if (m_compositeSyncLog.size() < MaxCompositeSamples) {
      m_compositeSyncLog.push_back(uint32_t(syncUs));
      m_compositeSubLog.push_back(uint32_t(subUs));
    }

    // GPU0: primary host buffer -> the D3D11 back buffer image.
    //
    // First step towards selective compositing: copy only the bottom
    // half of GPU1's output instead of the whole frame. Everything
    // GPU0 rendered in the top half survives, which is directly
    // visible in Superposition -- its UI is currently erased by the
    // wholesale overwrite. Generalises to checkerboard tiles once the
    // partial copy is proven; a single strip is far easier to confirm
    // by eye than a tile pattern.
    if (extentMismatch)
      return;

    bool splitComposite = env::getEnvVar("DXVK_MGPU_NO_SPLIT") != "1";

    EmitCs([
      cImage  = DstImage,
      cBuffer = m_primaryStaging,
      cExtent = extent,
      cSplit  = splitComposite
    ] (DxvkContext* ctx) {
      VkImageSubresourceLayers sub = { };
      sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      sub.layerCount = 1;

      if (!cSplit) {
        ctx->copyBufferToImage(cImage, sub,
          VkOffset3D{ 0, 0, 0 }, cExtent,
          cBuffer, 0, 0, 0,
          VK_FORMAT_R8G8B8A8_UNORM);
        return;
      }

      uint32_t splitY = cExtent.height / 2u;

      VkExtent3D half = { cExtent.width, cExtent.height - splitY, 1u };
      VkDeviceSize srcOffset = VkDeviceSize(splitY) * cExtent.width * 4u;

      ctx->copyBufferToImage(cImage, sub,
        VkOffset3D{ 0, int32_t(splitY), 0 }, half,
        cBuffer, srcOffset, 0, 0,
        VK_FORMAT_R8G8B8A8_UNORM);
    });
  }

  void D3D11ImmediateContext::TrackTextureSequenceNumber(
          D3D11CommonTexture*         pResource,
          UINT                        Subresource) {
    uint64_t sequenceNumber = GetCurrentSequenceNumber();
    pResource->TrackSequenceNumber(Subresource, sequenceNumber);

    ConsiderFlush(GpuFlushType::ImplicitStrongHint);
  }


  void D3D11ImmediateContext::TrackBufferSequenceNumber(
          D3D11Buffer*                pResource) {
    uint64_t sequenceNumber = GetCurrentSequenceNumber();
    pResource->TrackSequenceNumber(sequenceNumber);

    ConsiderFlush(GpuFlushType::ImplicitStrongHint);
  }


  uint64_t D3D11ImmediateContext::GetCurrentSequenceNumber() {
    // We do not flush empty chunks, so if we are tracking a resource
    // immediately after a flush, we need to use the sequence number
    // of the previously submitted chunk to prevent deadlocks.
    return m_csChunk->empty() ? m_csSeqNum : m_csSeqNum + 1;
  }


  uint64_t D3D11ImmediateContext::GetPendingCsChunks() {
    return GetCurrentSequenceNumber() - m_flushSeqNum;
  }


  void D3D11ImmediateContext::ApplyDirtyNullBindings() {
    // At the end of a submission, set all bindings that have not been applied yet
    // to null on the DXVK context. This way, we avoid keeping resources alive that
    // are bound to the DXVK context but not to the immediate context.
    //
    // Note: This requires that all methods that may modify dirty bindings on the
    // DXVK context also reset the corresponding dirty bits *before* performing the
    // bind operation, or otherwise an implicit flush can potentially override them.
    auto& dirtyState = m_state.lazy.bindingsDirty;

    EmitCs<false>([
      cDirtyState = dirtyState
    ] (DxvkContext* ctx) {
      for (uint32_t i = 0; i < D3D11ShaderTypeCount; i++) {
        auto dxStage = D3D11ShaderType(i);
        auto vkStage = GetShaderStage(dxStage);

        // Unbind all dirty constant buffers
        auto cbvSlot = D3D11ShaderResourceMapping::computeCbvBinding(dxStage, 0);

        for (uint32_t index : bit::BitMask(cDirtyState[dxStage].cbvMask))
          ctx->bindUniformBuffer(vkStage, cbvSlot + index, DxvkBufferSlice());

        // Unbind all dirty samplers
        auto samplerSlot = D3D11ShaderResourceMapping::computeSamplerBinding(dxStage, 0);

        for (uint32_t index : bit::BitMask(cDirtyState[dxStage].samplerMask))
          ctx->bindResourceSampler(vkStage, samplerSlot + index, nullptr);

        // Unbind all dirty shader resource views
        auto srvSlot = D3D11ShaderResourceMapping::computeSrvBinding(dxStage, 0);

        for (uint32_t m = 0; m < cDirtyState[dxStage].srvMask.size(); m++) {
          for (uint32_t index : bit::BitMask(cDirtyState[dxStage].srvMask[m]))
            ctx->bindResourceImageView(vkStage, srvSlot + index + m * 64u, nullptr);
        }

        // Unbind all dirty unordered access views
        VkShaderStageFlags uavStages = 0u;

        if (dxStage == D3D11ShaderType::eCompute)
          uavStages = VK_SHADER_STAGE_COMPUTE_BIT;
        else if (dxStage == D3D11ShaderType::ePixel)
          uavStages = VK_SHADER_STAGE_ALL_GRAPHICS;

        if (uavStages) {
          auto uavSlot = D3D11ShaderResourceMapping::computeUavBinding(dxStage, 0);
          auto ctrSlot = D3D11ShaderResourceMapping::computeUavCounterBinding(dxStage, 0);

          for (uint32_t index : bit::BitMask(cDirtyState[dxStage].uavMask)) {
            ctx->bindResourceImageView(vkStage, uavSlot + index, nullptr);
            ctx->bindResourceBufferView(vkStage, ctrSlot + index, nullptr);
          }
        }
      }
    });

    // Since we set the DXVK context bindings to null, any bindings that are null
    // on the D3D context are no longer dirty, so we can clear the respective bits.
    for (uint32_t i = 0; i < D3D11ShaderTypeCount; i++) {
      auto stage = D3D11ShaderType(i);

      for (uint32_t index : bit::BitMask(dirtyState[stage].cbvMask)) {
        if (!m_state.cbv[stage].buffers[index].buffer.ptr())
          dirtyState[stage].cbvMask &= ~(1u << index);
      }

      for (uint32_t index : bit::BitMask(dirtyState[stage].samplerMask)) {
        if (!m_state.samplers[stage].samplers[index])
          dirtyState[stage].samplerMask &= ~(1u << index);
      }

      for (uint32_t m = 0; m < dirtyState[stage].srvMask.size(); m++) {
        for (uint32_t index : bit::BitMask(dirtyState[stage].srvMask[m])) {
          if (!m_state.srv[stage].views[index + m * 64u].ptr())
            dirtyState[stage].srvMask[m] &= ~(uint64_t(1u) << index);
        }
      }

      if (stage == D3D11ShaderType::eCompute || stage == D3D11ShaderType::ePixel) {
        auto& uavs = stage == D3D11ShaderType::eCompute ? m_state.uav.views : m_state.om.uavs;

        for (uint32_t index : bit::BitMask(dirtyState[stage].uavMask)) {
          if (!uavs[index].ptr())
            dirtyState[stage].uavMask &= ~(uint64_t(1u) << index);
        }
      }

      if (dirtyState[stage].empty())
        m_state.lazy.shadersDirty.clr(stage);
    }
  }


  void D3D11ImmediateContext::ConsiderFlush(
          GpuFlushType                FlushType) {
    // In stress test mode, behave as if this would always flush
    if (DebugLazyBinding == Tristate::True)
      ApplyDirtyNullBindings();

    uint64_t chunkId = GetCurrentSequenceNumber();
    uint64_t submissionId = m_submissionFence->value();

    if (m_flushTracker.considerFlush(FlushType, chunkId, submissionId, m_estimatedCost))
      ExecuteFlush(FlushType, nullptr, false);
  }


  void D3D11ImmediateContext::ExecuteFlush(
          GpuFlushType                FlushType,
          HANDLE                      hEvent,
          BOOL                        Synchronize) {
    bool synchronizeSubmission = Synchronize && m_parent->Is11on12Device();

    if (synchronizeSubmission)
      m_submitStatus.result = VK_NOT_READY;

    // Exit early if there's nothing to do
    if (!GetPendingCsChunks() && !hEvent)
      return;

    m_hasPendingUnresolvedPass = false;

    // Unbind unused resources
    ApplyDirtyNullBindings();

    // Signal the submission fence and flush the command list
    uint64_t submissionId = ++m_submissionId;

    if (hEvent) {
      m_submissionFence->setCallback(submissionId, [hEvent] {
        SetEvent(hEvent);
      });
    }

    EmitCs<false>([
      cSubmissionFence  = m_submissionFence,
      cSubmissionId     = submissionId,
      cSubmissionStatus = synchronizeSubmission ? &m_submitStatus : nullptr,
      cStagingFence     = m_stagingBufferFence,
      cStagingMemory    = GetStagingMemoryStatistics().allocatedTotal,
      cFlushReason      = std::exchange(m_flushReason, std::string())
    ] (DxvkContext* ctx) {
      auto debugLabel = vk::makeLabel(0xff5959, cFlushReason.c_str());

      ctx->signal(cSubmissionFence, cSubmissionId);
      ctx->signal(cStagingFence, cStagingMemory);
      ctx->flushCommandList(&debugLabel, cSubmissionStatus);
    });

    FlushCsChunk();
    // Mirror the submission on the worker. Without this the worker
    // chunk only reaches GPU1 at the composite, so GPU1 idles for the
    // whole frame and then renders it from scratch while GPU0 waits
    // (afc3e16b: 16800 primary submissions against 3600 worker).
    if (m_workerCsThread && !m_noWorkerFlush) {
      EmitWorkerCs([] (DxvkContext* ctx) {
        ctx->flushCommandList(nullptr, nullptr);
      });
      FlushWorkerCsChunk();
    }
    // Notify flush tracker about the flush
    m_flushSeqNum = m_csSeqNum;
    m_flushTracker.notifyFlush(m_flushSeqNum, submissionId);

    // If necessary, block calling thread until the
    // Vulkan queue submission is performed.
    if (synchronizeSubmission)
      m_device->waitForSubmission(&m_submitStatus);

    // Free local staging buffer so that we don't
    // end up with a persistent allocation
    ResetStagingBuffer();

    // Reset counter for discarded memory in flight
    m_discardMemoryOnFlush = m_discardMemoryCounter;

    // Reset GPU execution cost estimate
    m_estimatedCost = 0u;

    // Notify the device that the context has been flushed,
    // this resets some resource initialization heuristics.
    m_parent->NotifyContextFlush();
  }


  void D3D11ImmediateContext::ThrottleAllocation() {
    DxvkStagingBufferStats stats = GetStagingMemoryStatistics();

    VkDeviceSize stagingMemoryInFlight = stats.allocatedTotal - m_stagingBufferFence->value();

    if (stagingMemoryInFlight > stats.allocatedSinceLastReset + D3D11Initializer::MaxMemoryInFlight) {
      // Stall calling thread to avoid situation where we keep growing the staging
      // buffer indefinitely, but ignore the newly allocated amount so that we don't
      // wait for the GPU to go fully idle in case of a large allocation.
      ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, false);

      m_device->waitForFence(*m_stagingBufferFence, stats.allocatedTotal -
        stats.allocatedSinceLastReset - D3D11Initializer::MaxMemoryInFlight);
    } else if (stats.allocatedSinceLastReset >= D3D11Initializer::MaxMemoryPerSubmission) {
      // Flush somewhat aggressively if there's a lot of memory in flight
      ExecuteFlush(GpuFlushType::ExplicitFlush, nullptr, false);
    }
  }


  void D3D11ImmediateContext::ThrottleDiscard(
          VkDeviceSize                Size) {
    m_discardMemoryCounter += Size;

    if (m_discardMemoryCounter - m_discardMemoryOnFlush >= D3D11Initializer::MaxMemoryPerSubmission)
      ThrottleAllocation();
  }


  void D3D11ImmediateContext::NotifyRenderPassBoundary(bool IsMultisampled) {
    // Doing this makes it less likely to flush during render passes
    ConsiderFlush(GpuFlushType::ImplicitWeakHint);

    if (m_device->perfHints().preferRenderPassOps) {
      // Make sure that explicit flushes get ignored until the next resolve
      m_hasPendingUnresolvedPass = m_hasPendingUnresolvedPass || IsMultisampled;

      // On tilers, we want to avoid submitting during a render pass or a sequence
      // of render passes as much as possible, but if a submission request has been
      // rejected before, we should do it now in order to avoid read-back delays.
      GpuFlushType pending = m_flushTracker.getPendingType();

      if (pending != GpuFlushType::None)
        ExecuteFlush(pending, nullptr, false);
    }
  }


  void D3D11ImmediateContext::NotifyResolve() {
    m_hasPendingUnresolvedPass = false;
  }


  void D3D11ImmediateContext::RequestFlush(
          D3D11_CONTEXT_TYPE          ContextType,
          HANDLE                      hEvent) {
    D3D10DeviceLock lock = LockContext();

    // If we're in tiler mode and a render pass hasn't been resolved yet,
    // ignore explicit flushes. This is a very dirty heuristic to work
    // around some Unity Engine performance issues.
    if (m_hasPendingUnresolvedPass && !m_parent->Is11on12Device())
      return;

    if (unlikely(m_device->debugFlags().test(DxvkDebugFlag::Capture)))
      m_flushReason = "Explicit Flush";

    ExecuteFlush(GpuFlushType::ExplicitFlush, hEvent, true);
  }


  DxvkStagingBufferStats D3D11ImmediateContext::GetStagingMemoryStatistics() {
    DxvkStagingBufferStats stats = m_staging.getStatistics();
    stats.allocatedTotal += m_discardMemoryCounter;
    stats.allocatedSinceLastReset += m_discardMemoryCounter - m_discardMemoryOnFlush;
    return stats;
  }


  GpuFlushType D3D11ImmediateContext::GetMaxFlushType(
          D3D11Device*    pParent,
    const Rc<DxvkDevice>& Device) {
    if (pParent->GetOptions()->reproducibleCommandStream)
      return GpuFlushType::ExplicitFlush;
    else if (Device->perfHints().preferRenderPassOps)
      return GpuFlushType::ImplicitStrongHint;
    else
      return GpuFlushType::ImplicitWeakHint;
  }

}

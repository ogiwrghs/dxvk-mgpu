#pragma once

#include "../dxvk/dxvk_cs.h"
#include "../dxvk/dxvk_device.h"

#include "../d3d10/d3d10_buffer.h"

#include "d3d11_device_child.h"
#include "d3d11_interfaces.h"
#include "d3d11_on_12.h"
#include "d3d11_resource.h"

namespace dxvk {
  
  class D3D11Device;


  /**
   * \brief Buffer map mode
   */
  enum D3D11_COMMON_BUFFER_MAP_MODE {
    D3D11_COMMON_BUFFER_MAP_MODE_NONE,
    D3D11_COMMON_BUFFER_MAP_MODE_DIRECT,
  };


  /**
   * \brief Stream output buffer offset
   *
   * A byte offset into the buffer that
   * stores the byte offset where new
   * data will be written to.
   */
  struct D3D11SOCounter {
    uint32_t byteOffset;
  };
  
  
  struct D3D11ShadowRange {
    VkDeviceSize begin;
    VkDeviceSize end;
  };

  class D3D11Buffer : public D3D11DeviceChild<ID3D11Buffer> {
    static constexpr VkDeviceSize BufferSliceAlignment = 64;
  public:
    
    D3D11Buffer(
            D3D11Device*                pDevice,
      const D3D11_BUFFER_DESC*          pDesc,
      const D3D11_ON_12_RESOURCE_INFO*  p11on12Info);

    ~D3D11Buffer();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject) final;
    
    void STDMETHODCALLTYPE GetType(
            D3D11_RESOURCE_DIMENSION *pResourceDimension) final;
    
    UINT STDMETHODCALLTYPE GetEvictionPriority() final;
    
    void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) final;
    
    void STDMETHODCALLTYPE GetDesc(
            D3D11_BUFFER_DESC *pDesc) final;
    
    void STDMETHODCALLTYPE SetDebugName(const char* pName) final;

    bool CheckViewCompatibility(
            UINT                BindFlags,
            DXGI_FORMAT         Format) const;

    const D3D11_BUFFER_DESC* Desc() const {
      return &m_desc;
    }

    BOOL IsTilePool() const {
      return bool(m_desc.MiscFlags & D3D11_RESOURCE_MISC_TILE_POOL);
    }

    D3D11_COMMON_BUFFER_MAP_MODE GetMapMode() const {
      return m_mapMode;
    }

    uint64_t GetCookie() const {
      return m_cookie;
    }

    Rc<DxvkBuffer> GetBuffer() const {
      return m_buffer;
    }

    Rc<DxvkBuffer> GetWorkerBuffer() const {
      return m_workerBuffer;
    }

    // Worker-device counterpart of GetBufferSlice(). Returns an empty
    // slice when this buffer has no mirror -- imported and sparse
    // buffers are deliberately not mirrored (see D3D11Buffer's
    // constructor), so callers must tolerate a null worker side rather
    // than assume every D3D11Buffer has one.
    DxvkBufferSlice GetWorkerBufferSlice() const {
      return m_workerBuffer != nullptr
        ? DxvkBufferSlice(m_workerBuffer, 0, m_desc.ByteWidth)
        : DxvkBufferSlice();
    }

    DxvkBufferSlice GetWorkerBufferSlice(VkDeviceSize offset) const {
      if (m_workerBuffer == nullptr)
        return DxvkBufferSlice();

      VkDeviceSize size = m_desc.ByteWidth;
      offset = std::min(offset, size);
      return DxvkBufferSlice(m_workerBuffer, offset, size - offset);
    }

    DxvkBufferSlice GetWorkerBufferSlice(VkDeviceSize offset, VkDeviceSize length) const {
      if (m_workerBuffer == nullptr)
        return DxvkBufferSlice();

      VkDeviceSize size = m_desc.ByteWidth;
      offset = std::min(offset, size);
      return DxvkBufferSlice(m_workerBuffer, offset, std::min(length, size - offset));
    }

    Rc<DxvkSparsePageAllocator> GetSparseAllocator() const {
      return m_sparseAllocator;
    }
    
    DxvkBufferSlice GetBufferSlice() const {
      return DxvkBufferSlice(m_buffer, 0, m_desc.ByteWidth);
    }
    
    DxvkBufferSlice GetBufferSlice(VkDeviceSize offset) const {
      VkDeviceSize size = m_desc.ByteWidth;
      offset = std::min(offset, size);
      return DxvkBufferSlice(m_buffer, offset, size - offset);
    }
    
    DxvkBufferSlice GetBufferSlice(VkDeviceSize offset, VkDeviceSize length) const {
      VkDeviceSize size = m_desc.ByteWidth;
      offset = std::min(offset, size);
      return DxvkBufferSlice(m_buffer, offset, std::min(length, size - offset));
    }

    VkDeviceSize GetRemainingSize(VkDeviceSize offset) const {
      VkDeviceSize size = m_desc.ByteWidth;
      offset = std::min(offset, size);
      return size - offset;
    }

    DxvkBufferSlice GetSOCounter() {
      return m_soCounter != nullptr
        ? DxvkBufferSlice(m_soCounter)
        : DxvkBufferSlice();
    }
    
    Rc<DxvkResourceAllocation> AllocSlice(DxvkLocalAllocationCache* cache) {
      return m_buffer->allocateStorage(cache);
    }
    
    Rc<DxvkResourceAllocation> DiscardSlice(DxvkLocalAllocationCache* cache) {
      auto allocation = m_buffer->allocateStorage(cache);
      m_mapPtr = allocation->mapPtr();
      return allocation;
    }

     void* GetMapPtr() const {
      return m_mapPtr;
    }

    void* GetShadowPtr() const {
      return m_shadowPtr;
    }

    bool EnsureShadow();

    void DropShadow();

    void CollectShadowDirtyRanges(std::vector<D3D11ShadowRange>& ranges);

    D3D10Buffer* GetD3D10Iface() {
      return &m_d3d10;
    }

    bool HasSequenceNumber() const {
      return m_mapMode != D3D11_COMMON_BUFFER_MAP_MODE_NONE
          && !(m_desc.MiscFlags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS)
          && !(m_desc.BindFlags);
    }

    void TrackSequenceNumber(uint64_t Seq) {
      m_seq = Seq;
    }

    uint64_t GetSequenceNumber() {
      return HasSequenceNumber() ? m_seq
        : DxvkCsThread::SynchronizeAll;
    }

    /**
     * \brief Retrieves D3D11on12 resource info
     * \returns 11on12 resource info
     */
    D3D11_ON_12_RESOURCE_INFO Get11on12Info() const {
      return m_11on12;
    }

    /**
     * \brief Normalizes buffer description
     * 
     * \param [in] pDesc Buffer description
     * \returns \c S_OK if the parameters are valid
     */
    static HRESULT NormalizeBufferProperties(
            D3D11_BUFFER_DESC*      pDesc);

    /**
     * \brief Initializes D3D11 buffer description from D3D12
     *
     * \param [in] pResource D3D12 resource
     * \param [in] pResourceFlags D3D11 flag overrides
     * \param [out] pBufferDesc D3D11 buffer description
     * \returns \c S_OK if the parameters are valid
     */
    static HRESULT GetDescFromD3D12(
            ID3D12Resource*         pResource,
      const D3D11_RESOURCE_FLAGS*   pResourceFlags,
            D3D11_BUFFER_DESC*      pBufferDesc);

    /**
     * \brief A live buffer with storage on both devices
     *
     * Diagnostic, for the buffer dump. Holds references to both
     * buffers, so the pair stays valid after the D3D11 buffer
     * that owned it is destroyed.
     */
    struct WorkerBufferPair {
      uint32_t        index;
      UINT            bindFlags;
      D3D11_USAGE     usage;
      UINT            cpuFlags;
      UINT            miscFlags;
      VkDeviceSize    size;
      Rc<DxvkBuffer>  primary;
      Rc<DxvkBuffer>  worker;
    };

    /**
     * \brief Lists every live buffer that has a worker copy
     *
     * Sorted by creation index.
     */
    static std::vector<WorkerBufferPair> ListWorkerBuffers();

  private:
    
    D3D11_BUFFER_DESC             m_desc;
    D3D11_ON_12_RESOURCE_INFO     m_11on12;
    D3D11_COMMON_BUFFER_MAP_MODE  m_mapMode;
    
    Rc<DxvkBuffer>                m_buffer;
    Rc<DxvkBuffer>                m_workerBuffer;
    uint32_t                      m_workerIndex = 0u;
    uint64_t                      m_cookie = 0u;

    Rc<DxvkBuffer>                m_soCounter;
    Rc<DxvkSparsePageAllocator>   m_sparseAllocator;
    uint64_t                      m_seq = 0ull;

    void*                         m_mapPtr = nullptr;
    void*                         m_shadowPtr = nullptr;
    std::vector<void*>            m_shadowPages;

    D3D11DXGIResource             m_resource;
    D3D10Buffer                   m_d3d10;

    D3DDestructionNotifier        m_destructionNotifier;

    BOOL CheckFormatFeatureSupport(
            VkFormat              Format,
            VkFormatFeatureFlags2 Features) const;
    
    VkMemoryPropertyFlags GetMemoryFlags() const;

    Rc<DxvkBuffer> CreateSoCounterBuffer();

    static D3D11_COMMON_BUFFER_MAP_MODE DetermineMapMode(
            VkMemoryPropertyFlags MemFlags);

  };


  /**
   * \brief Retrieves buffer from resource pointer
   * 
   * \param [in] pResource The resource to query
   * \returns Pointer to buffer, or \c nullptr
   */
  D3D11Buffer* GetCommonBuffer(
          ID3D11Resource*       pResource);
  
}

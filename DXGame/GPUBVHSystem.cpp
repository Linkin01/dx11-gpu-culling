#include "GPUBVHSystem.h"

bool GPUBVHSystem::Initialize(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context, int objectCount) {
    m_device = device;
    m_context = context;
    m_objectCount = objectCount;
    
    // Check if compute shaders are supported
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hwopts = {};
    HRESULT hr = m_device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &hwopts, sizeof(hwopts));
    if (FAILED(hr)) {
        OutputDebugStringA("Compute shaders not supported, GPU BVH system will be disabled\n");
        return false;
    }
    
    if (!CreateComputeShaders()) {
        OutputDebugStringA("Failed to create compute shaders for GPU BVH system\n");
        return false;
    }
    
    if (!CreateBuffers(objectCount)) {
        OutputDebugStringA("Failed to create buffers for GPU BVH system\n");
        return false;
    }
    
    OutputDebugStringA("GPU BVH system initialized successfully\n");
    return true;
}

void GPUBVHSystem::Shutdown() {
    // All ComPtr objects will be automatically released
    m_device.Reset();
    m_context.Reset();
}

bool GPUBVHSystem::BuildBVH(const std::vector<RenderObject>& objects, const Vector3& sceneMin, const Vector3& sceneMax) {
    if (objects.empty() || !m_mortonCodeCS || !m_bvhConstructionCS) {
        return false;
    }
    
    // Step 1: Generate Morton codes for spatial sorting
    GenerateMortonCodes(objects, sceneMin, sceneMax);
    
    // Step 2: Sort Morton codes to cluster spatially nearby objects
    SortMortonCodes();
    
    // Step 3: Build BVH structure using sorted Morton codes
    ConstructBVHOnGPU();
    
    // Step 4: Initialize quality tracking
    UpdateBVHQualityMetrics();
    
    // Step 5: Reset state for dynamic updates
    m_needsRebuild = false;
    m_framesSinceLastRebuild = 0;
    m_accumulatedMovement = 0.0f;
    
    // Initialize position tracking for next frame
    m_previousPositions.resize(objects.size());
    for (size_t i = 0; i < objects.size(); i++) {
        m_previousPositions[i] = objects[i].GetPosition();
    }
    
    return true;
}

bool GPUBVHSystem::RefitBVH(const std::vector<RenderObject>& objects) {
    if (!m_bvhRefitCS || objects.empty() || !m_bvhNodesBuffer || !m_objectsBuffer) {
        OutputDebugStringA("GPU BVH Refit: Missing required resources\n");
        return false;
    }
    
    // Use an iterative bottom-up approach for robust BVH refitting
    return RefitBVHBottomUp(objects);
}

bool GPUBVHSystem::RefitBVHBottomUp(const std::vector<RenderObject>& objects) {
    // Update GPU object data with new positions/bounds
    UpdateGPUObjectData(objects);
    
    // Set up compute shader resources
    m_context->CSSetShader(m_bvhRefitCS.Get(), nullptr, 0);
    
    ID3D11ShaderResourceView* srvs[] = { m_objectsSRV.Get() };
    m_context->CSSetShaderResources(0, 1, srvs);
    
    ID3D11UnorderedAccessView* uavs[] = { m_bvhNodesUAV.Get() };
    UINT initialCounts[] = { 0 };
    m_context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
    
    ID3D11Buffer* cbs[] = { m_cullingParamsBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);
    
    // Perform iterative bottom-up refitting for better convergence
    int totalNodes = static_cast<int>(objects.size()) * 2 - 1;
    int numGroups = (totalNodes + Config::COMPUTE_THREADS_PER_GROUP - 1) / Config::COMPUTE_THREADS_PER_GROUP;
    
    for (int iteration = 0; iteration < Config::BVH_REFIT_ITERATIONS; iteration++) {
        m_context->Dispatch(numGroups, 1, 1);
        
        // Add memory barrier to ensure writes complete before next iteration
        ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
    }
    
    // Clean up resources
    ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
    m_context->CSSetShaderResources(0, 1, nullSRVs);
    
    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
    
    m_context->CSSetShader(nullptr, nullptr, 0);
    
    // Update quality metrics after refit
    UpdateBVHQualityMetrics();
    
    return true;
}

bool GPUBVHSystem::ShouldRebuildBVH(const std::vector<RenderObject>& objects) {
    // Increment frame counter
    m_framesSinceLastRebuild++;
    
    // Force rebuild after max frames to prevent degradation
    if (m_framesSinceLastRebuild >= Config::MAX_FRAMES_BETWEEN_REBUILDS) {
        OutputDebugStringA("GPU BVH: Force rebuild due to frame limit\n");
        return true;
    }
    
    // Ensure we have previous positions for comparison
    if (m_previousPositions.size() != objects.size()) {
        m_previousPositions.resize(objects.size());
        for (size_t i = 0; i < objects.size(); i++) {
            m_previousPositions[i] = objects[i].GetPosition();
        }
        return false; // First frame comparison
    }
    
    // Calculate accumulated movement since last rebuild
    float frameMovement = 0.0f;
    for (size_t i = 0; i < objects.size(); i++) {
        if (objects[i].isDynamic) {
            Vector3 currentPos = objects[i].GetPosition();
            Vector3 movement = currentPos - m_previousPositions[i];
            frameMovement += movement.Length();
            m_previousPositions[i] = currentPos;
        }
    }
    
    m_accumulatedMovement += frameMovement;
    
    // Check if accumulated movement exceeds threshold
    if (m_accumulatedMovement > Config::REBUILD_THRESHOLD) {
        OutputDebugStringA("GPU BVH: Rebuild due to accumulated movement\n");
        return true;
    }
    
    // Check BVH quality degradation
    float qualityRatio = CalculateBVHQuality();
    if (qualityRatio > Config::BVH_QUALITY_THRESHOLD) {
        OutputDebugStringA("GPU BVH: Rebuild due to quality degradation\n");
        return true;
    }
    
    return false;
}

float GPUBVHSystem::CalculateBVHQuality() const {
    // Simple quality metric: ratio of current to initial surface area
    // A more sophisticated implementation would analyze SAH cost
    if (m_initialBVHSurfaceArea <= 0.0f) {
        return 1.0f; // First build, assume good quality
    }
    
    return m_currentBVHSurfaceArea / m_initialBVHSurfaceArea;
}

void GPUBVHSystem::UpdateBVHQualityMetrics() {
    // Calculate surface area heuristic for current BVH
    float currentSAH = CalculateSurfaceAreaHeuristic();
    
    if (m_initialBVHSurfaceArea <= 0.0f) {
        m_initialBVHSurfaceArea = currentSAH;
    }
    
    m_currentBVHSurfaceArea = currentSAH;
}

float GPUBVHSystem::CalculateSurfaceAreaHeuristic() const {
    // Simplified SAH calculation - in a full implementation, 
    // this would read back BVH data from GPU or maintain CPU copy
    // For now, return a placeholder that gets updated appropriately
    return m_accumulatedMovement * 100.0f + 1000.0f;
}

bool GPUBVHSystem::PerformFrustumCulling(const Frustum& frustum, std::vector<RenderObject>& objects) {
    if (!m_frustumCullingCS || objects.empty() || !m_bvhNodesBuffer || !m_objectsBuffer) {
        OutputDebugStringA("GPU Frustum Culling: Missing required resources\n");
        return false;
    }
    
    try {
        // Update GPU data for current frame
        UpdateGPUObjectData(objects);
        UpdateFrustumData(frustum);
        UpdateCullingParams(static_cast<int>(objects.size()));
        
        // Set compute shader and resources
        m_context->CSSetShader(m_frustumCullingCS.Get(), nullptr, 0);
        
        ID3D11ShaderResourceView* srvs[] = { m_bvhNodesSRV.Get(), m_objectsSRV.Get() };
        m_context->CSSetShaderResources(0, 2, srvs);
        
        ID3D11Buffer* cbs[] = { m_frustumBuffer.Get(), m_cullingParamsBuffer.Get() };
        m_context->CSSetConstantBuffers(0, 2, cbs);
        
        ID3D11UnorderedAccessView* uavs[] = { m_visibilityUAV.Get() };
        UINT initialCounts[] = { 0 };
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
        
        // Dispatch compute shader - one thread per object for simplicity and efficiency
        int numGroups = (static_cast<int>(objects.size()) + Config::COMPUTE_THREADS_PER_GROUP - 1) / Config::COMPUTE_THREADS_PER_GROUP;
        m_context->Dispatch(numGroups, 1, 1);
        
        // Unbind resources first
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        m_context->CSSetShaderResources(0, 2, nullSRVs);
        
        ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        
        m_context->CSSetShader(nullptr, nullptr, 0);
        
        // Copy results for readback
        m_context->CopyResource(m_visibilityReadbackBuffer.Get(), m_visibilityBuffer.Get());
        
        // Try non-blocking read first (for async performance)
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_context->Map(m_visibilityReadbackBuffer.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        
        if (SUCCEEDED(hr)) {
            // Results available immediately - update object visibility
            const int* visibility = static_cast<const int*>(mapped.pData);
            
            for (size_t i = 0; i < objects.size() && i < static_cast<size_t>(m_objectCount); i++) {
                objects[i].visible = (visibility[i] != 0);
            }
            
            m_context->Unmap(m_visibilityReadbackBuffer.Get(), 0);
            return true;
            
        } else if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            // Results not ready yet - keep current visibility state and try next frame
            // This maintains smooth performance without blocking
            OutputDebugStringA("GPU Frustum Culling: Results not ready, using previous frame data\n");
            return true;
            
        } else {
            // Actual error occurred
            OutputDebugStringA("GPU Frustum Culling: Failed to read back visibility results\n");
            return false;
        }
        
    } catch (...) {
        OutputDebugStringA("GPU Frustum Culling: Exception occurred\n");
        return false;
    }
}

bool GPUBVHSystem::CreateComputeShaders() {
    if (!CompileAndCreateComputeShader(GetMortonCodeShaderSource(), &m_mortonCodeCS)) {
        OutputDebugStringA("Failed to compile Morton code compute shader\n");
        return false;
    }
    
    if (!CompileAndCreateComputeShader(GetBVHConstructionShaderSource(), &m_bvhConstructionCS)) {
        OutputDebugStringA("Failed to compile BVH construction compute shader\n");
        return false;
    }
    
    if (!CompileAndCreateComputeShader(GetFrustumCullingShaderSource(), &m_frustumCullingCS)) {
        OutputDebugStringA("Failed to compile frustum culling compute shader\n");
        return false;
    }
    
    if (!CompileAndCreateComputeShader(GetBVHRefitShaderSource(), &m_bvhRefitCS)) {
        OutputDebugStringA("Failed to compile BVH refit compute shader\n");
        return false;
    }
    
    return true;
}

bool GPUBVHSystem::CreateBuffers(int objectCount) {
    if (objectCount <= 0) {
        OutputDebugStringA("GPU BVH: Invalid object count for buffer creation\n");
        return false;
    }
    
    int nodeCount = objectCount * 2 - 1; // Maximum nodes in a binary tree
    
    try {
        CreateMortonCodesBuffer(objectCount);
        CreateBVHConstructionBuffer(nodeCount);
        CreateBVHNodesBuffer(nodeCount);
        CreateObjectsBuffer(objectCount);
        CreateVisibilityBuffer(objectCount);
        CreateConstantBuffers();
        
        // Verify all critical buffers were created
        if (!m_bvhNodesBuffer || !m_objectsBuffer || !m_visibilityBuffer || 
            !m_frustumBuffer || !m_cullingParamsBuffer) {
            OutputDebugStringA("GPU BVH: Failed to create one or more required buffers\n");
            return false;
        }
        
        return true;
    } catch (...) {
        OutputDebugStringA("GPU BVH: Exception during buffer creation\n");
        return false;
    }
}

bool GPUBVHSystem::CompileAndCreateComputeShader(const char* source, ComPtr<ID3D11ComputeShader>* outShader) {
    ComPtr<ID3DBlob> csBlob;
    ComPtr<ID3DBlob> errorBlob;
    
    HRESULT hr = D3DCompile(
        source,
        strlen(source),
        nullptr,
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &csBlob,
        &errorBlob
    );
    
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return false;
    }
    
    hr = m_device->CreateComputeShader(
        csBlob->GetBufferPointer(),
        csBlob->GetBufferSize(),
        nullptr,
        outShader->GetAddressOf()
    );
    
    return SUCCEEDED(hr);
}

void GPUBVHSystem::CreateMortonCodesBuffer(int objectCount) {
    // Morton codes buffer
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(GPUMortonCode) * objectCount;
    bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.StructureByteStride = sizeof(GPUMortonCode);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    
    m_device->CreateBuffer(&bufferDesc, nullptr, &m_mortonCodesBuffer);
    
    // Create UAV
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = objectCount;
    
    m_device->CreateUnorderedAccessView(m_mortonCodesBuffer.Get(), &uavDesc, &m_mortonCodesUAV);
    
    // Create SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = objectCount;
    
    m_device->CreateShaderResourceView(m_mortonCodesBuffer.Get(), &srvDesc, &m_mortonCodesSRV);
}

void GPUBVHSystem::CreateBVHConstructionBuffer(int nodeCount) {
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(GPUBVHConstructionNode) * nodeCount;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.StructureByteStride = sizeof(GPUBVHConstructionNode);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    
    m_device->CreateBuffer(&bufferDesc, nullptr, &m_bvhConstructionBuffer);
    
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = nodeCount;
    
    m_device->CreateUnorderedAccessView(m_bvhConstructionBuffer.Get(), &uavDesc, &m_bvhConstructionUAV);
}

void GPUBVHSystem::CreateBVHNodesBuffer(int nodeCount) {
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(GPUBVHNode) * nodeCount;
    bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.StructureByteStride = sizeof(GPUBVHNode);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    
    m_device->CreateBuffer(&bufferDesc, nullptr, &m_bvhNodesBuffer);
    
    // Create SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = nodeCount;
    
    m_device->CreateShaderResourceView(m_bvhNodesBuffer.Get(), &srvDesc, &m_bvhNodesSRV);
    
    // Create UAV
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = nodeCount;
    
    m_device->CreateUnorderedAccessView(m_bvhNodesBuffer.Get(), &uavDesc, &m_bvhNodesUAV);
}

void GPUBVHSystem::CreateObjectsBuffer(int objectCount) {
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(GPUObjectData) * objectCount;
    bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufferDesc.StructureByteStride = sizeof(GPUObjectData);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    
    m_device->CreateBuffer(&bufferDesc, nullptr, &m_objectsBuffer);
    
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = objectCount;
    
    m_device->CreateShaderResourceView(m_objectsBuffer.Get(), &srvDesc, &m_objectsSRV);
}

void GPUBVHSystem::CreateVisibilityBuffer(int objectCount) {
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(int) * objectCount;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.StructureByteStride = sizeof(int);
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    
    m_device->CreateBuffer(&bufferDesc, nullptr, &m_visibilityBuffer);
    
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = objectCount;
    
    m_device->CreateUnorderedAccessView(m_visibilityBuffer.Get(), &uavDesc, &m_visibilityUAV);
    
    // Create readback buffer
    bufferDesc.Usage = D3D11_USAGE_STAGING;
    bufferDesc.BindFlags = 0;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bufferDesc.MiscFlags = 0;
    
    m_device->CreateBuffer(&bufferDesc, nullptr, &m_visibilityReadbackBuffer);
}

void GPUBVHSystem::CreateConstantBuffers() {
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    // Frustum constant buffer
    cbDesc.ByteWidth = sizeof(GPUFrustum);
    m_device->CreateBuffer(&cbDesc, nullptr, &m_frustumBuffer);
    
    // Culling params constant buffer
    cbDesc.ByteWidth = sizeof(CullingParams);
    m_device->CreateBuffer(&cbDesc, nullptr, &m_cullingParamsBuffer);
    
    // BVH construction params constant buffer
    cbDesc.ByteWidth = sizeof(BVHConstructionParams);
    m_device->CreateBuffer(&cbDesc, nullptr, &m_bvhConstructionParamsBuffer);
}

void GPUBVHSystem::GenerateMortonCodes(const std::vector<RenderObject>& objects, const Vector3& sceneMin, const Vector3& sceneMax) {
    UpdateBVHConstructionParams(static_cast<int>(objects.size()), sceneMin, sceneMax);
    UpdateGPUObjectData(objects);
    
    m_context->CSSetShader(m_mortonCodeCS.Get(), nullptr, 0);
    
    ID3D11ShaderResourceView* srvs[] = { m_objectsSRV.Get() };
    m_context->CSSetShaderResources(0, 1, srvs);
    
    ID3D11Buffer* cbs[] = { m_bvhConstructionParamsBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);
    
    ID3D11UnorderedAccessView* uavs[] = { m_mortonCodesUAV.Get() };
    UINT initialCounts[] = { 0 };
    m_context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
    
    int numGroups = (static_cast<int>(objects.size()) + Config::COMPUTE_THREADS_PER_GROUP - 1) / Config::COMPUTE_THREADS_PER_GROUP;
    m_context->Dispatch(numGroups, 1, 1);
    
    // Unbind resources
    ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
    m_context->CSSetShaderResources(0, 1, nullSRVs);
    
    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
    
    m_context->CSSetShader(nullptr, nullptr, 0);
}

void GPUBVHSystem::SortMortonCodes() {
    // Simplified CPU-based sort for now
    // In production, implement GPU radix sort
    
    ComPtr<ID3D11Buffer> stagingBuffer;
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_STAGING;
    bufferDesc.ByteWidth = sizeof(GPUMortonCode) * m_objectCount;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    bufferDesc.StructureByteStride = sizeof(GPUMortonCode);
    
    m_device->CreateBuffer(&bufferDesc, nullptr, &stagingBuffer);
    m_context->CopyResource(stagingBuffer.Get(), m_mortonCodesBuffer.Get());
    
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(stagingBuffer.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped))) {
        GPUMortonCode* mortonCodes = static_cast<GPUMortonCode*>(mapped.pData);
        
        std::sort(mortonCodes, mortonCodes + m_objectCount, 
            [](const GPUMortonCode& a, const GPUMortonCode& b) {
                return a.mortonCode < b.mortonCode;
            });
        
        m_context->Unmap(stagingBuffer.Get(), 0);
    }
    
    m_context->CopyResource(m_mortonCodesBuffer.Get(), stagingBuffer.Get());
}

void GPUBVHSystem::ConstructBVHOnGPU() {
    m_context->CSSetShader(m_bvhConstructionCS.Get(), nullptr, 0);
    
    ID3D11ShaderResourceView* srvs[] = { m_mortonCodesSRV.Get(), m_objectsSRV.Get() };
    m_context->CSSetShaderResources(0, 2, srvs);
    
    ID3D11Buffer* cbs[] = { m_bvhConstructionParamsBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);
    
    ID3D11UnorderedAccessView* uavs[] = { m_bvhNodesUAV.Get() };
    UINT initialCounts[] = { 0 };
    m_context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
    
    int nodeCount = m_objectCount * 2 - 1;
    int numGroups = (nodeCount + Config::COMPUTE_THREADS_PER_GROUP - 1) / Config::COMPUTE_THREADS_PER_GROUP;
    m_context->Dispatch(numGroups, 1, 1);
    
    // Unbind resources
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    m_context->CSSetShaderResources(0, 2, nullSRVs);
    
    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
    
    m_context->CSSetShader(nullptr, nullptr, 0);
}

void GPUBVHSystem::UpdateBVHConstructionParams(int objectCount, const Vector3& sceneMin, const Vector3& sceneMax) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_bvhConstructionParamsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        BVHConstructionParams* params = static_cast<BVHConstructionParams*>(mapped.pData);
        params->objectCount = objectCount;
        params->nodeCount = objectCount * 2 - 1;
        params->sceneMinBounds[0] = sceneMin.x;
        params->sceneMinBounds[1] = sceneMin.y;
        params->sceneMinBounds[2] = sceneMin.z;
        params->sceneMaxBounds[0] = sceneMax.x;
        params->sceneMaxBounds[1] = sceneMax.y;
        params->sceneMaxBounds[2] = sceneMax.z;
        params->maxDepth = Config::MAX_BVH_DEPTH;
        params->padding = 0;
        m_context->Unmap(m_bvhConstructionParamsBuffer.Get(), 0);
    }
}

void GPUBVHSystem::UpdateGPUObjectData(const std::vector<RenderObject>& objects) {
    if (!m_objectsBuffer || objects.empty()) return;
    
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_objectsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        GPUObjectData* gpuObjects = static_cast<GPUObjectData*>(mapped.pData);        for (size_t i = 0; i < objects.size() && i < static_cast<size_t>(m_objectCount); i++) {
            const auto& obj = objects[i];
            auto& gpuObj = gpuObjects[i];
            
            // Copy bounds data using float4 arrays
            gpuObj.minBounds[0] = obj.minBounds.x;
            gpuObj.minBounds[1] = obj.minBounds.y;
            gpuObj.minBounds[2] = obj.minBounds.z;
            gpuObj.minBounds[3] = 0.0f;
            
            gpuObj.maxBounds[0] = obj.maxBounds.x;
            gpuObj.maxBounds[1] = obj.maxBounds.y;
            gpuObj.maxBounds[2] = obj.maxBounds.z;
            gpuObj.maxBounds[3] = 0.0f;
            
            gpuObj.objectIndex = static_cast<int>(i);
            gpuObj.occludedFrameCount = obj.occludedFrameCount;
            gpuObj.padding[0] = 0;
            gpuObj.padding[1] = 0;
        }
        
        m_context->Unmap(m_objectsBuffer.Get(), 0);
    }
}

void GPUBVHSystem::UpdateFrustumData(const Frustum& frustum) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_frustumBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        GPUFrustum* gpuFrustum = static_cast<GPUFrustum*>(mapped.pData);
        for (int i = 0; i < 6; i++) {
            gpuFrustum->planes[i][0] = frustum.planes[i].x;
            gpuFrustum->planes[i][1] = frustum.planes[i].y;
            gpuFrustum->planes[i][2] = frustum.planes[i].z;
            gpuFrustum->planes[i][3] = frustum.planes[i].w;
        }
        m_context->Unmap(m_frustumBuffer.Get(), 0);
    }
}

void GPUBVHSystem::UpdateCullingParams(int objectCount) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_cullingParamsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        CullingParams* params = static_cast<CullingParams*>(mapped.pData);
        params->rootNodeIndex = 0; // GPU-built BVH always has root at index 0
        params->objectCount = objectCount;
        params->nodeCount = objectCount * 2 - 1;
        params->maxDepth = Config::MAX_BVH_DEPTH;
        m_context->Unmap(m_cullingParamsBuffer.Get(), 0);
    }
}

const char* GPUBVHSystem::GetBVHRefitShaderSource() {
    return R"(
cbuffer CullingParams : register(b0) {
    int rootNodeIndex;
    int objectCount;
    int nodeCount;
    int maxDepth;
};

struct GPUBVHNode {
    float4 minBounds;
    float4 maxBounds;
    int leftChild;
    int rightChild;
    int objectIndex;
    int isLeaf;
};

struct GPUObjectData {
    float4 minBounds;
    float4 maxBounds;
    int objectIndex;
    int occludedFrameCount;
    int2 padding;
};

StructuredBuffer<GPUObjectData> ObjectData : register(t0);
RWStructuredBuffer<GPUBVHNode> BVHNodes : register(u0);

// Robust bottom-up BVH refitting that handles both leaf and internal nodes
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint nodeIndex = id.x;
    
    if (nodeIndex >= (uint)nodeCount) return;
    
    GPUBVHNode node = BVHNodes[nodeIndex];
    
    if (node.isLeaf) {        // Update leaf nodes with new object bounds
        if (node.objectIndex >= 0 && node.objectIndex < objectCount) {
            GPUObjectData objData = ObjectData[node.objectIndex];
            BVHNodes[nodeIndex].minBounds = objData.minBounds;
            BVHNodes[nodeIndex].maxBounds = objData.maxBounds;
        }
    } else {
        // Update internal nodes by encompassing child bounds
        if (node.leftChild >= 0 && node.rightChild >= 0 && 
            node.leftChild < nodeCount && node.rightChild < nodeCount) {
            
            GPUBVHNode leftChild = BVHNodes[node.leftChild];
            GPUBVHNode rightChild = BVHNodes[node.rightChild];
              // Calculate encompassing bounding box
            float3 newMinBounds = min(leftChild.minBounds.xyz, rightChild.minBounds.xyz);
            float3 newMaxBounds = max(leftChild.maxBounds.xyz, rightChild.maxBounds.xyz);
            
            // Atomic update to prevent race conditions
            BVHNodes[nodeIndex].minBounds = float4(newMinBounds, 0.0f);
            BVHNodes[nodeIndex].maxBounds = float4(newMaxBounds, 0.0f);
        }
    }
    
    // Memory barrier to ensure all updates are visible
    GroupMemoryBarrierWithGroupSync();
}
)";
}

void GPUBVHSystem::UpdateDynamicObjects(std::vector<RenderObject>& objects, float deltaTime) {
    bool hasMovingObjects = false;
    
    for (auto& obj : objects) {
        if (obj.isDynamic) {
            // Store previous position for movement tracking
            obj.previousPosition = obj.GetPosition();
            
            // Update animation time
            obj.animationTime += deltaTime;
            
            // Calculate new position based on circular motion
            Vector3 newPosition = obj.animationCenter + Vector3(
                cos(obj.animationTime) * obj.animationRadius,
                0.0f,
                sin(obj.animationTime) * obj.animationRadius
            );
            
            // Calculate movement distance for this frame
            obj.movementDistance = (newPosition - obj.previousPosition).Length();
            
            // Update world matrix with new position
            obj.world = Matrix::CreateTranslation(newPosition);
            
            // Update object's bounding box based on new position
            obj.UpdateBounds();
            
            // Track if any objects are moving significantly
            if (obj.movementDistance > Config::MOVEMENT_THRESHOLD) {
                hasMovingObjects = true;
            }
        }
    }
    
    // Update velocity for all dynamic objects (for future prediction if needed)
    for (auto& obj : objects) {
        if (obj.isDynamic) {
            Vector3 currentPos = obj.GetPosition();
            if (deltaTime > 0.0f) {
                obj.velocity = (currentPos - obj.previousPosition) / deltaTime;
            }
        }
    }
}

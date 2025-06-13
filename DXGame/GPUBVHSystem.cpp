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
    
    // Step 1: Generate Morton codes
    GenerateMortonCodes(objects, sceneMin, sceneMax);
    
    // Step 2: Sort Morton codes
    SortMortonCodes();
    
    // Step 3: Build BVH structure
    ConstructBVHOnGPU();
    
    m_needsRebuild = false;
    return true;
}

bool GPUBVHSystem::PerformFrustumCulling(const Frustum& frustum, std::vector<RenderObject>& objects) {
    if (!m_frustumCullingCS || objects.empty() || !m_bvhNodesBuffer || !m_objectsBuffer) {
        return false;
    }
    
    // Read results from previous frame (non-blocking)
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_visibilityReadbackBuffer.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (SUCCEEDED(hr)) {
        const int* visibility = static_cast<const int*>(mapped.pData);
        for (size_t i = 0; i < objects.size() && i < m_objectCount; i++) {
            objects[i].visible = (visibility[i] != 0);
        }
        m_context->Unmap(m_visibilityReadbackBuffer.Get(), 0);
    }
    
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
    
    // Dispatch compute shader
    int numGroups = (static_cast<int>(objects.size()) + Config::COMPUTE_THREADS_PER_GROUP - 1) / Config::COMPUTE_THREADS_PER_GROUP;
    m_context->Dispatch(numGroups, 1, 1);
    
    // Unbind resources
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    m_context->CSSetShaderResources(0, 2, nullSRVs);
    
    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
    
    m_context->CSSetShader(nullptr, nullptr, 0);
    
    // Copy results for next frame
    m_context->CopyResource(m_visibilityReadbackBuffer.Get(), m_visibilityBuffer.Get());
    
    return true;
}

bool GPUBVHSystem::CreateComputeShaders() {
    if (!CompileAndCreateComputeShader(GetMortonCodeShaderSource(), &m_mortonCodeCS)) {
        return false;
    }
    
    if (!CompileAndCreateComputeShader(GetBVHConstructionShaderSource(), &m_bvhConstructionCS)) {
        return false;
    }
    
    if (!CompileAndCreateComputeShader(GetFrustumCullingShaderSource(), &m_frustumCullingCS)) {
        return false;
    }
    
    return true;
}

bool GPUBVHSystem::CreateBuffers(int objectCount) {
    if (objectCount <= 0) return false;
    
    int nodeCount = objectCount * 2 - 1; // Maximum nodes in a binary tree
    
    CreateMortonCodesBuffer(objectCount);
    CreateBVHConstructionBuffer(nodeCount);
    CreateBVHNodesBuffer(nodeCount);
    CreateObjectsBuffer(objectCount);
    CreateVisibilityBuffer(objectCount);
    CreateConstantBuffers();
    
    return true;
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
        GPUObjectData* gpuObjects = static_cast<GPUObjectData*>(mapped.pData);
        
        for (size_t i = 0; i < objects.size() && i < m_objectCount; i++) {
            const auto& obj = objects[i];
            auto& gpuObj = gpuObjects[i];
            
            gpuObj.minBounds[0] = obj.minBounds.x;
            gpuObj.minBounds[1] = obj.minBounds.y;
            gpuObj.minBounds[2] = obj.minBounds.z;
            gpuObj.maxBounds[0] = obj.maxBounds.x;
            gpuObj.maxBounds[1] = obj.maxBounds.y;
            gpuObj.maxBounds[2] = obj.maxBounds.z;
            gpuObj.objectIndex = static_cast<int>(i);
            gpuObj.occludedFrameCount = obj.occludedFrameCount;
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

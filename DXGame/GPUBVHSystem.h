#pragma once

#include "Common.h"
#include "Structures.h"

// ============================================================================
// GPU BVH SYSTEM CLASS
// ============================================================================

class GPUBVHSystem {
public:
    GPUBVHSystem() = default;
    ~GPUBVHSystem() = default;

    // Initialization
    bool Initialize(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context, int objectCount);
    void Shutdown();    // BVH operations
    bool BuildBVH(const std::vector<RenderObject>& objects, const Vector3& sceneMin, const Vector3& sceneMax);
    bool PerformFrustumCulling(const Frustum& frustum, std::vector<RenderObject>& objects);
    bool RefitBVH(const std::vector<RenderObject>& objects);
    
    // Dynamic object management
    void UpdateDynamicObjects(std::vector<RenderObject>& objects, float deltaTime);
    bool ShouldRebuildBVH(const std::vector<RenderObject>& objects);
    float CalculateBVHQuality() const;
    
    // State management
    void MarkForRebuild() { m_needsRebuild = true; }
    bool NeedsRebuild() const { return m_needsRebuild; }
    void ResetFrameCounter() { m_framesSinceLastRebuild = 0; }

private:
    // Device and context
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;    // Compute shaders
    ComPtr<ID3D11ComputeShader> m_mortonCodeCS;
    ComPtr<ID3D11ComputeShader> m_bvhConstructionCS;
    ComPtr<ID3D11ComputeShader> m_frustumCullingCS;
    ComPtr<ID3D11ComputeShader> m_bvhRefitCS;
    
    // GPU buffers
    ComPtr<ID3D11Buffer> m_bvhNodesBuffer;
    ComPtr<ID3D11Buffer> m_bvhConstructionBuffer;
    ComPtr<ID3D11Buffer> m_objectsBuffer;
    ComPtr<ID3D11Buffer> m_mortonCodesBuffer;
    ComPtr<ID3D11Buffer> m_visibilityBuffer;
    ComPtr<ID3D11Buffer> m_frustumBuffer;
    ComPtr<ID3D11Buffer> m_cullingParamsBuffer;
    ComPtr<ID3D11Buffer> m_bvhConstructionParamsBuffer;
    
    // Resource views
    ComPtr<ID3D11ShaderResourceView> m_bvhNodesSRV;
    ComPtr<ID3D11ShaderResourceView> m_objectsSRV;
    ComPtr<ID3D11ShaderResourceView> m_mortonCodesSRV;
    ComPtr<ID3D11UnorderedAccessView> m_bvhNodesUAV;
    ComPtr<ID3D11UnorderedAccessView> m_bvhConstructionUAV;
    ComPtr<ID3D11UnorderedAccessView> m_mortonCodesUAV;
    ComPtr<ID3D11UnorderedAccessView> m_visibilityUAV;
    ComPtr<ID3D11Buffer> m_visibilityReadbackBuffer;    // State tracking for intelligent BVH management
    bool m_needsRebuild = true;
    int m_objectCount = 0;
    int m_framesSinceLastRebuild = 0;
    float m_accumulatedMovement = 0.0f;
    std::vector<Vector3> m_previousPositions;
    
    // BVH quality metrics
    float m_initialBVHSurfaceArea = 0.0f;
    float m_currentBVHSurfaceArea = 0.0f;
    
    // Initialization helpers
    bool CreateComputeShaders();
    bool CreateBuffers(int objectCount);
    bool CompileAndCreateComputeShader(const char* source, ComPtr<ID3D11ComputeShader>* outShader);
    
    // Buffer creation helpers
    void CreateMortonCodesBuffer(int objectCount);
    void CreateBVHConstructionBuffer(int nodeCount);
    void CreateBVHNodesBuffer(int nodeCount);
    void CreateObjectsBuffer(int objectCount);
    void CreateVisibilityBuffer(int objectCount);
    void CreateConstantBuffers();
      // BVH construction and updates
    void GenerateMortonCodes(const std::vector<RenderObject>& objects, const Vector3& sceneMin, const Vector3& sceneMax);
    void SortMortonCodes();
    void ConstructBVHOnGPU();
    bool RefitBVHBottomUp(const std::vector<RenderObject>& objects);
    
    // Quality and decision making
    float CalculateSurfaceAreaHeuristic() const;
    void UpdateBVHQualityMetrics();
    
    // Data updates
    void UpdateBVHConstructionParams(int objectCount, const Vector3& sceneMin, const Vector3& sceneMax);
    void UpdateGPUObjectData(const std::vector<RenderObject>& objects);
    void UpdateFrustumData(const Frustum& frustum);
    void UpdateCullingParams(int objectCount);
    
    // Shader source code
    static const char* GetMortonCodeShaderSource();
    static const char* GetBVHConstructionShaderSource();
    static const char* GetFrustumCullingShaderSource();
    static const char* GetBVHRefitShaderSource();
};

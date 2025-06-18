#pragma once

#include "Common.h"

// ============================================================================
// GPU-ALIGNED STRUCTURES
// ============================================================================

// GPU-aligned BVH Node for compute shaders
struct GPUBVHNode {
    float minBounds[4];      // Use float4 for alignment
    float maxBounds[4];      // Use float4 for alignment  
    int leftChild;
    int rightChild;
    int objectIndex;
    int isLeaf;
};

// GPU-aligned Object data for compute shaders
struct GPUObjectData {
    float minBounds[4];      // Use float4 for alignment
    float maxBounds[4];      // Use float4 for alignment
    int objectIndex;
    int occludedFrameCount;
    int padding[2];          // Pad to 16-byte boundary
};

// GPU Morton code structure for BVH construction
struct GPUMortonCode {
    uint32_t mortonCode;
    int objectIndex;
    float padding[2];
};

// GPU BVH construction data
struct GPUBVHConstructionNode {
    float minBounds[4];      // Use float4 for alignment
    float maxBounds[4];      // Use float4 for alignment
    int leftChild;
    int rightChild;
    int objectIndex;
    int isLeaf;
};

// GPU Frustum data
struct GPUFrustum {
    float planes[6][4]; // 6 planes, each with 4 components (x,y,z,w)
};

// ============================================================================
// PARAMETER STRUCTURES
// ============================================================================

// Culling parameters
struct CullingParams {
    int rootNodeIndex;
    int objectCount;
    int nodeCount;
    int maxDepth;
};

// BVH Construction parameters
struct BVHConstructionParams {
    int objectCount;
    int nodeCount;
    float sceneMinBounds[3];
    float sceneMaxBounds[3];
    int maxDepth;
    int padding;
};

// ============================================================================
// CPU STRUCTURES
// ============================================================================

// BVH Node Structure (CPU version)
struct BVHNode {
    Vector3 minBounds;
    Vector3 maxBounds;
    int leftChild = -1;
    int rightChild = -1;
    int objectIndex = -1; // For leaf nodes
    bool isLeaf = false;
};

// Renderable Object
struct RenderObject {
    Matrix world;
    Vector3 minBounds;
    Vector3 maxBounds;
    bool visible = true;
    ComPtr<ID3D11Query> occlusionQuery;
    UINT64 lastQueryResult = 0;
    bool queryInProgress = false;
    int occludedFrameCount = 0;  // Track consecutive occluded frames
    
    // Dynamic object support
    bool isDynamic = false;
    Vector3 velocity = Vector3::Zero;
    Vector3 previousPosition = Vector3::Zero;
    float movementDistance = 0.0f;
    Vector3 baseSize = Vector3(1.0f, 1.0f, 1.0f);  // Object's base dimensions
    
    // Animation support
    float animationTime = 0.0f;
    Vector3 animationCenter = Vector3::Zero;
    float animationRadius = 0.0f;
    
    // Helper methods
    Vector3 GetPosition() const {
        return Vector3(world._41, world._42, world._43);
    }
    
    void UpdateBounds() {
        Vector3 position = GetPosition();
        Vector3 halfSize = baseSize * 0.5f;
        minBounds = position - halfSize;
        maxBounds = position + halfSize;
    }
};

// Frustum structure for culling (CPU version)
struct Frustum {
    XMFLOAT4 planes[6]; // left, right, top, bottom, near, far
    
    void ExtractFromMatrix(const Matrix& viewProjection);
    bool IsBoxInFrustum(const Vector3& minBounds, const Vector3& maxBounds) const;
};

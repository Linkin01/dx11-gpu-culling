#pragma once

#include "Common.h"
#include "Structures.h"

// ============================================================================
// CPU BVH SYSTEM CLASS (Fallback)
// ============================================================================

class CPUBVHSystem {
public:
    CPUBVHSystem() = default;
    ~CPUBVHSystem() = default;

    // BVH operations
    void BuildBVH(const std::vector<RenderObject>& objects);
    void PerformFrustumCulling(const Frustum& frustum, std::vector<RenderObject>& objects);
    
    // State management
    bool IsValid() const { return m_rootNode >= 0 && !m_bvhNodes.empty(); }

private:
    std::vector<BVHNode> m_bvhNodes;
    int m_rootNode = -1;
    
    // BVH construction helpers
    int BuildBVHRecursive(std::vector<int>& nodeIndices);
    void FrustumCullBVH(int nodeIndex, const Frustum& frustum, std::vector<RenderObject>& objects);
};

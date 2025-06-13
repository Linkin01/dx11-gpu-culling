#include "CPUBVHSystem.h"

void CPUBVHSystem::BuildBVH(const std::vector<RenderObject>& objects) {
    if (objects.empty()) return;
    
    m_bvhNodes.clear();
    m_bvhNodes.reserve(objects.size() * 2);
    
    // Create leaf nodes
    std::vector<int> objectIndices;
    for (size_t i = 0; i < objects.size(); ++i) {
        BVHNode leafNode;
        leafNode.minBounds = objects[i].minBounds;
        leafNode.maxBounds = objects[i].maxBounds;
        leafNode.objectIndex = static_cast<int>(i);
        leafNode.isLeaf = true;
        
        m_bvhNodes.push_back(leafNode);
        objectIndices.push_back(static_cast<int>(m_bvhNodes.size() - 1));
    }
    
    // Build tree recursively
    m_rootNode = BuildBVHRecursive(objectIndices);
}

void CPUBVHSystem::PerformFrustumCulling(const Frustum& frustum, std::vector<RenderObject>& objects) {
    // Reset all objects to not visible
    for (auto& obj : objects) {
        obj.visible = false;
    }
    
    // Traverse BVH and perform frustum culling
    if (IsValid()) {
        FrustumCullBVH(m_rootNode, frustum, objects);
    }
}

int CPUBVHSystem::BuildBVHRecursive(std::vector<int>& nodeIndices) {
    if (nodeIndices.size() == 1) {
        return nodeIndices[0];
    }
    
    // Calculate bounding box for all nodes
    Vector3 minBounds = m_bvhNodes[nodeIndices[0]].minBounds;
    Vector3 maxBounds = m_bvhNodes[nodeIndices[0]].maxBounds;
    
    for (size_t i = 1; i < nodeIndices.size(); ++i) {
        const auto& node = m_bvhNodes[nodeIndices[i]];
        minBounds = Vector3::Min(minBounds, node.minBounds);
        maxBounds = Vector3::Max(maxBounds, node.maxBounds);
    }
    
    // Find the axis with the largest extent
    Vector3 extent = maxBounds - minBounds;
    int axis = 0;
    if (extent.y > extent.x) axis = 1;
    if (extent.z > (axis == 0 ? extent.x : extent.y)) axis = 2;
    
    // Sort nodes along the chosen axis
    std::sort(nodeIndices.begin(), nodeIndices.end(), [this, axis](int a, int b) {
        Vector3 centerA = (m_bvhNodes[a].minBounds + m_bvhNodes[a].maxBounds) * 0.5f;
        Vector3 centerB = (m_bvhNodes[b].minBounds + m_bvhNodes[b].maxBounds) * 0.5f;
        if (axis == 0)
            return centerA.x < centerB.x;
        else if (axis == 1)
            return centerA.y < centerB.y;
        else
            return centerA.z < centerB.z;
    });
    
    // Split in half
    size_t mid = nodeIndices.size() / 2;
    std::vector<int> leftNodes(nodeIndices.begin(), nodeIndices.begin() + mid);
    std::vector<int> rightNodes(nodeIndices.begin() + mid, nodeIndices.end());
    
    // Create internal node
    BVHNode internalNode;
    internalNode.minBounds = minBounds;
    internalNode.maxBounds = maxBounds;
    internalNode.isLeaf = false;
    
    m_bvhNodes.push_back(internalNode);
    int nodeIndex = static_cast<int>(m_bvhNodes.size() - 1);
    
    // Recursively build children
    m_bvhNodes[nodeIndex].leftChild = BuildBVHRecursive(leftNodes);
    m_bvhNodes[nodeIndex].rightChild = BuildBVHRecursive(rightNodes);
    
    return nodeIndex;
}

void CPUBVHSystem::FrustumCullBVH(int nodeIndex, const Frustum& frustum, std::vector<RenderObject>& objects) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_bvhNodes.size())) return;
    
    const auto& node = m_bvhNodes[nodeIndex];
    
    // Check if node is in frustum
    if (!frustum.IsBoxInFrustum(node.minBounds, node.maxBounds)) {
        return; // Node is outside frustum, skip entire subtree
    }
    
    if (node.isLeaf) {
        // Mark object as visible
        if (node.objectIndex >= 0 && node.objectIndex < static_cast<int>(objects.size())) {
            objects[node.objectIndex].visible = true;
        }
    } else {
        // Recursively check children
        FrustumCullBVH(node.leftChild, frustum, objects);
        FrustumCullBVH(node.rightChild, frustum, objects);
    }
}

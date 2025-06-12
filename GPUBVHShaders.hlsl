// GPU BVH Construction and Frustum Culling Shaders
// Comprehensive HLSL implementation for fully GPU-side BVH and culling

// ============================================================================
// STRUCTURES
// ============================================================================

struct ObjectData {
    float3 minBounds;
    float3 maxBounds;
    int objectIndex;
    int occludedFrameCount;
};

struct MortonCode {
    uint mortonCode;
    int objectIndex;
    float2 padding;
};

struct BVHNode {
    float3 minBounds;
    float3 maxBounds;
    int leftChild;
    int rightChild;
    int objectIndex;
    int isLeaf;
    int parent;
    int atomicCounter;
};

struct Frustum {
    float4 planes[6];
};

struct CullingParams {
    int rootNodeIndex;
    int objectCount;
    int nodeCount;
    int maxDepth;
};

struct BVHConstructionParams {
    int objectCount;
    int nodeCount;
    float3 sceneMinBounds;
    float3 sceneMaxBounds;
    int maxDepth;
    int padding;
};

// ============================================================================
// MORTON CODE GENERATION COMPUTE SHADER
// ============================================================================

StructuredBuffer<ObjectData> Objects : register(t0);
ConstantBuffer<BVHConstructionParams> Params : register(b0);
RWStructuredBuffer<MortonCode> MortonCodes : register(u0);

// Expand a 10-bit integer into 30 bits by inserting 2 zeros after each bit
uint expandBits(uint v) {
    v = (v * 0x00010001u) & 0xFF0000FFu;
    v = (v * 0x00000101u) & 0x0F00F00Fu;
    v = (v * 0x00000011u) & 0xC30C30C3u;
    v = (v * 0x00000005u) & 0x49249249u;
    return v;
}

// Calculate 3D Morton code for a 3D point
uint morton3D(float3 pos) {
    pos = clamp(pos, 0.0f, 1023.0f);
    uint x = expandBits((uint)pos.x);
    uint y = expandBits((uint)pos.y);
    uint z = expandBits((uint)pos.z);
    return x * 4 + y * 2 + z;
}

[numthreads(64, 1, 1)]
void CSMortonCode(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)Params.objectCount) return;
    
    ObjectData obj = Objects[id.x];
    
    // Calculate object center
    float3 center = (obj.minBounds + obj.maxBounds) * 0.5f;
    
    // Normalize to [0, 1023] range for Morton code calculation
    float3 extent = Params.sceneMaxBounds - Params.sceneMinBounds;
    float3 normalizedPos = (center - Params.sceneMinBounds) / extent * 1023.0f;
    
    // Calculate Morton code
    uint mortonCode = morton3D(normalizedPos);
    
    // Store result
    MortonCodes[id.x].mortonCode = mortonCode;
    MortonCodes[id.x].objectIndex = obj.objectIndex;
}

// ============================================================================
// BVH CONSTRUCTION COMPUTE SHADER
// ============================================================================

StructuredBuffer<MortonCode> SortedMortonCodes : register(t0);
StructuredBuffer<ObjectData> ObjectsForBVH : register(t1);
ConstantBuffer<BVHConstructionParams> BVHParams : register(b0);
RWStructuredBuffer<BVHNode> BVHNodes : register(u0);

// Find the split position using binary search on Morton codes
int findSplit(int first, int last) {
    uint firstCode = SortedMortonCodes[first].mortonCode;
    uint lastCode = SortedMortonCodes[last].mortonCode;
    
    if (firstCode == lastCode) {
        return (first + last) >> 1;
    }
    
    // Find the highest bit where they differ
    int commonPrefix = firstbithigh(firstCode ^ lastCode);
    int split = first;
    int step = last - first;
    
    do {
        step = (step + 1) >> 1;
        int newSplit = split + step;
        
        if (newSplit < last) {
            uint splitCode = SortedMortonCodes[newSplit].mortonCode;
            int splitPrefix = firstbithigh(firstCode ^ splitCode);
            if (splitPrefix > commonPrefix) {
                split = newSplit;
            }
        }
    } while (step > 1);
    
    return split;
}

// Calculate AABB for a range of objects
void calculateBounds(int first, int last, out float3 minBounds, out float3 maxBounds) {
    int firstObjIdx = SortedMortonCodes[first].objectIndex;
    ObjectData firstObj = ObjectsForBVH[firstObjIdx];
    minBounds = firstObj.minBounds;
    maxBounds = firstObj.maxBounds;
    
    for (int i = first + 1; i <= last; i++) {
        int objIdx = SortedMortonCodes[i].objectIndex;
        ObjectData obj = ObjectsForBVH[objIdx];
        minBounds = min(minBounds, obj.minBounds);
        maxBounds = max(maxBounds, obj.maxBounds);
    }
}

[numthreads(64, 1, 1)]
void CSBVHConstruction(uint3 id : SV_DispatchThreadID) {
    int nodeIndex = (int)id.x;
    int numInternalNodes = BVHParams.objectCount - 1;
    
    if (nodeIndex >= BVHParams.nodeCount) return;
    
    if (nodeIndex >= numInternalNodes) {
        // Create leaf nodes
        int leafIndex = nodeIndex - numInternalNodes;
        if (leafIndex < BVHParams.objectCount) {
            int objIdx = SortedMortonCodes[leafIndex].objectIndex;
            ObjectData obj = ObjectsForBVH[objIdx];
            
            int leafNodeIndex = numInternalNodes + leafIndex;
            BVHNodes[leafNodeIndex].minBounds = obj.minBounds;
            BVHNodes[leafNodeIndex].maxBounds = obj.maxBounds;
            BVHNodes[leafNodeIndex].leftChild = -1;
            BVHNodes[leafNodeIndex].rightChild = -1;
            BVHNodes[leafNodeIndex].objectIndex = objIdx;
            BVHNodes[leafNodeIndex].isLeaf = 1;
            BVHNodes[leafNodeIndex].parent = -1;
            BVHNodes[leafNodeIndex].atomicCounter = 0;
        }
        return;
    }
    
    // Create internal nodes using binary radix tree construction
    int first, last;
    
    if (nodeIndex == 0) {
        // Root node spans all objects
        first = 0;
        last = BVHParams.objectCount - 1;
    } else {
        // Determine range for this internal node
        // This is a simplified version - in practice you'd use more sophisticated
        // range determination based on the binary radix tree algorithm
        int objectsPerNode = (BVHParams.objectCount + numInternalNodes - 1) / numInternalNodes;
        first = nodeIndex * objectsPerNode;
        last = min(first + objectsPerNode - 1, BVHParams.objectCount - 1);
    }
    
    if (first >= last) return;
    
    int split = findSplit(first, last);
    
    // Create child indices
    int leftChild = (split == first) ? numInternalNodes + split : split;
    int rightChild = (split + 1 == last) ? numInternalNodes + split + 1 : split + 1;
    
    // Ensure valid child indices
    leftChild = clamp(leftChild, 0, BVHParams.nodeCount - 1);
    rightChild = clamp(rightChild, 0, BVHParams.nodeCount - 1);
    
    BVHNodes[nodeIndex].leftChild = leftChild;
    BVHNodes[nodeIndex].rightChild = rightChild;
    BVHNodes[nodeIndex].objectIndex = -1;
    BVHNodes[nodeIndex].isLeaf = 0;
    BVHNodes[nodeIndex].parent = -1;
    BVHNodes[nodeIndex].atomicCounter = 0;
    
    // Set parent pointers for children
    if (leftChild < BVHParams.nodeCount) {
        BVHNodes[leftChild].parent = nodeIndex;
    }
    if (rightChild < BVHParams.nodeCount) {
        BVHNodes[rightChild].parent = nodeIndex;
    }
    
    // Calculate bounding box for this internal node
    float3 minBounds, maxBounds;
    calculateBounds(first, last, minBounds, maxBounds);
    BVHNodes[nodeIndex].minBounds = minBounds;
    BVHNodes[nodeIndex].maxBounds = maxBounds;
}

// ============================================================================
// FRUSTUM CULLING COMPUTE SHADER
// ============================================================================

StructuredBuffer<BVHNode> BVHNodesForCulling : register(t0);
StructuredBuffer<ObjectData> ObjectsForCulling : register(t1);
ConstantBuffer<Frustum> FrustumData : register(b0);
ConstantBuffer<CullingParams> CullingSettings : register(b1);
RWStructuredBuffer<int> Visibility : register(u0);

// Test if an AABB is inside or intersects the viewing frustum
bool IsBoxInFrustum(float3 minBounds, float3 maxBounds) {
    // Test against all 6 frustum planes
    for (int i = 0; i < 6; i++) {
        float4 plane = FrustumData.planes[i];
        
        // Find the "positive vertex" - the corner furthest in the plane normal direction
        float3 positiveVertex;
        positiveVertex.x = (plane.x >= 0.0f) ? maxBounds.x : minBounds.x;
        positiveVertex.y = (plane.y >= 0.0f) ? maxBounds.y : minBounds.y;
        positiveVertex.z = (plane.z >= 0.0f) ? maxBounds.z : minBounds.z;
        
        // If the positive vertex is behind the plane, the entire AABB is outside
        float distance = dot(plane.xyz, positiveVertex) + plane.w;
        if (distance < 0.0f) {
            return false;
        }
    }
    return true;
}

[numthreads(64, 1, 1)]
void CSFrustumCulling(uint3 id : SV_DispatchThreadID) {
    uint threadId = id.x;
    
    // Each thread processes multiple objects for better parallelization
    uint objectsPerThread = (CullingSettings.objectCount + 63) / 64;
    uint startIdx = threadId * objectsPerThread;
    uint endIdx = min(startIdx + objectsPerThread, (uint)CullingSettings.objectCount);
    
    // Process objects assigned to this thread
    for (uint objIdx = startIdx; objIdx < endIdx; objIdx++) {
        ObjectData obj = ObjectsForCulling[objIdx];        
        // Reset visibility for frustum culling
        Visibility[objIdx] = 0;
              // Traverse BVH iteratively to test frustum culling for this object
            int stack[64]; // Increased stack size for deeper BVHs
            int stackPtr = 0;
            
            if (CullingSettings.rootNodeIndex >= 0) {
                stack[stackPtr++] = CullingSettings.rootNodeIndex;
                
                while (stackPtr > 0 && stackPtr < 64) {
                    int nodeIndex = stack[--stackPtr];
                    
                    if (nodeIndex < 0 || nodeIndex >= CullingSettings.nodeCount) continue;
                    
                    BVHNode node = BVHNodesForCulling[nodeIndex];
                    
                    // Test if this BVH node is in the frustum
                    if (!IsBoxInFrustum(node.minBounds, node.maxBounds)) {
                        continue; // Skip this entire subtree
                    }
                    
                    if (node.isLeaf) {
                        // If this leaf contains our object, mark it visible
                        if (node.objectIndex == (int)objIdx) {
                            Visibility[objIdx] = 1;
                            break; // Found our object and it's visible
                        }
                    } else {
                        // Add children to traversal stack with bounds checking
                        if (node.rightChild >= 0 && stackPtr < 63) {
                            stack[stackPtr++] = node.rightChild;
                        }
                        if (node.leftChild >= 0 && stackPtr < 63) {
                            stack[stackPtr++] = node.leftChild;
                        }
                        // If stack is full, we may miss some nodes, but prevent overflow
                    }
                }
            }
        }
    }
}

// ============================================================================
// ADVANCED GPU RADIX SORT (Optional - for high-performance Morton code sorting)
// ============================================================================

// This is a placeholder for a full GPU radix sort implementation
// For production code, you would implement a complete radix sort here
// or use existing libraries like CUB or ModernGPU equivalents for DirectX

groupshared uint sharedMem[1024];

[numthreads(256, 1, 1)]
void CSRadixSort(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    // Simplified radix sort implementation
    // In practice, this would be a full multi-pass radix sort
    // operating on Morton codes to sort them efficiently on GPU
    
    uint threadId = tid.x;
    uint globalId = id.x;
    
    // Load data into shared memory
    if (globalId < (uint)BVHParams.objectCount) {
        sharedMem[threadId] = SortedMortonCodes[globalId].mortonCode;
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    // Perform local sort within workgroup
    // This is a placeholder - implement full bitonic sort or similar
    for (uint size = 2; size <= 256; size <<= 1) {
        for (uint stride = size >> 1; stride > 0; stride >>= 1) {
            if (threadId < 128) {
                uint pos = 2 * threadId - (threadId & (stride - 1));
                if ((threadId & (size >> 1)) == 0) {
                    if (sharedMem[pos] > sharedMem[pos + stride]) {
                        uint temp = sharedMem[pos];
                        sharedMem[pos] = sharedMem[pos + stride];
                        sharedMem[pos + stride] = temp;
                    }
                } else {
                    if (sharedMem[pos] < sharedMem[pos + stride]) {
                        uint temp = sharedMem[pos];
                        sharedMem[pos] = sharedMem[pos + stride];
                        sharedMem[pos + stride] = temp;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    
    // Write back to global memory
    if (globalId < (uint)BVHParams.objectCount) {
        // Note: This is simplified - real implementation would need
        // to maintain object index association during sort
        SortedMortonCodes[globalId].mortonCode = sharedMem[threadId];
    }
}

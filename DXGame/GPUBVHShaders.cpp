#include "GPUBVHSystem.h"

// Static shader source methods implementation

const char* GPUBVHSystem::GetMortonCodeShaderSource() {
    return R"(
        struct ObjectData {
            float4 minBounds;
            float4 maxBounds;
            int objectIndex;
            int occludedFrameCount;
            int2 padding;
        };
        
        struct MortonCode {
            uint mortonCode;
            int objectIndex;
            float2 padding;
        };
        
        struct BVHConstructionParams {
            int objectCount;
            int nodeCount;
            float3 sceneMinBounds;
            float3 sceneMaxBounds;
            int maxDepth;
            int padding;
        };
        
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
        
        // Calculate Morton code for 3D point
        uint morton3D(float3 pos) {
            pos = clamp(pos, 0.0f, 1023.0f);
            uint x = expandBits((uint)pos.x);
            uint y = expandBits((uint)pos.y);
            uint z = expandBits((uint)pos.z);
            return x * 4 + y * 2 + z;
        }
        
        [numthreads(64, 1, 1)]
        void main(uint3 id : SV_DispatchThreadID) {
            if (id.x >= (uint)Params.objectCount) return;
            
            ObjectData obj = Objects[id.x];
              // Calculate object center
            float3 center = (obj.minBounds.xyz + obj.maxBounds.xyz) * 0.5f;
            
            // Normalize to [0, 1023] range for Morton code
            float3 extent = Params.sceneMaxBounds - Params.sceneMinBounds;
            float3 normalizedPos = (center - Params.sceneMinBounds) / extent * 1023.0f;
            
            // Calculate Morton code
            uint mortonCode = morton3D(normalizedPos);
            
            // Store result
            MortonCodes[id.x].mortonCode = mortonCode;
            MortonCodes[id.x].objectIndex = obj.objectIndex;
        }
    )";
}

const char* GPUBVHSystem::GetBVHConstructionShaderSource() {
    return R"(
        struct MortonCode {
            uint mortonCode;
            int objectIndex;
            float2 padding;
        };
          struct ObjectData {
            float4 minBounds;
            float4 maxBounds;
            int objectIndex;
            int occludedFrameCount;
            int2 padding;
        };
        
        struct BVHNode {
            float4 minBounds;
            float4 maxBounds;
            int leftChild;
            int rightChild;
            int objectIndex;
            int isLeaf;
        };
        
        struct BVHConstructionParams {
            int objectCount;
            int nodeCount;
            float3 sceneMinBounds;
            float3 sceneMaxBounds;
            int maxDepth;
            int padding;
        };
        
        StructuredBuffer<MortonCode> SortedMortonCodes : register(t0);
        StructuredBuffer<ObjectData> Objects : register(t1);
        ConstantBuffer<BVHConstructionParams> Params : register(b0);
        RWStructuredBuffer<BVHNode> BVHNodes : register(u0);
        
        // Find the split position using binary search
        int findSplit(int first, int last) {
            uint firstCode = SortedMortonCodes[first].mortonCode;
            uint lastCode = SortedMortonCodes[last].mortonCode;
            
            if (firstCode == lastCode) {
                return (first + last) >> 1;
            }
            
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
          // Calculate bounding box for a range of objects
        void calculateBounds(int first, int last, out float3 minBounds, out float3 maxBounds) {
            int firstObjIdx = SortedMortonCodes[first].objectIndex;
            ObjectData firstObj = Objects[firstObjIdx];
            minBounds = firstObj.minBounds.xyz;
            maxBounds = firstObj.maxBounds.xyz;
            
            for (int i = first + 1; i <= last; i++) {
                int objIdx = SortedMortonCodes[i].objectIndex;
                ObjectData obj = Objects[objIdx];
                minBounds = min(minBounds, obj.minBounds.xyz);
                maxBounds = max(maxBounds, obj.maxBounds.xyz);
            }
        }
        
        [numthreads(64, 1, 1)]
        void main(uint3 id : SV_DispatchThreadID) {
            int nodeIndex = (int)id.x;
            int numInternalNodes = Params.objectCount - 1;
            
            if (nodeIndex >= numInternalNodes) {
                // Create leaf nodes
                int leafIndex = nodeIndex - numInternalNodes;
                if (leafIndex < Params.objectCount) {
                    int objIdx = SortedMortonCodes[leafIndex].objectIndex;
                    ObjectData obj = Objects[objIdx];
                      int leafNodeIndex = numInternalNodes + leafIndex;
                    BVHNodes[leafNodeIndex].minBounds = float4(obj.minBounds.xyz, 0.0f);
                    BVHNodes[leafNodeIndex].maxBounds = float4(obj.maxBounds.xyz, 0.0f);                    BVHNodes[leafNodeIndex].leftChild = -1;
                    BVHNodes[leafNodeIndex].rightChild = -1;
                    BVHNodes[leafNodeIndex].objectIndex = objIdx;
                    BVHNodes[leafNodeIndex].isLeaf = 1;
                }
                return;
            }
            
            // Create internal nodes
            int first = nodeIndex;
            int last = nodeIndex + 1;
            
            // Determine range
            if (nodeIndex == 0) {
                first = 0;
                last = Params.objectCount - 1;
            } else {
                // Binary radix tree construction
                int split = findSplit(0, Params.objectCount - 1);
                
                if (nodeIndex <= split) {
                    last = split;
                } else {
                    first = split + 1;
                    last = Params.objectCount - 1;
                }
            }
            
            int split = findSplit(first, last);
            
            // Create child indices
            int leftChild = (split == first) ? numInternalNodes + split : split;
            int rightChild = (split + 1 == last) ? numInternalNodes + split + 1 : split + 1;
              BVHNodes[nodeIndex].leftChild = leftChild;
            BVHNodes[nodeIndex].rightChild = rightChild;
            BVHNodes[nodeIndex].objectIndex = -1;
            BVHNodes[nodeIndex].isLeaf = 0;
            
            // Calculate bounding box
            float3 minBounds, maxBounds;
            calculateBounds(first, last, minBounds, maxBounds);            BVHNodes[nodeIndex].minBounds = float4(minBounds, 0.0f);
            BVHNodes[nodeIndex].maxBounds = float4(maxBounds, 0.0f);
        }
    )";
}

const char* GPUBVHSystem::GetFrustumCullingShaderSource() {
    return R"(        struct BVHNode {
            float4 minBounds;
            float4 maxBounds;
            int leftChild;
            int rightChild;
            int objectIndex;
            int isLeaf;
        };
        
        struct ObjectData {
            float4 minBounds;
            float4 maxBounds;
            int objectIndex;
            int occludedFrameCount;
            int2 padding;
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
        
        StructuredBuffer<BVHNode> BVHNodes : register(t0);
        StructuredBuffer<ObjectData> Objects : register(t1);
        ConstantBuffer<Frustum> FrustumData : register(b0);
        ConstantBuffer<CullingParams> Params : register(b1);
        RWStructuredBuffer<int> Visibility : register(u0);
        
        bool IsBoxInFrustum(float3 minBounds, float3 maxBounds) {
            // Test AABB against all 6 frustum planes using positive vertex test
            for (int i = 0; i < 6; i++) {
                float4 plane = FrustumData.planes[i];
                
                // Find the positive vertex (corner of AABB furthest in plane normal direction)
                float3 positiveVertex;
                positiveVertex.x = (plane.x >= 0.0f) ? maxBounds.x : minBounds.x;
                positiveVertex.y = (plane.y >= 0.0f) ? maxBounds.y : minBounds.y;
                positiveVertex.z = (plane.z >= 0.0f) ? maxBounds.z : minBounds.z;
                
                // If positive vertex is outside plane, entire AABB is outside frustum
                float distance = dot(plane.xyz, positiveVertex) + plane.w;
                if (distance < 0.0f) {
                    return false;
                }
            }
            return true;
        }        [numthreads(64, 1, 1)]
        void main(uint3 id : SV_DispatchThreadID) {
            uint objectIndex = id.x;
            
            // Early exit if thread ID exceeds object count
            if (objectIndex >= (uint)Params.objectCount) {
                return;
            }
            
            // Initialize visibility to false (conservative approach)
            Visibility[objectIndex] = 0;
            
            // Get object data (bounds already checked above)
            ObjectData obj = Objects[objectIndex];
            
            // Skip heavily occluded objects to reduce GPU load
            if (obj.occludedFrameCount > 5) {
                return;
            }
              // Validate bounding box before testing
            if (any(isnan(obj.minBounds)) || any(isnan(obj.maxBounds)) ||
                any(obj.minBounds.xyz > obj.maxBounds.xyz)) {
                return; // Invalid bounds
            }
            
            // Test object's bounding box directly against frustum
            if (!IsBoxInFrustum(obj.minBounds.xyz, obj.maxBounds.xyz)) {
                return; // Outside frustum
            }
            
            // Object passed frustum test - mark as visible
            Visibility[objectIndex] = 1;
        }
    )";
}

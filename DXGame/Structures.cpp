#include "Structures.h"

void Frustum::ExtractFromMatrix(const Matrix& viewProjection) {
    XMFLOAT4X4 vp;
    // DirectXTK matrices are already in the correct format - no transpose needed
    XMStoreFloat4x4(&vp, viewProjection);
    
    // Extract frustum planes using Gribb/Hartmann method for right-handed system
    // Left plane (w + x >= 0)
    planes[0].x = vp._14 + vp._11;
    planes[0].y = vp._24 + vp._21;
    planes[0].z = vp._34 + vp._31;
    planes[0].w = vp._44 + vp._41;
    
    // Right plane (w - x >= 0)
    planes[1].x = vp._14 - vp._11;
    planes[1].y = vp._24 - vp._21;
    planes[1].z = vp._34 - vp._31;
    planes[1].w = vp._44 - vp._41;
    
    // Bottom plane (w + y >= 0)
    planes[2].x = vp._14 + vp._12;
    planes[2].y = vp._24 + vp._22;
    planes[2].z = vp._34 + vp._32;
    planes[2].w = vp._44 + vp._42;
    
    // Top plane (w - y >= 0)
    planes[3].x = vp._14 - vp._12;
    planes[3].y = vp._24 - vp._22;
    planes[3].z = vp._34 - vp._32;
    planes[3].w = vp._44 - vp._42;
    
    // Near plane (w + z >= 0) - Right-handed: near plane is positive Z
    planes[4].x = vp._14 + vp._13;
    planes[4].y = vp._24 + vp._23;
    planes[4].z = vp._34 + vp._33;
    planes[4].w = vp._44 + vp._43;
    
    // Far plane (w - z >= 0) - Right-handed: far plane is negative Z
    planes[5].x = vp._14 - vp._13;
    planes[5].y = vp._24 - vp._23;
    planes[5].z = vp._34 - vp._33;
    planes[5].w = vp._44 - vp._43;
    
    // Normalize planes (ensure they point inward to the frustum)
    for (int i = 0; i < 6; i++) {
        float length = sqrt(planes[i].x * planes[i].x + 
                          planes[i].y * planes[i].y + 
                          planes[i].z * planes[i].z);
        if (length > 0.0001f) {
            planes[i].x /= length;
            planes[i].y /= length;
            planes[i].z /= length;
            planes[i].w /= length;
        }
    }
}

bool Frustum::IsBoxInFrustum(const Vector3& minBounds, const Vector3& maxBounds) const {
    // For each frustum plane, test if the AABB is completely outside
    for (int i = 0; i < 6; i++) {
        // Find the "positive vertex" - the corner of the AABB that's furthest 
        // in the direction of the plane normal
        Vector3 positiveVertex;
        positiveVertex.x = (planes[i].x >= 0.0f) ? maxBounds.x : minBounds.x;
        positiveVertex.y = (planes[i].y >= 0.0f) ? maxBounds.y : minBounds.y;
        positiveVertex.z = (planes[i].z >= 0.0f) ? maxBounds.z : minBounds.z;
        
        // If the positive vertex is outside the plane, the entire AABB is outside
        float distance = planes[i].x * positiveVertex.x + 
                       planes[i].y * positiveVertex.y + 
                       planes[i].z * positiveVertex.z + 
                       planes[i].w;
        
        if (distance < 0.0f) {
            return false; // AABB is completely outside this frustum plane
        }
    }
    return true; // AABB is inside or intersects the frustum
}

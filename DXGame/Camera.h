#pragma once

#include "Common.h"

// ============================================================================
// FPS CAMERA CLASS
// ============================================================================

class FPSCamera {
public:
    // Camera properties
    Vector3 position = Vector3(0, 0, 0);
    Vector3 forward = Vector3(0, 0, 1);
    Vector3 up = Vector3(0, 1, 0);
    Vector3 right = Vector3(1, 0, 0);
    
    float yaw = 0.0f;
    float pitch = 0.0f;
    float mouseSensitivity = 0.1f;
    float moveSpeed = 5.0f;
    
    // Camera matrices
    Matrix GetViewMatrix() const;
    Matrix GetProjectionMatrix(float aspectRatio) const;
    
    // Input processing
    void ProcessInput(const DirectX::Keyboard::State& kb, float deltaTime);
    void ProcessMouse(float xOffset, float yOffset);
    
private:
    void UpdateVectors();
};

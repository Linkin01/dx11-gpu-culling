#include "Camera.h"

Matrix FPSCamera::GetViewMatrix() const {
    return Matrix::CreateLookAt(position, position + forward, up);
}

Matrix FPSCamera::GetProjectionMatrix(float aspectRatio) const {
    return Matrix::CreatePerspectiveFieldOfView(XM_PIDIV4, aspectRatio, 0.1f, 1000.0f);
}

void FPSCamera::UpdateVectors() {
    forward.x = cos(XMConvertToRadians(yaw)) * cos(XMConvertToRadians(pitch));
    forward.y = sin(XMConvertToRadians(pitch));
    forward.z = sin(XMConvertToRadians(yaw)) * cos(XMConvertToRadians(pitch));
    forward.Normalize();
    
    right = forward.Cross(Vector3::Up);
    right.Normalize();
    up = right.Cross(forward);
    up.Normalize();
}

void FPSCamera::ProcessInput(const DirectX::Keyboard::State& kb, float deltaTime) {
    float velocity = moveSpeed * deltaTime;
    
    if (kb.W) position += forward * velocity;
    if (kb.S) position -= forward * velocity;
    if (kb.A) position -= right * velocity;
    if (kb.D) position += right * velocity;
}

void FPSCamera::ProcessMouse(float xOffset, float yOffset) {
    xOffset *= mouseSensitivity;
    yOffset *= mouseSensitivity;
    
    yaw += xOffset;
    pitch += yOffset;
    
    // Constrain pitch
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    
    UpdateVectors();
}

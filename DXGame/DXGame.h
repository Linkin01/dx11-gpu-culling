#pragma once

#include "Common.h"
#include "Structures.h"
#include "Camera.h"
#include "GPUBVHSystem.h"
#include "CPUBVHSystem.h"

// ============================================================================
// MAIN APPLICATION CLASS
// ============================================================================

class DXGame {
public:
    DXGame() = default;
    ~DXGame() = default;

    // Application lifecycle
    bool Initialize(HWND hwnd, int width, int height);
    void Update();
    void Render();
    void OnResize(int width, int height);

private:
    // Window and device
    HWND m_hwnd = nullptr;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    ComPtr<ID3D11Texture2D> m_depthStencilBuffer;
    
    // DirectXTK objects
    std::unique_ptr<GeometricPrimitive> m_cube;
    std::unique_ptr<BasicEffect> m_effect;
    std::unique_ptr<CommonStates> m_states;
    std::unique_ptr<DirectX::Keyboard> m_keyboard;
    std::unique_ptr<DirectX::Mouse> m_mouse;
    
    // Camera and input
    FPSCamera m_camera;
    bool m_fpsMode = false;
    DirectX::Keyboard::KeyboardStateTracker m_keyTracker;
    DirectX::Mouse::ButtonStateTracker m_mouseTracker;
    
    // Render objects and culling
    std::vector<RenderObject> m_objects;
    Frustum m_frustum;
    
    // BVH systems
    std::unique_ptr<GPUBVHSystem> m_gpuBVH;
    std::unique_ptr<CPUBVHSystem> m_cpuBVH;
    bool m_useGPUBVH = true;
    bool m_bvhNeedsRebuild = true;
    Vector3 m_sceneMinBounds, m_sceneMaxBounds;
    
    // Timing
    std::chrono::high_resolution_clock::time_point m_lastTime;
    float m_deltaTime = 0.0f;
    
    // Window dimensions
    int m_width = 1024;
    int m_height = 768;
    
    // Initialization methods
    bool CreateDeviceAndSwapChain();
    bool CreateRenderTargetView();
    bool CreateDepthStencilView();
    bool InitializeDirectXTK();
    bool CreateRenderObjects();
    bool InitializeBVHSystems();
      // Update methods
    void UpdateInput();
    void UpdateCamera();
    void UpdateFrustum();
    void UpdateBVH();
    void UpdateCulling();
    void UpdateDynamicObjects();  // New method for object animation
    void UpdateSceneBounds();  // Dynamic scene bounds calculation
    
    // Culling methods
    void PerformCulling();
    void ProcessOcclusionQueries();
      // Utility methods
    void CalculateSceneBounds();
    void CreateOcclusionQueries();
    void UpdateObjectBounds(RenderObject& obj);  // Update bounds from world matrix
    Vector3 GetObjectPosition(const Matrix& worldMatrix);  // Extract position from matrix
};

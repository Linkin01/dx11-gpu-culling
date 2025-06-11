#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <chrono>

// DirectXTK Headers
#include "SimpleMath.h"
#include "GeometricPrimitive.h"
#include "Effects.h"
#include "CommonStates.h"
#include "DirectXHelpers.h"
#include "Keyboard.h"
#include "Mouse.h"

using namespace DirectX;
using namespace DirectX::SimpleMath;
using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

// Forward declarations
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// BVH Node Structure
struct BVHNode {
    Vector3 minBounds;
    Vector3 maxBounds;
    int leftChild = -1;
    int rightChild = -1;
    int objectIndex = -1; // For leaf nodes
    bool isLeaf = false;
};

// GPU-aligned BVH Node for compute shader
struct GPUBVHNode {
    float minBounds[3];
    float maxBounds[3];
    int leftChild;
    int rightChild;
    int objectIndex;
    int isLeaf;
    float padding[2]; // Ensure 16-byte alignment
};

// GPU-aligned Object data for compute shader
struct GPUObjectData {
    float minBounds[3];
    float maxBounds[3];
    int objectIndex;
    int occludedFrameCount;
};

// GPU Frustum data
struct GPUFrustum {
    float planes[6][4]; // 6 planes, each with 4 components (x,y,z,w)
};

// Culling parameters
struct CullingParams {
    int rootNodeIndex;
    int objectCount;
    int nodeCount;
    int padding;
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
};

// Camera class for FPS controls
class FPSCamera {
public:
    Vector3 position = Vector3(0, 0, -5);
    Vector3 forward = Vector3(0, 0, 1);
    Vector3 up = Vector3(0, 1, 0);
    Vector3 right = Vector3(1, 0, 0);
    
    float yaw = 0.0f;
    float pitch = 0.0f;
    float mouseSensitivity = 0.1f;
    float moveSpeed = 5.0f;
    
    Matrix GetViewMatrix() const {
        return Matrix::CreateLookAt(position, position + forward, up);
    }
    
    Matrix GetProjectionMatrix(float aspectRatio) const {
        return Matrix::CreatePerspectiveFieldOfView(XM_PIDIV4, aspectRatio, 0.1f, 1000.0f);
    }
    
    void UpdateVectors() {
        forward.x = cos(XMConvertToRadians(yaw)) * cos(XMConvertToRadians(pitch));
        forward.y = sin(XMConvertToRadians(pitch));
        forward.z = sin(XMConvertToRadians(yaw)) * cos(XMConvertToRadians(pitch));
        forward.Normalize();
        
        right = forward.Cross(Vector3::Up);
        right.Normalize();
        up = right.Cross(forward);
        up.Normalize();
    }
    
    void ProcessInput(const DirectX::Keyboard::State& kb, float deltaTime) {
        float velocity = moveSpeed * deltaTime;
        
        if (kb.W) position += forward * velocity;
        if (kb.S) position -= forward * velocity;
        if (kb.A) position -= right * velocity;
        if (kb.D) position += right * velocity;
    }
    
    void ProcessMouse(float xOffset, float yOffset) {
        xOffset *= mouseSensitivity;
        yOffset *= mouseSensitivity;
        
        yaw += xOffset;
        pitch += yOffset;
        
        // Constrain pitch
        if (pitch > 89.0f) pitch = 89.0f;
        if (pitch < -89.0f) pitch = -89.0f;
        
        UpdateVectors();
    }
};

// Frustum structure for culling
struct Frustum {
    XMFLOAT4 planes[6]; // left, right, top, bottom, near, far
      void ExtractFromMatrix(const Matrix& viewProjection) {
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
      bool IsBoxInFrustum(const Vector3& minBounds, const Vector3& maxBounds) const {
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
};

// Main Application Class
class DXGame {
private:
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
      // Render objects and BVH
    std::vector<RenderObject> m_objects;
    std::vector<BVHNode> m_bvhNodes;
    int m_rootNode = -1;
    Frustum m_frustum;
    
    // GPU Frustum Culling Resources
    ComPtr<ID3D11ComputeShader> m_frustumCullingCS;
    ComPtr<ID3D11Buffer> m_bvhNodesBuffer;
    ComPtr<ID3D11Buffer> m_objectsBuffer;
    ComPtr<ID3D11Buffer> m_visibilityBuffer;
    ComPtr<ID3D11Buffer> m_frustumBuffer;
    ComPtr<ID3D11Buffer> m_cullingParamsBuffer;
    ComPtr<ID3D11ShaderResourceView> m_bvhNodesSRV;
    ComPtr<ID3D11ShaderResourceView> m_objectsSRV;
    ComPtr<ID3D11UnorderedAccessView> m_visibilityUAV;
    ComPtr<ID3D11Buffer> m_visibilityReadbackBuffer;
    
    // Timing
    std::chrono::high_resolution_clock::time_point m_lastTime;
    float m_deltaTime = 0.0f;
    
    // Window dimensions
    int m_width = 1024;
    int m_height = 768;
    
public:
    bool Initialize(HWND hwnd, int width, int height) {
        m_hwnd = hwnd;
        m_width = width;
        m_height = height;
          if (!CreateDeviceAndSwapChain()) return false;
        if (!CreateRenderTargetView()) return false;
        if (!CreateDepthStencilView()) return false;
        if (!InitializeDirectXTK()) return false;
        if (!CreateRenderObjects()) return false;
        
        BuildBVH();
        
        // Initialize GPU frustum culling
        if (!InitializeGPUFrustumCulling()) return false;
        
        // Set initial viewport
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);
        
        m_lastTime = std::chrono::high_resolution_clock::now();
        
        return true;
    }
    
    void Update() {
        // Calculate delta time
        auto currentTime = std::chrono::high_resolution_clock::now();
        m_deltaTime = std::chrono::duration<float>(currentTime - m_lastTime).count();
        m_lastTime = currentTime;
        
        // Update input
        auto kb = m_keyboard->GetState();
        auto mouse = m_mouse->GetState();
        m_keyTracker.Update(kb);
        m_mouseTracker.Update(mouse);
        
        // Toggle FPS mode
        if (m_keyTracker.IsKeyPressed(DirectX::Keyboard::F1)) {
            m_fpsMode = !m_fpsMode;
            if (m_fpsMode) {
                ShowCursor(FALSE);
                RECT rect;
                GetClientRect(m_hwnd, &rect);
                POINT center = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
                ClientToScreen(m_hwnd, &center);
                SetCursorPos(center.x, center.y);
            } else {
                ShowCursor(TRUE);
            }
        }
        
        // Update camera
        if (m_fpsMode) {
            m_camera.ProcessInput(kb, m_deltaTime);
            
            // Mouse sensitivity adjustment
            if (kb.PageUp && m_camera.mouseSensitivity < 2.0f) {
                m_camera.mouseSensitivity += 0.01f;
            }
            if (kb.PageDown && m_camera.mouseSensitivity > 0.01f) {
                m_camera.mouseSensitivity -= 0.01f;
            }
            
            // Mouse look
            static int lastX = mouse.x;
            static int lastY = mouse.y;
            
            float xOffset = static_cast<float>(mouse.x - lastX);
            float yOffset = static_cast<float>(lastY - mouse.y); // Reversed since y-coordinates go from bottom to top
            
            m_camera.ProcessMouse(xOffset, yOffset);
            
            // Center cursor
            RECT rect;
            GetClientRect(m_hwnd, &rect);
            POINT center = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
            ClientToScreen(m_hwnd, &center);
            SetCursorPos(center.x, center.y);
            lastX = (rect.right - rect.left) / 2;
            lastY = (rect.bottom - rect.top) / 2;
        }
          // Update frustum
        Matrix view = m_camera.GetViewMatrix();
        Matrix projection = m_camera.GetProjectionMatrix(static_cast<float>(m_width) / m_height);
        m_frustum.ExtractFromMatrix(view * projection);
          // Perform GPU frustum culling using BVH (fallback to CPU if GPU not available)
        if (m_frustumCullingCS) {
            PerformGPUFrustumCulling();
        } else {
            PerformCPUFrustumCulling();
        }
        
        // Process occlusion query results
        ProcessOcclusionQueries();
    }
      void Render() {
        // Clear render target and depth buffer
        const float clearColor[4] = { 0.2f, 0.3f, 0.4f, 1.0f };
        m_context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
        m_context->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        
        // Set render target and depth buffer
        m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());
        
        // Set up matrices
        Matrix view = m_camera.GetViewMatrix();
        Matrix projection = m_camera.GetProjectionMatrix(static_cast<float>(m_width) / m_height);
        
        m_effect->SetView(view);
        m_effect->SetProjection(projection);
        
        // Render all frustum-culled objects and handle occlusion queries
        for (auto& obj : m_objects) {
            if (obj.visible) {
                // Start occlusion query for this object (for next frame)
                bool shouldStartQuery = obj.occlusionQuery && !obj.queryInProgress;
                
                if (shouldStartQuery) {
                    m_context->Begin(obj.occlusionQuery.Get());
                    obj.queryInProgress = true;
                }
                
                // Render the object
                m_cube->Draw(obj.world, view, projection);
                
                // End occlusion query
                if (shouldStartQuery) {
                    m_context->End(obj.occlusionQuery.Get());
                }
            }
        }
        
        // Present
        m_swapChain->Present(1, 0);
    }
    
    void OnResize(int width, int height) {
        if (width <= 0 || height <= 0) return;
        
        m_width = width;
        m_height = height;
        
        // Release old views
        m_renderTargetView.Reset();
        m_depthStencilView.Reset();
        m_depthStencilBuffer.Reset();
        
        // Resize swap chain
        HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return;
        
        // Recreate views
        CreateRenderTargetView();
        CreateDepthStencilView();
        
        // Update viewport
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);
    }
    
private:
    bool CreateDeviceAndSwapChain() {
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.BufferDesc.Width = m_width;
        swapChainDesc.BufferDesc.Height = m_height;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow = m_hwnd;
        swapChainDesc.SampleDesc.Count = 4; // MSAA 4x
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        
        UINT createDeviceFlags = 0;
#ifdef _DEBUG
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &m_swapChain,
            &m_device,
            nullptr,
            &m_context
        );
        
        return SUCCEEDED(hr);
    }
    
    bool CreateRenderTargetView() {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) return false;
        
        hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);
        return SUCCEEDED(hr);
    }
    
    bool CreateDepthStencilView() {
        D3D11_TEXTURE2D_DESC depthStencilDesc = {};
        depthStencilDesc.Width = m_width;
        depthStencilDesc.Height = m_height;
        depthStencilDesc.MipLevels = 1;
        depthStencilDesc.ArraySize = 1;
        depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthStencilDesc.SampleDesc.Count = 4; // MSAA 4x
        depthStencilDesc.SampleDesc.Quality = 0;
        depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
        depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        
        HRESULT hr = m_device->CreateTexture2D(&depthStencilDesc, nullptr, &m_depthStencilBuffer);
        if (FAILED(hr)) return false;
        
        hr = m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), nullptr, &m_depthStencilView);
        return SUCCEEDED(hr);
    }
    
    bool InitializeDirectXTK() {
        m_cube = GeometricPrimitive::CreateCube(m_context.Get());
        m_effect = std::make_unique<BasicEffect>(m_device.Get());
        m_states = std::make_unique<CommonStates>(m_device.Get());
        m_keyboard = std::make_unique<DirectX::Keyboard>();
        m_mouse = std::make_unique<DirectX::Mouse>();
        
        m_mouse->SetWindow(m_hwnd);
        
        m_effect->EnableDefaultLighting();
        
        return true;
    }
      bool CreateRenderObjects() {
        // Create multiple test cubes in different positions
        m_objects.resize(6);
        
        // Front cubes
        m_objects[0].world = Matrix::CreateTranslation(Vector3(-2, 0, 5));
        m_objects[0].minBounds = Vector3(-3, -1, 4);
        m_objects[0].maxBounds = Vector3(-1, 1, 6);
        
        m_objects[1].world = Matrix::CreateTranslation(Vector3(2, 0, 5));
        m_objects[1].minBounds = Vector3(1, -1, 4);
        m_objects[1].maxBounds = Vector3(3, 1, 6);
        
        // Middle cubes
        m_objects[2].world = Matrix::CreateTranslation(Vector3(0, 2, 10));
        m_objects[2].minBounds = Vector3(-1, 1, 9);
        m_objects[2].maxBounds = Vector3(1, 3, 11);
        
        m_objects[3].world = Matrix::CreateTranslation(Vector3(0, -2, 10));
        m_objects[3].minBounds = Vector3(-1, -3, 9);
        m_objects[3].maxBounds = Vector3(1, -1, 11);
        
        // Far cubes
        m_objects[4].world = Matrix::CreateTranslation(Vector3(-4, 0, 20));
        m_objects[4].minBounds = Vector3(-5, -1, 19);
        m_objects[4].maxBounds = Vector3(-3, 1, 21);
        
        m_objects[5].world = Matrix::CreateTranslation(Vector3(4, 0, 20));
        m_objects[5].minBounds = Vector3(3, -1, 19);
        m_objects[5].maxBounds = Vector3(5, 1, 21);
        
        // Create occlusion queries
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_OCCLUSION;
        
        for (auto& obj : m_objects) {
            HRESULT hr = m_device->CreateQuery(&queryDesc, &obj.occlusionQuery);
            if (FAILED(hr)) return false;
            
            // Initialize all objects as visible
            obj.visible = true;
            obj.occludedFrameCount = 0;
        }
        
        return true;
    }
    
    void BuildBVH() {
        if (m_objects.empty()) return;
        
        m_bvhNodes.clear();
        m_bvhNodes.reserve(m_objects.size() * 2);
        
        // Create leaf nodes
        std::vector<int> objectIndices;
        for (size_t i = 0; i < m_objects.size(); ++i) {
            BVHNode leafNode;
            leafNode.minBounds = m_objects[i].minBounds;
            leafNode.maxBounds = m_objects[i].maxBounds;
            leafNode.objectIndex = static_cast<int>(i);
            leafNode.isLeaf = true;
            
            m_bvhNodes.push_back(leafNode);
            objectIndices.push_back(static_cast<int>(m_bvhNodes.size() - 1));
        }
        
        // Build tree recursively
        m_rootNode = BuildBVHRecursive(objectIndices);
    }
    
    int BuildBVHRecursive(std::vector<int>& nodeIndices) {
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
    }    // CPU Frustum Culling Methods (replaced with GPU implementation)
    /*
    void PerformFrustumCulling() {
        // Reset visibility for frustum culling, but preserve occlusion state
        for (auto& obj : m_objects) {
            // Only reset to visible if the object isn't heavily occluded
            if (obj.occludedFrameCount <= 3) {
                obj.visible = false; // Will be set to true if in frustum
            }
        }
        
        // Traverse BVH and perform frustum culling
        if (m_rootNode >= 0) {
            FrustumCullBVH(m_rootNode);
        }
    }
    
    void FrustumCullBVH(int nodeIndex) {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_bvhNodes.size())) return;
        
        const auto& node = m_bvhNodes[nodeIndex];
        
        // Check if node is in frustum
        if (!m_frustum.IsBoxInFrustum(node.minBounds, node.maxBounds)) {
            return; // Node is outside frustum, skip entire subtree
        }
        
        if (node.isLeaf) {
            // Mark object as visible
            if (node.objectIndex >= 0 && node.objectIndex < static_cast<int>(m_objects.size())) {
                m_objects[node.objectIndex].visible = true;
            }
        } else {
            // Recursively check children
            FrustumCullBVH(node.leftChild);
            FrustumCullBVH(node.rightChild);
        }
    }
    */void ProcessOcclusionQueries() {
        for (auto& obj : m_objects) {
            if (obj.occlusionQuery && obj.queryInProgress) {
                UINT64 result = 0;
                HRESULT hr = m_context->GetData(obj.occlusionQuery.Get(), &result, sizeof(result), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                
                if (hr == S_OK) {
                    obj.lastQueryResult = result;
                    obj.queryInProgress = false;
                    
                    // Use the object's own counter instead of static vector
                    if (result == 0) {
                        obj.occludedFrameCount++;
                        // Only hide after being occluded for 3+ consecutive frames
                        if (obj.occludedFrameCount > 3) {
                            obj.visible = false;
                        }
                    } else {                        // Reset counter if object becomes visible
                        obj.occludedFrameCount = 0;
                        // Don't immediately set to visible here - let frustum culling handle it
                    }
                }
            }
        }
    }
      // GPU Frustum Culling Methods
    bool InitializeGPUFrustumCulling() {
        // Check if compute shaders are supported
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hwopts = {};
        HRESULT hr = m_device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &hwopts, sizeof(hwopts));
        if (FAILED(hr)) {
            OutputDebugStringA("Compute shaders not supported, falling back to CPU culling\n");
            return true; // Don't fail, just use CPU culling
        }
        
        // Create compute shader
        if (!CreateFrustumCullingComputeShader()) {
            OutputDebugStringA("Failed to create compute shader, falling back to CPU culling\n");
            return true; // Don't fail, just use CPU culling
        }
        
        // Create GPU buffers
        if (!CreateGPUBuffers()) {
            OutputDebugStringA("Failed to create GPU buffers, falling back to CPU culling\n");
            return true; // Don't fail, just use CPU culling
        }
        
        OutputDebugStringA("GPU frustum culling initialized successfully\n");
        return true;
    }
      bool CreateFrustumCullingComputeShader() {
        // Compute shader HLSL code
        const char* csSource = R"(
            struct BVHNode {
                float3 minBounds;
                float3 maxBounds;
                int leftChild;
                int rightChild;
                int objectIndex;
                int isLeaf;
                float2 padding;
            };
            
            struct ObjectData {
                float3 minBounds;
                float3 maxBounds;
                int objectIndex;
                int occludedFrameCount;
            };
            
            struct Frustum {
                float4 planes[6];
            };
            
            struct CullingParams {
                int rootNodeIndex;
                int objectCount;
                int nodeCount;
                int padding;
            };
            
            StructuredBuffer<BVHNode> BVHNodes : register(t0);
            StructuredBuffer<ObjectData> Objects : register(t1);
            ConstantBuffer<Frustum> FrustumData : register(b0);
            ConstantBuffer<CullingParams> Params : register(b1);
            RWStructuredBuffer<int> Visibility : register(u0);
            
            bool IsBoxInFrustum(float3 minBounds, float3 maxBounds) {
                for (int i = 0; i < 6; i++) {
                    float4 plane = FrustumData.planes[i];
                    float3 positiveVertex;
                    positiveVertex.x = (plane.x >= 0.0f) ? maxBounds.x : minBounds.x;
                    positiveVertex.y = (plane.y >= 0.0f) ? maxBounds.y : minBounds.y;
                    positiveVertex.z = (plane.z >= 0.0f) ? maxBounds.z : minBounds.z;
                    
                    float distance = dot(plane.xyz, positiveVertex) + plane.w;
                    if (distance < 0.0f) {
                        return false;
                    }
                }
                return true;
            }
            
            [numthreads(1, 1, 1)]
            void main(uint3 id : SV_DispatchThreadID) {
                if (id.x == 0) {
                    // Clear visibility buffer
                    for (int i = 0; i < Params.objectCount; i++) {
                        ObjectData obj = Objects[i];
                        // Only reset if not heavily occluded
                        if (obj.occludedFrameCount <= 3) {
                            Visibility[i] = 0;
                        }
                    }
                    
                    // Iterative BVH traversal using a simple stack
                    int stack[64]; // Simple stack for BVH traversal
                    int stackPtr = 0;
                    
                    if (Params.rootNodeIndex >= 0) {
                        stack[stackPtr++] = Params.rootNodeIndex;
                        
                        while (stackPtr > 0 && stackPtr < 64) {
                            int nodeIndex = stack[--stackPtr];
                            
                            if (nodeIndex < 0 || nodeIndex >= Params.nodeCount) continue;
                            
                            BVHNode node = BVHNodes[nodeIndex];
                            
                            if (!IsBoxInFrustum(node.minBounds, node.maxBounds)) {
                                continue;
                            }
                            
                            if (node.isLeaf) {
                                if (node.objectIndex >= 0 && node.objectIndex < Params.objectCount) {
                                    ObjectData obj = Objects[node.objectIndex];
                                    // Only set visible if not heavily occluded
                                    if (obj.occludedFrameCount <= 3) {
                                        Visibility[node.objectIndex] = 1;
                                    }
                                }
                            } else {
                                // Add children to stack
                                if (node.rightChild >= 0 && stackPtr < 63) {
                                    stack[stackPtr++] = node.rightChild;
                                }
                                if (node.leftChild >= 0 && stackPtr < 63) {
                                    stack[stackPtr++] = node.leftChild;
                                }
                            }
                        }
                    }
                }
            }
        )";
        
        ComPtr<ID3DBlob> csBlob;
        ComPtr<ID3DBlob> errorBlob;
        
        HRESULT hr = D3DCompile(
            csSource,
            strlen(csSource),
            nullptr,
            nullptr,
            nullptr,
            "main",
            "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            &csBlob,
            &errorBlob
        );
        
        if (FAILED(hr)) {
            if (errorBlob) {
                OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            return false;
        }
        
        hr = m_device->CreateComputeShader(
            csBlob->GetBufferPointer(),
            csBlob->GetBufferSize(),
            nullptr,
            &m_frustumCullingCS
        );
        
        return SUCCEEDED(hr);
    }
    
    bool CreateGPUBuffers() {
        // Create BVH nodes buffer
        if (!m_bvhNodes.empty()) {
            std::vector<GPUBVHNode> gpuNodes(m_bvhNodes.size());
            for (size_t i = 0; i < m_bvhNodes.size(); i++) {
                const auto& node = m_bvhNodes[i];
                auto& gpuNode = gpuNodes[i];
                
                gpuNode.minBounds[0] = node.minBounds.x;
                gpuNode.minBounds[1] = node.minBounds.y;
                gpuNode.minBounds[2] = node.minBounds.z;
                gpuNode.maxBounds[0] = node.maxBounds.x;
                gpuNode.maxBounds[1] = node.maxBounds.y;
                gpuNode.maxBounds[2] = node.maxBounds.z;
                gpuNode.leftChild = node.leftChild;
                gpuNode.rightChild = node.rightChild;
                gpuNode.objectIndex = node.objectIndex;
                gpuNode.isLeaf = node.isLeaf ? 1 : 0;
            }
            
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
            bufferDesc.ByteWidth = static_cast<UINT>(sizeof(GPUBVHNode) * gpuNodes.size());
            bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bufferDesc.StructureByteStride = sizeof(GPUBVHNode);
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            
            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem = gpuNodes.data();
            
            HRESULT hr = m_device->CreateBuffer(&bufferDesc, &initData, &m_bvhNodesBuffer);
            if (FAILED(hr)) return false;
              // Create SRV
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = static_cast<UINT>(gpuNodes.size());
            
            hr = m_device->CreateShaderResourceView(m_bvhNodesBuffer.Get(), &srvDesc, &m_bvhNodesSRV);
            if (FAILED(hr)) return false;
        }
        
        // Create objects buffer (dynamic for updates)
        if (!m_objects.empty()) {
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
            bufferDesc.ByteWidth = static_cast<UINT>(sizeof(GPUObjectData) * m_objects.size());
            bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bufferDesc.StructureByteStride = sizeof(GPUObjectData);
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            
            HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_objectsBuffer);
            if (FAILED(hr)) return false;
              // Create SRV
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = static_cast<UINT>(m_objects.size());
            
            hr = m_device->CreateShaderResourceView(m_objectsBuffer.Get(), &srvDesc, &m_objectsSRV);
            if (FAILED(hr)) return false;
        }
        
        // Create visibility buffer
        if (!m_objects.empty()) {
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.Usage = D3D11_USAGE_DEFAULT;
            bufferDesc.ByteWidth = static_cast<UINT>(sizeof(int) * m_objects.size());
            bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bufferDesc.StructureByteStride = sizeof(int);
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            
            HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_visibilityBuffer);
            if (FAILED(hr)) return false;
            
            // Create UAV
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = static_cast<UINT>(m_objects.size());
            
            hr = m_device->CreateUnorderedAccessView(m_visibilityBuffer.Get(), &uavDesc, &m_visibilityUAV);
            if (FAILED(hr)) return false;
            
            // Create readback buffer
            bufferDesc.Usage = D3D11_USAGE_STAGING;
            bufferDesc.BindFlags = 0;
            bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bufferDesc.MiscFlags = 0;
            
            hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_visibilityReadbackBuffer);
            if (FAILED(hr)) return false;
        }
        
        // Create frustum constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.ByteWidth = sizeof(GPUFrustum);
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        
        HRESULT hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_frustumBuffer);
        if (FAILED(hr)) return false;
        
        // Create culling params constant buffer
        cbDesc.ByteWidth = sizeof(CullingParams);
        hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_cullingParamsBuffer);
        if (FAILED(hr)) return false;
        
        return true;
    }
    
    void UpdateGPUObjectData() {
        if (!m_objectsBuffer || m_objects.empty()) return;
        
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_context->Map(m_objectsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            GPUObjectData* gpuObjects = static_cast<GPUObjectData*>(mapped.pData);
            
            for (size_t i = 0; i < m_objects.size(); i++) {
                const auto& obj = m_objects[i];
                auto& gpuObj = gpuObjects[i];
                
                gpuObj.minBounds[0] = obj.minBounds.x;
                gpuObj.minBounds[1] = obj.minBounds.y;
                gpuObj.minBounds[2] = obj.minBounds.z;
                gpuObj.maxBounds[0] = obj.maxBounds.x;
                gpuObj.maxBounds[1] = obj.maxBounds.y;
                gpuObj.maxBounds[2] = obj.maxBounds.z;
                gpuObj.objectIndex = static_cast<int>(i);
                gpuObj.occludedFrameCount = obj.occludedFrameCount;
            }
            
            m_context->Unmap(m_objectsBuffer.Get(), 0);
        }
    }
      void PerformGPUFrustumCulling() {
        if (!m_frustumCullingCS || m_objects.empty() || !m_bvhNodesBuffer || !m_objectsBuffer) {
            // Fall back to CPU culling if GPU resources aren't available
            PerformCPUFrustumCulling();
            return;
        }
        
        // Update object data on GPU
        UpdateGPUObjectData();
        
        // Update frustum constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_context->Map(m_frustumBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            GPUFrustum* gpuFrustum = static_cast<GPUFrustum*>(mapped.pData);
            for (int i = 0; i < 6; i++) {
                gpuFrustum->planes[i][0] = m_frustum.planes[i].x;
                gpuFrustum->planes[i][1] = m_frustum.planes[i].y;
                gpuFrustum->planes[i][2] = m_frustum.planes[i].z;
                gpuFrustum->planes[i][3] = m_frustum.planes[i].w;
            }
            m_context->Unmap(m_frustumBuffer.Get(), 0);
        }
        
        // Update culling params
        hr = m_context->Map(m_cullingParamsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            CullingParams* params = static_cast<CullingParams*>(mapped.pData);
            params->rootNodeIndex = m_rootNode;
            params->objectCount = static_cast<int>(m_objects.size());
            params->nodeCount = static_cast<int>(m_bvhNodes.size());
            params->padding = 0;
            m_context->Unmap(m_cullingParamsBuffer.Get(), 0);
        }
        
        // Set compute shader and resources
        m_context->CSSetShader(m_frustumCullingCS.Get(), nullptr, 0);
        
        ID3D11ShaderResourceView* srvs[] = { m_bvhNodesSRV.Get(), m_objectsSRV.Get() };
        m_context->CSSetShaderResources(0, 2, srvs);
        
        ID3D11Buffer* cbs[] = { m_frustumBuffer.Get(), m_cullingParamsBuffer.Get() };
        m_context->CSSetConstantBuffers(0, 2, cbs);
        
        ID3D11UnorderedAccessView* uavs[] = { m_visibilityUAV.Get() };
        UINT initialCounts[] = { 0 };
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);
        
        // Dispatch compute shader
        m_context->Dispatch(1, 1, 1);
        
        // Unbind resources
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        m_context->CSSetShaderResources(0, 2, nullSRVs);
        
        ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        
        m_context->CSSetShader(nullptr, nullptr, 0);
        
        // Copy visibility results back to CPU
        m_context->CopyResource(m_visibilityReadbackBuffer.Get(), m_visibilityBuffer.Get());
        
        // Read back visibility results
        hr = m_context->Map(m_visibilityReadbackBuffer.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(hr)) {
            const int* visibility = static_cast<const int*>(mapped.pData);
            for (size_t i = 0; i < m_objects.size(); i++) {
                m_objects[i].visible = (visibility[i] != 0);            }
            m_context->Unmap(m_visibilityReadbackBuffer.Get(), 0);
        }
    }
    
    // CPU Fallback Frustum Culling
    void PerformCPUFrustumCulling() {
        // Reset visibility for frustum culling, but preserve occlusion state
        for (auto& obj : m_objects) {
            // Only reset to visible if the object isn't heavily occluded
            if (obj.occludedFrameCount <= 3) {
                obj.visible = false; // Will be set to true if in frustum
            }
        }
        
        // Traverse BVH and perform frustum culling
        if (m_rootNode >= 0) {
            FrustumCullBVH(m_rootNode);
        }
    }
    
    void FrustumCullBVH(int nodeIndex) {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_bvhNodes.size())) return;
        
        const auto& node = m_bvhNodes[nodeIndex];
        
        // Check if node is in frustum
        if (!m_frustum.IsBoxInFrustum(node.minBounds, node.maxBounds)) {
            return; // Node is outside frustum, skip entire subtree
        }
        
        if (node.isLeaf) {
            // Mark object as visible
            if (node.objectIndex >= 0 && node.objectIndex < static_cast<int>(m_objects.size())) {
                m_objects[node.objectIndex].visible = true;
            }
        } else {
            // Recursively check children
            FrustumCullBVH(node.leftChild);
            FrustumCullBVH(node.rightChild);
        }
    }
};

// Global variables
std::unique_ptr<DXGame> g_game;

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_SIZE:
        if (g_game && wParam != SIZE_MINIMIZED) {
            g_game->OnResize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
        
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
        
    case WM_ACTIVATEAPP:
        DirectX::Keyboard::ProcessMessage(uMsg, wParam, lParam);
        DirectX::Mouse::ProcessMessage(uMsg, wParam, lParam);
        return 0;
          case WM_KEYDOWN:
        // Handle ESC key directly in WindowProc as fallback
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
            return 0;
        }
        DirectX::Keyboard::ProcessMessage(uMsg, wParam, lParam);
        return 0;
        
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        DirectX::Keyboard::ProcessMessage(uMsg, wParam, lParam);
        return 0;
        
    case WM_INPUT:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEHOVER:
        DirectX::Mouse::ProcessMessage(uMsg, wParam, lParam);
        return 0;
    }
    
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DXGameWindow";
    
    if (!RegisterClassEx(&wc)) {
        MessageBox(nullptr, L"Failed to register window class", L"Error", MB_OK);
        return -1;
    }
    
    // Create window
    HWND hwnd = CreateWindowEx(
        0,
        L"DXGameWindow",
        L"DirectX 11 - GPU Occlusion Queries & Frustum Culling with BVH",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1024, 768,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );
    
    if (!hwnd) {
        MessageBox(nullptr, L"Failed to create window", L"Error", MB_OK);
        return -1;  
    }
    
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    // Initialize game
    g_game = std::make_unique<DXGame>();
    if (!g_game->Initialize(hwnd, 1024, 768)) {
        MessageBox(nullptr, L"Failed to initialize DirectX", L"Error", MB_OK);
        return -1;
    }
    
    // Main message loop
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            g_game->Update();
            g_game->Render();
        }
    }
    
    return static_cast<int>(msg.wParam);
}

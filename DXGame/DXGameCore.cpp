#define NOMINMAX
#include "DXGame.h"

// Global variables
std::unique_ptr<DXGame> g_game;

// Forward declarations
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// INITIALIZATION METHODS
// ============================================================================

bool DXGame::Initialize(HWND hwnd, int width, int height) {
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    if (!CreateDeviceAndSwapChain()) return false;
    if (!CreateRenderTargetView()) return false;
    if (!CreateDepthStencilView()) return false;
    if (!InitializeDirectXTK()) return false;
    if (!CreateRenderObjects()) return false;
    if (!InitializeBVHSystems()) return false;

    // Calculate scene bounds for BVH construction
    CalculateSceneBounds();

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

bool DXGame::CreateDeviceAndSwapChain() {
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = m_width;
    swapChainDesc.BufferDesc.Height = m_height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = m_hwnd;
    swapChainDesc.SampleDesc.Count = Config::MSAA_SAMPLES;
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

bool DXGame::CreateRenderTargetView() {
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);
    return SUCCEEDED(hr);
}

bool DXGame::CreateDepthStencilView() {
    D3D11_TEXTURE2D_DESC depthStencilDesc = {};
    depthStencilDesc.Width = m_width;
    depthStencilDesc.Height = m_height;
    depthStencilDesc.MipLevels = 1;
    depthStencilDesc.ArraySize = 1;
    depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthStencilDesc.SampleDesc.Count = Config::MSAA_SAMPLES;
    depthStencilDesc.SampleDesc.Quality = 0;
    depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
    depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = m_device->CreateTexture2D(&depthStencilDesc, nullptr, &m_depthStencilBuffer);
    if (FAILED(hr)) return false;

    hr = m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), nullptr, &m_depthStencilView);
    return SUCCEEDED(hr);
}

bool DXGame::InitializeDirectXTK() {
    m_cube = GeometricPrimitive::CreateCube(m_context.Get());
    m_effect = std::make_unique<BasicEffect>(m_device.Get());
    m_states = std::make_unique<CommonStates>(m_device.Get());
    m_keyboard = std::make_unique<DirectX::Keyboard>();
    m_mouse = std::make_unique<DirectX::Mouse>();

    m_mouse->SetWindow(m_hwnd);
    m_effect->EnableDefaultLighting();

    return true;
}

bool DXGame::CreateRenderObjects() {
    // Create test cubes arranged vertically for better occlusion testing
    m_objects.resize(12);  // Increased to include dynamic objects

    // Bottom row cubes (Y = -2) - these should occlude upper cubes when looking from below
    m_objects[0].world = Matrix::CreateTranslation(Vector3(-4, -2, 10));
    m_objects[0].minBounds = Vector3(-5, -3, 9);
    m_objects[0].maxBounds = Vector3(-3, -1, 11);
    m_objects[0].baseSize = Vector3(2, 2, 2);

    m_objects[1].world = Matrix::CreateTranslation(Vector3(0, -2, 10));
    m_objects[1].minBounds = Vector3(-1, -3, 9);
    m_objects[1].maxBounds = Vector3(1, -1, 11);
    m_objects[1].baseSize = Vector3(2, 2, 2);

    m_objects[2].world = Matrix::CreateTranslation(Vector3(4, -2, 10));
    m_objects[2].minBounds = Vector3(3, -3, 9);
    m_objects[2].maxBounds = Vector3(5, -1, 11);
    m_objects[2].baseSize = Vector3(2, 2, 2);

    // Middle row cubes (Y = 0)
    m_objects[3].world = Matrix::CreateTranslation(Vector3(-4, 0, 10));
    m_objects[3].minBounds = Vector3(-5, -1, 9);
    m_objects[3].maxBounds = Vector3(-3, 1, 11);
    m_objects[3].baseSize = Vector3(2, 2, 2);

    m_objects[4].world = Matrix::CreateTranslation(Vector3(0, 0, 10));
    m_objects[4].minBounds = Vector3(-1, -1, 9);
    m_objects[4].maxBounds = Vector3(1, 1, 11);
    m_objects[4].baseSize = Vector3(2, 2, 2);

    m_objects[5].world = Matrix::CreateTranslation(Vector3(4, 0, 10));
    m_objects[5].minBounds = Vector3(3, -1, 9);
    m_objects[5].maxBounds = Vector3(5, 1, 11);
    m_objects[5].baseSize = Vector3(2, 2, 2);

    // Top row cubes (Y = 2) - these should be occluded when looking from below
    m_objects[6].world = Matrix::CreateTranslation(Vector3(-4, 2, 10));
    m_objects[6].minBounds = Vector3(-5, 1, 9);
    m_objects[6].maxBounds = Vector3(-3, 3, 11);
    m_objects[6].baseSize = Vector3(2, 2, 2);

    m_objects[7].world = Matrix::CreateTranslation(Vector3(0, 2, 10));
    m_objects[7].minBounds = Vector3(-1, 1, 9);
    m_objects[7].maxBounds = Vector3(1, 3, 11);
    m_objects[7].baseSize = Vector3(2, 2, 2);

    m_objects[8].world = Matrix::CreateTranslation(Vector3(4, 2, 10));
    m_objects[8].minBounds = Vector3(3, 1, 9);
    m_objects[8].maxBounds = Vector3(5, 3, 11);
    m_objects[8].baseSize = Vector3(2, 2, 2);    // Dynamic objects - circular moving cubes for testing
    m_objects[9].world = Matrix::CreateTranslation(Vector3(-8, 0, 15));
    m_objects[9].baseSize = Vector3(2, 2, 2);
    m_objects[9].isDynamic = true;
    m_objects[9].animationCenter = Vector3(-8, 0, 15);  // Center of circular motion
    m_objects[9].animationRadius = 3.0f;  // Radius of 3 units
    m_objects[9].animationTime = 0.0f;
    m_objects[9].UpdateBounds();

    m_objects[10].world = Matrix::CreateTranslation(Vector3(8, 0, 15));
    m_objects[10].baseSize = Vector3(2, 2, 2);
    m_objects[10].isDynamic = true;
    m_objects[10].animationCenter = Vector3(8, 0, 15);  // Center of circular motion
    m_objects[10].animationRadius = 4.0f;  // Radius of 4 units
    m_objects[10].animationTime = 1.57f;  // Start at 90 degrees offset
    m_objects[10].UpdateBounds();

    m_objects[11].world = Matrix::CreateTranslation(Vector3(0, 4, 12));
    m_objects[11].baseSize = Vector3(2, 2, 2);
    m_objects[11].isDynamic = true;
    m_objects[11].animationCenter = Vector3(0, 4, 12);  // Center of circular motion
    m_objects[11].animationRadius = 2.5f;  // Radius of 2.5 units
    m_objects[11].animationTime = 3.14f;  // Start at 180 degrees offset
    m_objects[11].UpdateBounds();

    CreateOcclusionQueries();
    return true;
}

bool DXGame::InitializeBVHSystems() {
    // Try to initialize GPU BVH system first
    m_gpuBVH = std::make_unique<GPUBVHSystem>();
    if (m_gpuBVH->Initialize(m_device, m_context, static_cast<int>(m_objects.size()))) {
        m_useGPUBVH = true;
        OutputDebugStringA("Using GPU BVH system\n");
    } else {
        m_gpuBVH.reset();
        m_useGPUBVH = false;
        OutputDebugStringA("GPU BVH not available, using CPU fallback\n");
    }

    // Always create CPU BVH as fallback
    m_cpuBVH = std::make_unique<CPUBVHSystem>();

    return true;
}

void DXGame::CalculateSceneBounds() {
    if (m_objects.empty()) {
        m_sceneMinBounds = Vector3::Zero;
        m_sceneMaxBounds = Vector3::Zero;
        return;
    }

    m_sceneMinBounds = m_objects[0].minBounds;
    m_sceneMaxBounds = m_objects[0].maxBounds;

    for (size_t i = 1; i < m_objects.size(); ++i) {
        m_sceneMinBounds = Vector3::Min(m_sceneMinBounds, m_objects[i].minBounds);
        m_sceneMaxBounds = Vector3::Max(m_sceneMaxBounds, m_objects[i].maxBounds);
    }

    // Add some padding to avoid edge cases
    Vector3 padding = (m_sceneMaxBounds - m_sceneMinBounds) * 0.01f;
    m_sceneMinBounds -= padding;
    m_sceneMaxBounds += padding;
}

void DXGame::UpdateSceneBounds() {
    if (m_objects.empty()) {
        m_sceneMinBounds = Vector3::Zero;
        m_sceneMaxBounds = Vector3::Zero;
        return;
    }

    // Initialize with first object's bounds
    m_sceneMinBounds = m_objects[0].minBounds;
    m_sceneMaxBounds = m_objects[0].maxBounds;

    // Expand bounds to encompass all objects
    for (size_t i = 1; i < m_objects.size(); ++i) {
        const auto& obj = m_objects[i];
        
        // Use current bounds for static objects, predicted bounds for dynamic objects
        Vector3 objMinBounds = obj.minBounds;
        Vector3 objMaxBounds = obj.maxBounds;
        
        if (obj.isDynamic) {
            // Add velocity-based prediction for fast-moving objects
            Vector3 velocityPadding = obj.velocity * 0.1f; // 100ms prediction
            objMinBounds = Vector3::Min(objMinBounds, objMinBounds + velocityPadding);
            objMaxBounds = Vector3::Max(objMaxBounds, objMaxBounds + velocityPadding);
        }
        
        m_sceneMinBounds = Vector3::Min(m_sceneMinBounds, objMinBounds);
        m_sceneMaxBounds = Vector3::Max(m_sceneMaxBounds, objMaxBounds);
    }

    // Add intelligent padding based on scene characteristics
    Vector3 sceneSize = m_sceneMaxBounds - m_sceneMinBounds;
    Vector3 basePadding = sceneSize * Config::SCENE_BOUNDS_PADDING;
    
    // Additional padding for scenes with many dynamic objects
    int dynamicObjectCount = 0;
    float maxVelocity = 0.0f;
    for (const auto& obj : m_objects) {
        if (obj.isDynamic) {
            dynamicObjectCount++;
            maxVelocity = std::max(maxVelocity, obj.velocity.Length());
        }
    }
    
    // Velocity-based padding for dynamic scenes
    Vector3 velocityPadding = Vector3::One * (maxVelocity * 0.2f);
    Vector3 totalPadding = basePadding + velocityPadding;
    
    m_sceneMinBounds -= totalPadding;
    m_sceneMaxBounds += totalPadding;
    
    // Ensure minimum scene size to prevent degenerate cases
    Vector3 finalSize = m_sceneMaxBounds - m_sceneMinBounds;
    const float minSceneSize = 1.0f;
    
    if (finalSize.x < minSceneSize || finalSize.y < minSceneSize || finalSize.z < minSceneSize) {
        Vector3 center = (m_sceneMinBounds + m_sceneMaxBounds) * 0.5f;
        Vector3 halfMinSize = Vector3::One * (minSceneSize * 0.5f);
        m_sceneMinBounds = center - halfMinSize;
        m_sceneMaxBounds = center + halfMinSize;
    }
}

void DXGame::CreateOcclusionQueries() {
    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_OCCLUSION;

    for (auto& obj : m_objects) {
        HRESULT hr = m_device->CreateQuery(&queryDesc, &obj.occlusionQuery);
        if (FAILED(hr)) {
            OutputDebugStringA("Failed to create occlusion query\n");
            return;
        }

        // Initialize all objects as visible
        obj.visible = true;
        obj.occludedFrameCount = 0;
    }
}

void DXGame::UpdateObjectBounds(RenderObject& obj) {
    // Extract position from world matrix
    Vector3 position = GetObjectPosition(obj.world);
    
    // Update bounding box based on object's base size and current position
    Vector3 halfSize = obj.baseSize * 0.5f;
    obj.minBounds = position - halfSize;
    obj.maxBounds = position + halfSize;
}

Vector3 DXGame::GetObjectPosition(const Matrix& worldMatrix) {
    return Vector3(worldMatrix._41, worldMatrix._42, worldMatrix._43);
}

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <chrono>
#include <utility>

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

// Common constants
namespace Config {
    constexpr int MAX_BVH_DEPTH = 16;
    constexpr int COMPUTE_THREADS_PER_GROUP = 64;
    constexpr int MAX_STACK_SIZE = 64;
    constexpr int MORTON_CODE_BITS = 10;
    constexpr int MORTON_CODE_RANGE = 1023;
    constexpr int MSAA_SAMPLES = 4;
    constexpr int OCCLUSION_FRAME_THRESHOLD = 1;
}

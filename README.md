# dx11-gpu-culling

A **high-performance DirectX 11 demonstration** showcasing GPU-accelerated occlusion queries, frustum culling with BVH (Bounding Volume Hierarchy), and advanced 3D rendering optimization techniques.

![DirectX 11](https://img.shields.io/badge/DirectX-11-blue.svg)
![Language](https://img.shields.io/badge/Language-C++-orange.svg)
![Platform](https://img.shields.io/badge/Platform-Windows-lightgrey.svg)

## 🚀 Features

- **GPU Occlusion Queries** - Hardware-accelerated visibility testing
- **Frustum Culling with BVH** - Hierarchical bounding volume optimization
- **FPS Camera Controls** - Smooth first-person navigation (WASD + mouse)
- **Performance Metrics** - Real-time rendering statistics
- **Multiple Geometric Primitives** - Colorful 3D objects for testing
- **Optimized Rendering Pipeline** - Efficient DirectX 11 implementation

## ⚠️ Important Configuration Notice

**This project is specifically configured for:**
- **Static Library Linking** (`/MT` runtime)
- **Release x64 configuration only**
- **DirectXTK static linking**

Other configurations (Debug, x86, Dynamic linking) may require additional setup and are not currently supported out-of-the-box.

## 🎮 Controls

- **F1** - First Person Mode (Walk Mode), to disable Walk Mode press F1 again. (Toggle)
- **WASD** - Move camera
- **Mouse** - Look around
- **ESC** - Exit application

## 🛠️ Technical Details

### Rendering Features
- **DirectX 11.1** with fallback support (11.0, 10.1, 10.0)
- **DirectXTK** integration for enhanced graphics capabilities
- **BVH (Bounding Volume Hierarchy)** for spatial partitioning
- **Hardware occlusion queries** for visibility optimization
- **Frustum culling** to eliminate off-screen objects

### Performance Optimizations
- Static linking configuration (`/MT`) for reduced dependencies
- WARP fallback for software rendering compatibility
- Debug layer support in debug builds only
- Optimized object culling algorithms

## 📋 System Requirements

### Minimum Requirements
- **OS**: Windows 10/11 (64-bit)
- **DirectX**: DirectX 11 compatible graphics card
- **RAM**: 4GB minimum
- **Visual C++**: 2022 Redistributable (x64)

### Recommended
- **Graphics**: Dedicated GPU with DirectX 11.1 support
- **RAM**: 8GB or more
- **Drivers**: Latest graphics drivers (NVIDIA/AMD/Intel)

## 🔧 Building from Source

### Prerequisites
1. **Visual Studio 2022** (Community/Professional/Enterprise)
2. **Windows SDK** (latest version)
3. **DirectX SDK** (included with Windows SDK)

### Build Steps
1. Clone the repository
2. Open `DXGame.sln` in Visual Studio
3. Ensure **Release** configuration is selected for distribution
4. Build the solution (`Ctrl+Shift+B`)
5. Output will be in `x64/Release/DXGame.exe`

## 📦 Distribution

### Required Dependencies
When distributing to other PCs, ensure these are installed:

1. **Microsoft Visual C++ Redistributable for Visual Studio 2022 (x64)**
   - Download: https://aka.ms/vs/17/release/vc_redist.x64.exe

2. **DirectX End-User Runtime**
   - Download from Microsoft's official DirectX page

### Testing Deployment
- Test on machines without Visual Studio installed
- Verify on different hardware configurations
- Check compatibility with older DirectX 11 hardware

## 🐛 Troubleshooting

### Common Issues

**Application crashes on other PCs:**
- Install Visual C++ 2022 Redistributable (x64)
- Update graphics drivers to latest version
- Ensure DirectX 11 support is available

**Performance issues:**
- Check graphics driver updates
- Verify DirectX 11 hardware acceleration is enabled
- Monitor system resources during execution

**Debug Information:**
- Check Windows Event Viewer for crash details
- Use DirectX Control Panel for debugging (if DirectX SDK installed)

**Graphics Debugging:**
- Tested and compatible with **NVIDIA Nsight Graphics 2025** for GPU debugging and profiling
- DirectX 11 API calls can be captured and analyzed for performance optimization

## 🧪 Architecture

### Core Components
- **FPSCamera** - First-person camera implementation
- **BVHNode** - Bounding volume hierarchy structure
- **RenderObject** - Renderable entity with culling data
- **Frustum** - View frustum mathematics for culling
- **GPU Culling Shaders** - Compute-based culling operations

### Culling Pipeline
1. **Frustum Culling** - Eliminate objects outside view
2. **BVH Traversal** - Hierarchical spatial queries
3. **Occlusion Queries** - GPU-based visibility testing
4. **Render Queue** - Optimized object submission

## 📊 Performance Features

- Delta time calculation for smooth frame-independent movement
- GPU compute shader fallback to CPU culling when unsupported
- Hardware occlusion query optimization
- BVH spatial partitioning for efficient culling

> **Note**: Advanced performance monitoring (FPS display, culling metrics) is planned for future implementation.

## 🤝 Contributing

This is a demonstration project showcasing DirectX 11 culling techniques. Feel free to:
- Study the implementation
- Suggest optimizations
- Report compatibility issues
- Extend with additional features

## 📄 License

MIT License

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party library licenses and attributions.

## 🔗 Links

- [DirectX Documentation](https://docs.microsoft.com/en-us/windows/win32/directx)
- [DirectXTK](https://github.com/Microsoft/DirectXTK)
- [Visual Studio](https://visualstudio.microsoft.com/)

---

**Built with DirectX 11 and DirectXTK for Windows**

#include "DXGame.h"

// Global variables
extern std::unique_ptr<DXGame> g_game;

// ============================================================================
// WINDOW PROCEDURES AND MAIN ENTRY POINT
// ============================================================================

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
        L"DirectX 11 - GPU-Built BVH with Frustum Culling (Cleaned)",
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

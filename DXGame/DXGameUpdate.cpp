#include "DXGame.h"

// ============================================================================
// UPDATE METHODS
// ============================================================================

void DXGame::Update() {
    // Calculate delta time
    auto currentTime = std::chrono::high_resolution_clock::now();
    m_deltaTime = std::chrono::duration<float>(currentTime - m_lastTime).count();
    m_lastTime = currentTime;

    UpdateInput();
    UpdateCamera();
    UpdateDynamicObjects();  // Update object animations
    UpdateSceneBounds();     // Update scene bounds for dynamic objects
    UpdateFrustum();
    UpdateBVH();
    UpdateCulling();
}

void DXGame::UpdateInput() {
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
}

void DXGame::UpdateCamera() {
    if (!m_fpsMode) return;

    auto kb = m_keyboard->GetState();
    auto mouse = m_mouse->GetState();

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
    float yOffset = static_cast<float>(lastY - mouse.y);

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

void DXGame::UpdateFrustum() {
    Matrix view = m_camera.GetViewMatrix();
    Matrix projection = m_camera.GetProjectionMatrix(static_cast<float>(m_width) / m_height);
    m_frustum.ExtractFromMatrix(view * projection);
}

void DXGame::UpdateBVH() {
    // First, check if we need a full rebuild
    bool needsRebuild = m_bvhNeedsRebuild;
    
    if (m_useGPUBVH && m_gpuBVH && !needsRebuild) {
        needsRebuild = m_gpuBVH->ShouldRebuildBVH(m_objects);
    }
    
    if (needsRebuild) {
        // Perform full BVH rebuild
        if (m_useGPUBVH && m_gpuBVH) {
            if (m_gpuBVH->BuildBVH(m_objects, m_sceneMinBounds, m_sceneMaxBounds)) {
                OutputDebugStringA("GPU BVH rebuilt successfully\n");
            } else {
                OutputDebugStringA("GPU BVH rebuild failed, falling back to CPU\n");
                if (m_cpuBVH) {
                    m_cpuBVH->BuildBVH(m_objects);
                }
            }
        } else if (m_cpuBVH) {
            m_cpuBVH->BuildBVH(m_objects);
        }
        
        m_bvhNeedsRebuild = false;
    } else {
        // Check if any dynamic objects have moved enough to warrant a refit
        bool hasSignificantMovement = false;
        for (const auto& obj : m_objects) {
            if (obj.isDynamic && obj.movementDistance > Config::MOVEMENT_THRESHOLD) {
                hasSignificantMovement = true;
                break;
            }
        }
        
        if (hasSignificantMovement) {
            // Perform efficient BVH refit
            if (m_useGPUBVH && m_gpuBVH) {
                if (!m_gpuBVH->RefitBVH(m_objects)) {
                    OutputDebugStringA("GPU BVH refit failed\n");
                    // Don't fallback to CPU refit as it's expensive
                    // Mark for rebuild next frame instead
                    m_bvhNeedsRebuild = true;
                }
            } else if (m_cpuBVH) {
                // CPU BVH doesn't have refit, so rebuild
                m_cpuBVH->BuildBVH(m_objects);
            }
        }
    }
}

void DXGame::UpdateCulling() {
    PerformCulling();
    ProcessOcclusionQueries();
}

void DXGame::UpdateDynamicObjects() {
    // Use the GPU BVH system to update dynamic objects if available
    if (m_useGPUBVH && m_gpuBVH) {
        m_gpuBVH->UpdateDynamicObjects(m_objects, m_deltaTime);
    } else {
        // Fallback: update dynamic objects manually
        for (auto& obj : m_objects) {
            if (obj.isDynamic) {
                // Update animation time
                obj.animationTime += m_deltaTime;
                
                // Calculate new position based on circular motion
                Vector3 newPosition = obj.animationCenter + Vector3(
                    cos(obj.animationTime) * obj.animationRadius,
                    0.0f,
                    sin(obj.animationTime) * obj.animationRadius
                );
                
                // Calculate movement distance
                Vector3 currentPos = obj.GetPosition();
                obj.movementDistance = (newPosition - currentPos).Length();
                obj.previousPosition = currentPos;
                
                // Update world matrix
                obj.world = Matrix::CreateTranslation(newPosition);
                
                // Update bounding box
                obj.UpdateBounds();
            }
        }
    }
}

// ============================================================================
// CULLING METHODS
// ============================================================================

void DXGame::PerformCulling() {
    bool gpuCullingSuccess = false;

    // Try GPU culling first
    if (m_useGPUBVH && m_gpuBVH) {
        gpuCullingSuccess = m_gpuBVH->PerformFrustumCulling(m_frustum, m_objects);
    }

    // Fallback to CPU culling if GPU failed
    if (!gpuCullingSuccess && m_cpuBVH) {
        m_cpuBVH->PerformFrustumCulling(m_frustum, m_objects);
    }
}

void DXGame::ProcessOcclusionQueries() {
    for (auto& obj : m_objects) {
        if (obj.occlusionQuery && obj.queryInProgress) {
            UINT64 result = 0;
            HRESULT hr = m_context->GetData(obj.occlusionQuery.Get(), &result, sizeof(result), D3D11_ASYNC_GETDATA_DONOTFLUSH);

            if (hr == S_OK) {
                obj.lastQueryResult = result;
                obj.queryInProgress = false;

                if (result == 0) {
                    // Object is occluded
                    obj.occludedFrameCount++;
                    // Hide object after being occluded for threshold frames
                    if (obj.occludedFrameCount >= Config::OCCLUSION_FRAME_THRESHOLD && obj.visible) {
                        obj.visible = false;
                    }
                } else {
                    // Object is visible - reset counter
                    obj.occludedFrameCount = 0;
                }
            }
            // If hr == S_FALSE, query data not ready yet, keep waiting
        }
    }
}

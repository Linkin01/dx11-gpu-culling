#include "DXGame.h"

// ============================================================================
// RENDERING METHODS
// ============================================================================

void DXGame::Render() {
    // Clear render target and depth buffer
    const float clearColor[4] = { 0.2f, 0.3f, 0.4f, 1.0f };
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    m_context->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // Set render target and depth buffer
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());

    // Enable backface culling for proper 3D rendering
    m_context->RSSetState(m_states->CullCounterClockwise());

    // Set up proper depth stencil state for occlusion culling
    m_context->OMSetDepthStencilState(m_states->DepthDefault(), 0);

    // Set up matrices
    Matrix view = m_camera.GetViewMatrix();
    Matrix projection = m_camera.GetProjectionMatrix(static_cast<float>(m_width) / m_height);

    m_effect->SetView(view);
    m_effect->SetProjection(projection);

    // Sort objects front-to-back for better occlusion culling
    std::vector<std::pair<float, int>> depthSortedObjects;
    for (int i = 0; i < m_objects.size(); ++i) {
        if (m_objects[i].visible) {
            // Calculate distance from camera to object center
            Vector3 objectCenter = (m_objects[i].minBounds + m_objects[i].maxBounds) * 0.5f;
            Vector3 toObject = objectCenter - m_camera.position;
            float distance = toObject.LengthSquared(); // Use squared distance to avoid sqrt
            depthSortedObjects.push_back({ distance, i });
        }
    }

    // Sort front-to-back (closest first)
    std::sort(depthSortedObjects.begin(), depthSortedObjects.end());

    // Render all frustum-culled objects in front-to-back order for occlusion culling
    for (const auto& sortedObj : depthSortedObjects) {
        auto& obj = m_objects[sortedObj.second];

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

    // Reset occlusion query state for objects not rendered (outside frustum)
    for (auto& obj : m_objects) {
        if (!obj.visible && obj.queryInProgress) {
            obj.queryInProgress = false;
        }
    }

    // Present
    m_swapChain->Present(1, 0);
}

void DXGame::OnResize(int width, int height) {
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

#include "vulkan_renderer.hpp"
#include <SDL2/SDL_vulkan.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

VulkanRenderer::VulkanRenderer(SDL_Window* win) : window(win) {
    if (createInstance() < 0) {
        std::cerr << "vkCreateInstance failed" << std::endl;
        return;
    }
    createSurface();
    pickPhysicalDevice();
    createDevice();
    createSwapchain();
    createRenderPass();
    createPipeline();
    createVertexBuffer();
    createFramebuffers();
    updateVertexData();
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
    int w, h;
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    windowWidth = w;
    windowHeight = h;

    try {
        textRenderer.init(
            { device, physicalDevice, graphicsQueue, graphicsFamily },
            renderPass,
            // Pass in your actual swapchain/window width/height
            (float)w, (float)h, 
            "fonts/atlas.png", // Path to your pre-generated MSDF texture atlas
            4.0f                         // The range value used when generating the atlas
        );
    } catch (const std::exception& e) {
        std::cerr << "Text Renderer initialization failed: " << e.what() << std::endl;
        // Handle error
    }
}

VulkanRenderer::~VulkanRenderer() {
    vkDeviceWaitIdle(device);
    vkDestroySemaphore(device, renderFinished, nullptr);
    vkDestroySemaphore(device, imageAvailable, nullptr);
    for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    for (auto iv : imageViews) vkDestroyImageView(device, iv, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
}

void VulkanRenderer::addText(const char* text, float x, float y, float scale, const glm::vec4& colour) {
    texts.emplace_back(text, x, y, scale, colour);
}

void VulkanRenderer::drawFrame() {
    uint32_t imageIndex;
    vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);

    // --- Rebuild the command buffer every frame ---
    VkCommandBuffer cmd = commandBuffers[imageIndex];

    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkClearValue clearColor = {0.08f, 0.09f, 0.11f, 1.0f};
    VkRenderPassBeginInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpInfo.renderPass = renderPass;
    rpInfo.framebuffer = framebuffers[imageIndex];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = swapchainExtent;
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport viewport{};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width  = (float)swapchainExtent.width;
    viewport.height = (float)swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent;

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (!vertices.empty()) {
        VkBuffer buffers[] = {vertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        vkCmdDraw(cmd, static_cast<uint32_t>(vertices.size()), 1, 0, 0);
    }

    // Begin staging all text strings
    textRenderer.beginTextSubmission(); 

    for (const auto& t : texts) {
        // printf("text: %s, x: %f, y: %f, size: %f", t.text, t.x, t.y, t.size);
        // Add multiple text strings (all text will be batched into a single draw call)
        textRenderer.addText(
            t.text,
            t.x, t.y,
            t.size,
            t.colour,
            cmd
        );
    }
    texts.clear();

    // End submission, which updates the vertex buffer and issues the draw command(s)
    textRenderer.endTextSubmission(cmd);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // --- Submit ---
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished;
    vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    vkQueuePresentKHR(graphicsQueue, &present);

    vkQueueWaitIdle(graphicsQueue);
}


VkShaderModule VulkanRenderer::loadShaderModule(const std::string& path) {
    auto code = readFile(path);
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
        throw std::runtime_error("failed to create shader module");
    return shaderModule;
}

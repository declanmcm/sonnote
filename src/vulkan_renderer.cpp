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
    createTransientQuadBuffer();
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
    destroyTransientQuadBuffer();
}

void VulkanRenderer::createSpectrogramPipeline()
{
    // Descriptor set layout: combined image sampler at binding 0
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;

    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &spectroSetLayout) != VK_SUCCESS)
        throw std::runtime_error("spectroSetLayout failed");

    // Pipeline layout includes descriptor set + push constants for uvRect
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset = 0;
    pc.size = sizeof(float) * 4; // vec4 uvRect

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &spectroSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;

    if (vkCreatePipelineLayout(device, &pli, nullptr, &spectroPipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("spectroPipelineLayout failed");

    // Load shaders
    VkShaderModule vert = loadShaderModule("shaders/spectro.vert.spv");
    VkShaderModule frag = loadShaderModule("shaders/spectro.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // Vertex input matches your Vertex {pos,color}
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(Vertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attribs{};
    attribs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)};
    attribs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bindingDesc;
    vi.vertexAttributeDescriptionCount = (uint32_t)attribs.size();
    vi.pVertexAttributeDescriptions = attribs.data();

    // Input assembly (triangles)
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor dynamic (or fixed like your existing pipeline)
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Blend: you likely want it somewhat subtle; simplest: no blend
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    // Dynamic states
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynStates;

    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &dss;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = spectroPipelineLayout;
    pi.renderPass = renderPass;
    pi.subpass = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &spectroPipeline) != VK_SUCCESS)
        throw std::runtime_error("spectroPipeline failed");

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);

    // Descriptor pool for 1 combined image sampler
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 1;

    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;

    if (vkCreateDescriptorPool(device, &dp, nullptr, &spectroDescPool) != VK_SUCCESS)
        throw std::runtime_error("spectroDescPool failed");

    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = spectroDescPool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &spectroSetLayout;

    if (vkAllocateDescriptorSets(device, &da, &spectroDescSet) != VK_SUCCESS)
        throw std::runtime_error("spectroDescSet alloc failed");

    // Sampler
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(device, &si, nullptr, &spectroSampler) != VK_SUCCESS)
        throw std::runtime_error("spectroSampler failed");
}

void VulkanRenderer::uploadSpectrogramR8(const uint8_t* data, int w, int h)
{
    // Destroy previous if any
    if (hasSpectrogram) {
        vkDeviceWaitIdle(device);
        vkDestroyImageView(device, spectroView, nullptr);
        vkDestroyImage(device, spectroImage, nullptr);
        vkFreeMemory(device, spectroMemory, nullptr);
        hasSpectrogram = false;
    }

    spectroW = w;
    spectroH = h;

    // Create GPU image
    createImage2D(
        w, h,
        VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        spectroImage,
        spectroMemory
    );
    spectroView = createImageView2D(spectroImage, VK_FORMAT_R8_UNORM);

    // Create staging buffer
    VkDeviceSize size = (VkDeviceSize)w * (VkDeviceSize)h;
    VkBuffer staging{};
    VkDeviceMemory stagingMem{};
    void* mapped = nullptr;
    createVulkanBuffer(device, physicalDevice, size,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem, &mapped);

    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(device, stagingMem);

    // Copy
    transitionImageLayout(spectroImage, VK_FORMAT_R8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    copyBufferToImage(staging, spectroImage, w, h);

    transitionImageLayout(spectroImage, VK_FORMAT_R8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, stagingMem, nullptr);

    // Update descriptor set
    VkDescriptorImageInfo ii{};
    ii.sampler = spectroSampler;
    ii.imageView = spectroView;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet = spectroDescSet;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &ii;

    auto require = [](bool ok, const char* msg){
        if (!ok) { fprintf(stderr, "SPECTRO ASSERT: %s\n", msg); abort(); }
    };

    require(device != VK_NULL_HANDLE, "device null");
    require(spectroDescSet != VK_NULL_HANDLE, "spectroDescSet null (not allocated?)");
    require(spectroSampler != VK_NULL_HANDLE, "spectroSampler null (not created?)");
    require(spectroView != VK_NULL_HANDLE, "spectroView null (view creation failed?)");
    require(spectroPipelineLayout != VK_NULL_HANDLE, "spectroPipelineLayout null");

    vkUpdateDescriptorSets(device, 1, &wds, 0, nullptr);

    hasSpectrogram = true;
}


void VulkanRenderer::addText(const char* text, float x, float y, float scale, const glm::vec4& colour) {
    texts.emplace_back(text, x, y, scale, colour);
}

void VulkanRenderer::drawFrame(uint32_t canvasX, uint32_t canvasY, uint32_t canvasW, uint32_t canvasH) {
    uint32_t imageIndex;
    vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);

    // --- Rebuild the command buffer every frame ---
    VkCommandBuffer cmd = commandBuffers[imageIndex];

    vkResetCommandPool(device, commandPool, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &beginInfo);

    // // ============================================================
    // // 1) LIMIT DRAWING TO NOTE CANVAS
    // // ============================================================
    // VkRect2D canvasScissor{};
    // canvasScissor.offset = { canvasX, canvasY };
    // canvasScissor.extent = {
    //     (uint32_t)canvasW,
    //     (uint32_t)canvasH
    // };
    // vkCmdSetScissor(cmd, 0, 1, &canvasScissor);

    VkClearValue clearColor = {0.1f, 0.1f, 0.1f, 1.0f};
    VkRenderPassBeginInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpInfo.renderPass = renderPass;
    rpInfo.framebuffer = framebuffers[imageIndex];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = swapchainExtent;
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // ============================================================
    // 2) BLACK BACKGROUND (normal pipeline)
    // ============================================================
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

    float x0 = ((float)canvasX / swapchainExtent.width) * 2.0f - 1.0f;
    float x1 = ((float)(canvasX + canvasW) / swapchainExtent.width) * 2.0f - 1.0f;
    float y0 = ((float)canvasY / swapchainExtent.height) * (-2.0f) + 1.0f;
    float y1 = ((float)(canvasY + canvasH) / swapchainExtent.height) * (-2.0f) + 1.0f;

    printf("x, y: (%f, %f, %f, %f)\n", x0, x1, y0, y1);
    printf("canvas: (%d, %d, %d,%d)\n", canvasX, canvasY, canvasW, canvasH);

    // CPU geometry
    Vertex blackQuad[6] = {
        {{x0, y0}, {0,0,0}},
        {{x1, y0}, {0,0,0}},
        {{x1, y1}, {0,0,0}},
        {{x1, y1}, {0,0,0}},
        {{x0, y1}, {0,0,0}},
        {{x0, y0}, {0,0,0}},
    };

    updateTransientVertexBuffer(blackQuad, sizeof(blackQuad));
    VkBuffer buffers[] = { transientQuadBuffer };
    VkDeviceSize offsets[] = {0};
    // vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    // vkCmdDraw(cmd, 6, 1, 0, 0);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    if (!vertices.empty() && (int)vertices.size() >= 12) {
        VkBuffer tempBuffers[] = {vertexBuffer};
        VkDeviceSize tempOffsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, tempBuffers, offsets);
        vkCmdDraw(cmd, 12, 1, 0, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, spectroPipeline);

    if (hasSpectrogram) {

        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            spectroPipelineLayout,
            0, 1, &spectroDescSet,
            0, nullptr
        );

        // float u0 = viewStartSeconds / totalDurationSeconds;
        // float u1 = viewEndSeconds   / totalDurationSeconds;
        // u0 = std::clamp(u0, 0.0f, 1.0f);
        // u1 = std::clamp(u1, 0.0f, 1.0f);
        float u0 = 0.0;
        float u1 = 1.0;

        float uvRect[4] = { u0, 0.0f, u1, 1.0f };

        vkCmdPushConstants(
            cmd,
            spectroPipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(uvRect),
            uvRect
        );

        // reuse same quad geometry
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    // // ============================================================
    // // 4) RESTORE FULL SCISSOR
    // // ============================================================
    // VkRect2D fullScissor{};
    // fullScissor.offset = {0,0};
    // fullScissor.extent = swapchainExtent;
    // vkCmdSetScissor(cmd, 0, 1, &fullScissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    if (!vertices.empty() && (int)vertices.size() > 12) {
        VkBuffer tempBuffers[] = {vertexBuffer};
        VkDeviceSize tempOffsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, tempBuffers, tempOffsets);
        vkCmdDraw(cmd, static_cast<uint32_t>(vertices.size() - 12), 1, 12, 0);
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

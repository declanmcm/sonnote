#pragma once
#include <vulkan/vulkan.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <array>

#define WIDTH 800
#define HEIGHT 600

struct Vertex {
    float pos[2];
    float color[3];
};

struct Rect {
    float x;
    float y;
    float w;
    float h;
};

static std::vector<char> readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("failed to open file!");
    size_t fileSize = (size_t) file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

class VulkanRenderer {
public:
    VulkanRenderer(SDL_Window* window);
    ~VulkanRenderer();

    void drawFrame();

    std::vector<Vertex> vertices; 
    std::vector<uint16_t> indices;
    std::vector<Rect> rects;

    void addRect(float x, float y, float w, float h, float r, float g, float b) {
        // Convert to NDC
        float x1 = (x / windowWidth)  * 2.0f - 1.0f;
        float y1 = 1.0f - (y / windowHeight) * 2.0f;
        float x2 = ((x + w) / windowWidth)  * 2.0f - 1.0f;
        float y2 = 1.0f - ((y + h) / windowHeight) * 2.0f;

        Vertex v0 = {{x1, y1}, {r, g, b}};
        Vertex v1 = {{x2, y1}, {r, g, b}};
        Vertex v2 = {{x2, y2}, {r, g, b}};
        Vertex v3 = {{x1, y2}, {r, g, b}};
        vertices.insert(vertices.end(), {v0, v1, v2, v2, v3, v0});
    }

    void updateVertexData() {
        if (vertices.empty()) return;
        void* data;
        VkDeviceSize size = sizeof(Vertex) * vertices.size();
        vkMapMemory(device, vertexMemory, 0, size, 0, &data);
        memcpy(data, vertices.data(), (size_t)size);
        vkUnmapMemory(device, vertexMemory);
    }

    void onResize(int newW, int newH)
    {
        vkDeviceWaitIdle(device);

        windowWidth = newW;
        windowHeight = newH;

        // Destroy old
        for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (auto iv : imageViews) vkDestroyImageView(device, iv, nullptr);
        vkDestroySwapchainKHR(device, swapchain, nullptr);

        // Recreate swapchain with new dimensions
        swapchainExtent = { (uint32_t)newW, (uint32_t)newH };
        createSwapchain();       // must update swapchainExtent
        createFramebuffers();    // must rebuild for new views
        createCommandBuffers();
    }
    
    uint32_t getWindowWidth() const { return windowWidth; }
    uint32_t getWindowHeight() const { return windowHeight; }

private:
    SDL_Window* window;
    uint32_t windowWidth;
    uint32_t windowHeight;
    VkInstance instance{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physicalDevice{};
    VkDevice device{};
    VkQueue graphicsQueue{};
    uint32_t graphicsFamily{};

    VkSwapchainKHR swapchain{};
    VkFormat swapchainFormat{};
    VkExtent2D swapchainExtent{};
    VkSurfaceFormatKHR surfaceFormat{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> imageViews;
    std::vector<VkFramebuffer> framebuffers;
    uint32_t imageCount;

    VkRenderPass renderPass{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexMemory;
    VkBuffer indexBuffer;
    VkDeviceMemory indexMemory;

    VkCommandPool commandPool{};
    std::vector<VkCommandBuffer> commandBuffers;
    VkSemaphore imageAvailable{}, renderFinished{};

    // Private helper functions
    int createInstance() {
        // ---- Vulkan instance ----
        unsigned int extCount = 0;
        SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr);
        std::vector<const char*> extensions(extCount);
        SDL_Vulkan_GetInstanceExtensions(window, &extCount, extensions.data());

        VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.pApplicationName = "Sonnote";
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = extCount;
        createInfo.ppEnabledExtensionNames = extensions.data();

        // const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
        // createInfo.enabledLayerCount = 1;
        // createInfo.ppEnabledLayerNames = validationLayers;

        for (auto e : extensions)
        std::cout << "Using extension: " << e << std::endl;

        VkResult res = vkCreateInstance(&createInfo, nullptr, &instance);
        if (res != VK_SUCCESS) {
            std::cerr << "vkCreateInstance failed with code " << res << std::endl;
            return -1;
        }
        return 0;
    }

    void createSurface() {
        // ---- Surface ----
        SDL_Vulkan_CreateSurface(window, instance, &surface);
    }

    void pickPhysicalDevice() {
        // ---- Pick physical device ----
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        physicalDevice = devices[0];

         // ---- Queue family ----
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
        graphicsFamily = 0;
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupport);
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && presentSupport) {
                graphicsFamily = i;
                break;
            }
        }
    }

    void createDevice() {
        // ---- Logical device ----
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qinfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qinfo.queueFamilyIndex = graphicsFamily;
        qinfo.queueCount = 1;
        qinfo.pQueuePriorities = &priority;

        const char* deviceExtensions[] = { "VK_KHR_swapchain" };

        VkDeviceCreateInfo devInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        devInfo.queueCreateInfoCount = 1;
        devInfo.pQueueCreateInfos = &qinfo;
        devInfo.enabledExtensionCount = 1;
        devInfo.ppEnabledExtensionNames = deviceExtensions;

        VkResult res = vkCreateDevice(physicalDevice, &devInfo, nullptr, &device);
        if (res != VK_SUCCESS) {
            std::cerr << "Failed to create logical device: " << res << std::endl;
        }

        vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    }

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }
        throw std::runtime_error("failed to find suitable memory type!");
    }

    void createVertexBuffer() {
        VkDeviceSize bufferSize = sizeof(Vertex) * 1000; // can hold many rectangles

        VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = bufferSize;
        bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufInfo, nullptr, &vertexBuffer);

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, vertexBuffer, &memReq);

        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(device, &allocInfo, nullptr, &vertexMemory);
        vkBindBufferMemory(device, vertexBuffer, vertexMemory, 0);

        void* data;
        vkMapMemory(device, vertexMemory, 0, bufferSize, 0, &data);
        memset(data, 0, bufferSize);
        vkUnmapMemory(device, vertexMemory);
    }

    void createSwapchain() {
        // ---- Swapchain ----
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
        surfaceFormat = formats[0];

        swapchainExtent = caps.currentExtent;

        imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;

        VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapInfo.surface = surface;
        swapInfo.minImageCount = imageCount;
        swapInfo.imageFormat = surfaceFormat.format;
        swapInfo.imageColorSpace = surfaceFormat.colorSpace;
        swapInfo.imageExtent = swapchainExtent;
        swapInfo.imageArrayLayers = 1;
        swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapInfo.preTransform = caps.currentTransform;
        swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapInfo.clipped = VK_TRUE;

        vkCreateSwapchainKHR(device, &swapInfo, nullptr, &swapchain);

        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        swapchainImages = std::vector<VkImage>(imageCount);
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());

        imageViews = std::vector<VkImageView>(imageCount);
        for (size_t i = 0; i < imageCount; i++) {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = swapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = surfaceFormat.format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]);
        }
    }

    void createRenderPass() {
        // ---- Render pass ----
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = surfaceFormat.format;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkRenderPassCreateInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAttachment;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;

        vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass);
    }

    void createPipeline() {
        // ---- Pipeline ----
        auto vertCode = readFile("shaders/rect.vert.spv");
        auto fragCode = readFile("shaders/rect.frag.spv");

        VkShaderModuleCreateInfo vinfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        vinfo.codeSize = vertCode.size();
        vinfo.pCode = reinterpret_cast<const uint32_t*>(vertCode.data());
        VkShaderModule vertShader;
        vkCreateShaderModule(device, &vinfo, nullptr, &vertShader);

        VkShaderModuleCreateInfo finfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        finfo.codeSize = fragCode.size();
        finfo.pCode = reinterpret_cast<const uint32_t*>(fragCode.data());
        VkShaderModule fragShader;
        vkCreateShaderModule(device, &finfo, nullptr, &fragShader);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertShader;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragShader;
        stages[1].pName = "main";

        // --- Vertex input ---
        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding = 0;
        bindingDesc.stride = sizeof(Vertex);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attribs{};
        attribs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)};
        attribs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)};

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
        vertexInput.pVertexAttributeDescriptions = attribs.data();
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{0.0f, 0.0f, (float)swapchainExtent.width, (float)swapchainExtent.height, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, swapchainExtent};
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &colorBlendAttachment;

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

        VkGraphicsPipelineCreateInfo pipeInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeInfo.stageCount = 2;
        pipeInfo.pStages = stages;
        pipeInfo.pVertexInputState = &vertexInput;
        pipeInfo.pInputAssemblyState = &inputAssembly;
        pipeInfo.pViewportState = &viewportState;
        pipeInfo.pRasterizationState = &raster;
        pipeInfo.pMultisampleState = &multisample;
        pipeInfo.pColorBlendState = &colorBlend;
        pipeInfo.layout = pipelineLayout;
        pipeInfo.renderPass = renderPass;

        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline);
        vkDestroyShaderModule(device, vertShader, nullptr);
        vkDestroyShaderModule(device, fragShader, nullptr);
    }

    void createFramebuffers() {
        // ---- Framebuffers ----
        framebuffers = std::vector<VkFramebuffer>(imageCount);
        for (size_t i = 0; i < imageCount; i++) {
            VkImageView attachments[] = { imageViews[i] };
            VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = attachments;
            fbInfo.width = swapchainExtent.width;
            fbInfo.height = swapchainExtent.height;
            fbInfo.layers = 1;
            vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[i]);
        }
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = graphicsFamily;
        vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    }

    void createCommandBuffers() {
        commandBuffers = std::vector<VkCommandBuffer>(imageCount);
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)commandBuffers.size();
        vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data());

        for (size_t i = 0; i < commandBuffers.size(); i++) {
            VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vkBeginCommandBuffer(commandBuffers[i], &beginInfo);

            VkClearValue clearColor = {0.1f, 0.1f, 0.1f, 1.0f};
            VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rpBegin.renderPass = renderPass;
            rpBegin.framebuffer = framebuffers[i];
            rpBegin.renderArea.extent = swapchainExtent;
            rpBegin.clearValueCount = 1;
            rpBegin.pClearValues = &clearColor;
            vkCmdBeginRenderPass(commandBuffers[i], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VkBuffer buffers[] = {vertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(commandBuffers[i], 0, 1, buffers, offsets);
            vkCmdDraw(commandBuffers[i], 3, 1, 0, 0);
            vkCmdEndRenderPass(commandBuffers[i]);
            vkEndCommandBuffer(commandBuffers[i]);
        }
    }

    void drawRect(float x, float y, float w, float h, float r, float g, float b) {
    // Convert pixel coordinates to Normalized Device Coordinates (NDC)
    float x1 = (x / swapchainExtent.width)  * 2.0f - 1.0f;
    float y1 = 1.0f - (y / swapchainExtent.height) * 2.0f;
    float x2 = ((x + w) / swapchainExtent.width)  * 2.0f - 1.0f;
    float y2 = 1.0f - ((y + h) / swapchainExtent.height) * 2.0f;

    vertices = {
        {{x1, y1}, {r, g, b}},
        {{x2, y1}, {r, g, b}},
        {{x2, y2}, {r, g, b}},
        {{x1, y2}, {r, g, b}}
    };
    indices = {0, 1, 2, 2, 3, 0};
}

    
    void createSyncObjects() {
        // ---- Sync objects ----
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &semInfo, nullptr, &imageAvailable);
        vkCreateSemaphore(device, &semInfo, nullptr, &renderFinished);
    }

    VkShaderModule loadShaderModule(const std::string& path);


};

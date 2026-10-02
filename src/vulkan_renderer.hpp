#pragma once
#include <vulkan/vulkan.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <array>
#include "text_renderer.hpp"

#define WIDTH 800
#define HEIGHT 600

struct Text {
    const char* text;
    float x;
    float y;
    float size;
    glm::vec4 colour;
};

struct Vertex {
    float pos[2];
    float color[3];
};


// Size (enough for a few quads)
static constexpr VkDeviceSize TRANSIENT_QUAD_BUFFER_SIZE =
    sizeof(Vertex) * 64;

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

    void drawFrame(uint32_t canvasX, uint32_t canvasY, uint32_t canvasW, uint32_t canvasH);

    void addText(const char* text, float x, float y, float scale, const glm::vec4& colour);

    // Start a new draw layer. Everything (rects AND text) added after this call
    // is drawn on top of everything added before it. Layers reset every frame.
    void nextLayer() { layerStarts.push_back({vertices.size(), texts.size()}); }

    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;

    void addRect(float x, float y, float w, float h, float r, float g, float b) {
        // Convert to NDC
        float x1 = normaliseX(x);
        float y1 = normaliseY(y);
        float x2 = normaliseX(x + w);
        float y2 = normaliseY(y + h);

        Vertex v0 = {{x1, y1}, {r, g, b}};
        Vertex v1 = {{x2, y1}, {r, g, b}};
        Vertex v2 = {{x2, y2}, {r, g, b}};
        Vertex v3 = {{x1, y2}, {r, g, b}};
        vertices.insert(vertices.end(), {v0, v1, v2, v2, v3, v0});
    }

    void addLine(float x1, float y1, float x2, float y2, float thickness, float r, float g, float b) {
      float x1n = normaliseX(x1);
      float x2n = normaliseX(x2);
      float y1n = normaliseY(y1);
      float y2n = normaliseY(y2);

      float mag = std::sqrt(std::pow(x2n - x1n, 2) + std::pow(y2n - y1n, 2));
      float ux = (x2n - x1n) / mag;
      float uy = (y2n - y1n) / mag;

      float x1a = x1n - 1 * uy * (thickness / 2);
      float y1a = y1n + ux * (thickness / 2);
      float x1b = x1n + uy * (thickness / 2);
      float y1b = y1n - 1 * ux * (thickness / 2);
      float x2a = x2n - 1 * uy * (thickness / 2);
      float y2a = y2n + ux * (thickness / 2);
      float x2b = x2n + uy * (thickness / 2);
      float y2b = y2n - 1 * ux * (thickness / 2);

      Vertex v0 = {{x1a, y1a}, {r, g, b}};
      Vertex v1 = {{x1b, y1b}, {r, g, b}};
      Vertex v2 = {{x2a, y2a}, {r, g, b}};
      Vertex v3 = {{x2b, y2b}, {r, g, b}};
      vertices.insert(vertices.end(), {v0, v1, v2, v2, v3, v1});
    }

    void addTriangle(float xa, float ya, float xb, float yb, float xc, float yc, float r, float g, float b) {
      Vertex v0 = {{normaliseX(xa), normaliseY(ya)}, {r, g, b}};
      Vertex v1 = {{normaliseX(xb), normaliseY(yb)}, {r, g, b}};
      Vertex v2 = {{normaliseX(xc), normaliseY(yc)}, {r, g, b}};
      vertices.insert(vertices.end(), {v0, v1, v2});
    }

    void addPercBlip(float x, float y, float t, float l, float r, float b, float g) {
      float xn = normaliseX(x);
      float yn = normaliseY(y);
      float yu = normaliseY(y + (t / 2));
      float yd = normaliseY(y - (t / 2));
      float xa = normaliseX(x + l);

      Vertex v0 = {{xn, yu}, {r, g, b}};
      Vertex v1 = {{xn, yd}, {r, g, b}};
      Vertex v2 = {{xa, yn}, {r, g, b}};
      vertices.insert(vertices.end(), {v0, v1, v2});
    }

    void addPartialXPercBlip(float x, float y, float t, float l, float s, float r, float b, float g) {
      float xn = normaliseX(x);
      float yu = normaliseY(y + (t / 2));
      float yd = normaliseY(y - (t / 2));
      float xs = normaliseX(x + s);
      float h = (l - s) * (t / 2) / (2 * l);
      float yut = normaliseY(y + h);
      float ydt = normaliseY(y - h);

      Vertex v0 = {{xn, yu}, {r, g, b}};
      Vertex v1 = {{xn, yd}, {r, g, b}};
      Vertex v2 = {{xs, yut}, {r, g, b}};
      Vertex v3 = {{xs, ydt}, {r, g, b}};
      vertices.insert(vertices.end(), {v0, v1, v2, v2, v1, v3});
    }

    void addPartialYPercBlip(float x, float y, float t, float l, float s, float r, float g, float b) {
      float xn = normaliseX(x);
      float xs = normaliseX(x + l - (2 * l * std::abs(s) / t));
      if (s > 0.0f) {
        float yu = normaliseY(y + s);
        float yd = normaliseY(y - (t / 2));
        Vertex v0 = {{xn, yu}, {r, g, b}};
        Vertex v1 = {{xn, yd}, {r, g, b}};
        Vertex v2 = {{xs, yu}, {r, g, b}};
        Vertex v3 = {{normaliseX(x + l), normaliseY(y)}, {r, g, b}};
        vertices.insert(vertices.end(), {v0, v1, v2, v1, v2, v3});
      } else {
        //float yu = normaliseY(y + (t / 2));
        //float yd = normaliseY(y + s);
        //Vertex v0 = {{xn, yd}, {r, g, b}};
        //Vertex v1 = {{}}
      }
    }

    void addCircle(float cx, float cy, float radius, float r, float g, float b, int segments = 32) {
        // Convert center & radius into NDC
        float cx_ndc = (cx / windowWidth)  * 2.0f - 1.0f;
        float cy_ndc = 1.0f - (cy / windowHeight) * 2.0f;

        float rx_ndc = (radius / windowWidth)  * 2.0f;
        float ry_ndc = (radius / windowHeight) * 2.0f;

        // Center vertex
        Vertex center = {{cx_ndc, cy_ndc}, {r, g, b}};

        float step = 2.0f * M_PI / segments;

        for (int i = 0; i < segments; i++) {
            float a0 = i * step;
            float a1 = (i + 1) * step;

            float x0 = cx_ndc + cos(a0) * rx_ndc;
            float y0 = cy_ndc + sin(a0) * ry_ndc;

            float x1 = cx_ndc + cos(a1) * rx_ndc;
            float y1 = cy_ndc + sin(a1) * ry_ndc;

            Vertex v0 = center;
            Vertex v1 = {{x0, y0}, {r, g, b}};
            Vertex v2 = {{x1, y1}, {r, g, b}};

            // Add triangle (center, v1, v2)
            vertices.push_back(v0);
            vertices.push_back(v1);
            vertices.push_back(v2);
        }
    }


    void updateVertexData() {
        if (vertices.empty()) return;
        void* data;
        VkDeviceSize size = sizeof(Vertex) * vertices.size();
        vkMapMemory(device, vertexMemory, 0, size, 0, &data);
        memcpy(data, vertices.data(), (size_t)size);
        vkUnmapMemory(device, vertexMemory);
    }

    void onResize(SDL_Window* window) {
        vkDeviceWaitIdle(device);

        // Get ACTUAL framebuffer size
        int dw, dh;
        SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
        textRenderer.onResize(dw, dh);

        windowWidth  = dw;
        windowHeight = dh;
        swapchainExtent = { (uint32_t)dw, (uint32_t)dh };

        // Destroy old resources
        for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (auto iv : imageViews) vkDestroyImageView(device, iv, nullptr);
        vkDestroySwapchainKHR(device, swapchain, nullptr);

        // Recreate swapchain & dependent objects
        createSwapchain();
        createFramebuffers();

        vertices.clear();
        updateVertexData(); 

        createCommandBuffers();
    }
    
    uint32_t getWindowWidth() const { return windowWidth; }
    uint32_t getWindowHeight() const { return windowHeight; }

    void createSpectrogramPipeline(); // call after createRenderPass()
    void uploadSpectrogramR8(const uint8_t* data, int w, int h);

    void createTransientQuadBuffer()
    {
        createVulkanBuffer(device, physicalDevice,
            TRANSIENT_QUAD_BUFFER_SIZE,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            transientQuadBuffer,
            transientQuadMemory, 
            &transientQuadMapped
        );
    }

    void updateTransientVertexBuffer(
        const Vertex* data,
        size_t sizeBytes
    ) {
        if (sizeBytes > TRANSIENT_QUAD_BUFFER_SIZE) {
            throw std::runtime_error("Transient quad buffer overflow");
        }

        memcpy(transientQuadMapped, data, sizeBytes);
    }

    void destroyTransientQuadBuffer()
    {
        if (transientQuadMapped) {
            vkUnmapMemory(device, transientQuadMemory);
            transientQuadMapped = nullptr;
        }

        vkDestroyBuffer(device, transientQuadBuffer, nullptr);
        vkFreeMemory(device, transientQuadMemory, nullptr);
    }

    VkDevice getDevice() {
        return device;
    }

    void createImage2D(
    int w, int h,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory
    ) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.extent = { (uint32_t)w, (uint32_t)h, 1 };
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.format = format;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage = usage;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &ci, nullptr, &image) != VK_SUCCESS)
            throw std::runtime_error("createImage2D failed");

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req);

        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateMemory image failed");

        vkBindImageMemory(device, image, memory, 0);
    }

    VkImageView createImageView2D(VkImage image, VkFormat format)
    {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel = 0;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount = 1;

        VkImageView view{};
        if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("createImageView2D failed");
        return view;
    }

    void transitionImageLayout(
        VkImage image, VkFormat,
        VkImageLayout oldLayout, VkImageLayout newLayout
    ) {
        VkCommandBuffer cmd = beginSingleTimeCommands(device, graphicsFamily);

        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;

        VkPipelineStageFlags srcStage = 0, dstStage = 0;

        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            throw std::runtime_error("unsupported layout transition");
        }

        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        endSingleTimeCommands(device, cmd, graphicsQueue, graphicsFamily);
    }

    void copyBufferToImage(VkBuffer buffer, VkImage image, int w, int h)
    {
        VkCommandBuffer cmd = beginSingleTimeCommands(device, graphicsFamily);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;   // tightly packed
        region.bufferImageHeight = 0; // tightly packed
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0,0,0};
        region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };

        vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        endSingleTimeCommands(device, cmd, graphicsQueue, graphicsFamily);
    }



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

    TextRenderer textRenderer;

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

    std::vector<Text> texts;

    // Where each layer after the first begins (see nextLayer())
    struct LayerStart { size_t vertex; size_t text; };
    std::vector<LayerStart> layerStarts;

    // --- Spectrogram texture resources ---
    VkImage spectroImage{};
    VkDeviceMemory spectroMemory{};
    VkImageView spectroView{};
    VkSampler spectroSampler{};

    VkDescriptorSetLayout spectroSetLayout{};
    VkDescriptorPool spectroDescPool{};
    VkDescriptorSet spectroDescSet{};

    VkPipelineLayout spectroPipelineLayout{};
    VkPipeline spectroPipeline{};

    // Transient quad buffer
    VkBuffer transientQuadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory transientQuadMemory = VK_NULL_HANDLE;
    void* transientQuadMapped = nullptr;

    bool hasSpectrogram = false;
    int spectroW = 0, spectroH = 0;

    float normaliseX(float x) {
      return (x / windowWidth) * 2.0f - 1.0f;
    }

    float normaliseY(float y) {
      return 1.0f - (y / windowHeight) * 2.0f;
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT             messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void*                                       pUserData)
    {
      (void)messageSeverity; (void)messageType; (void)pUserData;
      std::cerr << "Vulkan Validation: " << pCallbackData->pMessage << std::endl;
      return VK_FALSE;
    }

    // Private helper functions
    int createInstance() {
        // ---- Vulkan instance ----
        unsigned int extCount = 0;
        SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr);
        std::vector<const char*> extensions(extCount);
        SDL_Vulkan_GetInstanceExtensions(window, &extCount, extensions.data());

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Sonnote";
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = extCount;
        createInfo.ppEnabledExtensionNames = extensions.data();

        const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = validationLayers;

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;

        createInfo.pNext = &debugCreateInfo;


        for (auto e : extensions)
        std::cout << "Using extension: " << e << std::endl;

        VkResult res = vkCreateInstance(&createInfo, nullptr, &instance);
        if (res != VK_SUCCESS) {
            std::cerr << "vkCreateInstance failed with code " << res << std::endl;
            return -1;
        }

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");

        VkDebugUtilsMessengerEXT debugMessenger;
        if (func != nullptr) {
            func(instance, &debugCreateInfo, nullptr, &debugMessenger);
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
        VkDeviceSize bufferSize = sizeof(Vertex) * 100000; // can hold many rectangles

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

        // ---- Dynamic viewport & scissor ----
        std::array<VkDynamicState, 2> dynStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };

        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = (uint32_t)dynStates.size();
        dyn.pDynamicStates = dynStates.data();

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

        // ---- NO fixed viewport/scissor here ----
        VkPipelineViewportStateCreateInfo viewportState {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

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
        pipeInfo.pDynamicState = &dyn;
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

        // for (size_t i = 0; i < commandBuffers.size(); i++) {
        //     VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        //     vkBeginCommandBuffer(commandBuffers[i], &beginInfo);

        //     VkClearValue clearColor = {0.1f, 0.1f, 0.1f, 1.0f};
        //     VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        //     rpBegin.renderPass = renderPass;
        //     rpBegin.framebuffer = framebuffers[i];
        //     rpBegin.renderArea.extent = swapchainExtent;
        //     rpBegin.clearValueCount = 1;
        //     rpBegin.pClearValues = &clearColor;
        //     vkCmdBeginRenderPass(commandBuffers[i], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        //     vkCmdBindPipeline(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        //     VkBuffer buffers[] = {vertexBuffer};
        //     VkDeviceSize offsets[] = {0};
        //     vkCmdBindVertexBuffers(commandBuffers[i], 0, 1, buffers, offsets);
        //     vkCmdDraw(commandBuffers[i], 3, 1, 0, 0);
        //     vkCmdEndRenderPass(commandBuffers[i]);
        //     vkEndCommandBuffer(commandBuffers[i]);
        // }
    }

    
    void createSyncObjects() {
        // ---- Sync objects ----
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &semInfo, nullptr, &imageAvailable);
        vkCreateSemaphore(device, &semInfo, nullptr, &renderFinished);
    }

    VkShaderModule loadShaderModule(const std::string& path);


};

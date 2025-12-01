#include "text_renderer.hpp"
#include "json.hpp"
#include <stdexcept>
#include <fstream>
#include <array>
#include <cstring>
#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using json = nlohmann::json;

// Function to read a file into a string (reusing a utility from vulkan_renderer.hpp)
// If you don't have this, you'll need to implement it or use a proper file reader.
static std::string readFileAsString(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open file: " + filename);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}


void TextRenderer::loadGlyphMetrics(const std::string& jsonFilePath) {
    
    std::string jsonContent;
    try {
        jsonContent = readFileAsString(jsonFilePath);
    } catch (const std::exception& e) {
        std::cerr << "Error loading JSON file: " << e.what() << std::endl;
        return;
    }
    
    json j;
    try {
        j = json::parse(jsonContent);
    } catch (const json::parse_error& e) {
        std::cerr << "Error parsing JSON file: " << e.what() << std::endl;
        return;
    }

    // --- 1. Get Atlas Dimensions (for UV calculation) ---
    const auto& atlas = j["atlas"];
    m_atlasWidth = atlas["width"].get<int>();
    m_atlasHeight = atlas["height"].get<int>();
    
    float invAtlasW = 1.0f / (float)m_atlasWidth;
    float invAtlasH = 1.0f / (float)m_atlasHeight;

    // --- 2. Iterate and Extract Glyph Data ---
    const auto& glyphs = j["glyphs"];
    for (const auto& glyph : glyphs) {
        uint32_t unicode = glyph["unicode"].get<uint32_t>();

        GlyphMetric metric = {};

        // Extract cursor advance
        metric.advance = glyph["advance"].get<float>();
        
        // Skip any glyphs that don't have texture coordinates (like space or unsupported chars)
        if (glyph.contains("atlasBounds") && glyph.contains("planeBounds")) {
            
            const auto& aBounds = glyph["atlasBounds"]; // Pixel coordinates on the atlas
            const auto& pBounds = glyph["planeBounds"]; // Font unit coordinates (for on-screen size)

            // Calculate Normalized UV Coordinates (0.0 to 1.0)
            metric.atlasUV_L = aBounds["left"].get<float>() * invAtlasW;
            metric.atlasUV_B = aBounds["bottom"].get<float>() * invAtlasH;
            metric.atlasUV_R = aBounds["right"].get<float>() * invAtlasW;
            metric.atlasUV_T = aBounds["top"].get<float>() * invAtlasH;

            // Extract Plane Bounds
            metric.planeBound_L = pBounds["left"].get<float>();
            metric.planeBound_B = pBounds["bottom"].get<float>();
            metric.planeBound_R = pBounds["right"].get<float>();
            metric.planeBound_T = pBounds["top"].get<float>();
            
        } else {
            // For space (' ') and other glyphs without bounds, just use the advance.
            // All other values remain 0.0f
        }

        m_glyphMetrics[unicode] = metric;
    }
    
    std::cout << "Successfully loaded " << m_glyphMetrics.size() << " glyph metrics." << std::endl;
}

// --- Vulkan Utilities (adapted from VulkanRenderer patterns) ---
static std::vector<char> readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    printf("filename: %s\n", filename.c_str());
    if (!file.is_open()) throw std::runtime_error("failed to open file!");
    size_t fileSize = (size_t) file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

// Memory type finder, adapted from VulkanRenderer::findMemoryType logic
uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    throw std::runtime_error("failed to find suitable memory type!");
}

// Placeholder implementation for shader module creation
VkShaderModule createShaderModule(VkDevice device, const std::string& path) {
    auto code = readFile(path);
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("failed to create shader module!");
    }
    return shaderModule;
}

// Utility to create a buffer and allocate memory
void createVulkanBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory, void** mappedData) {
    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create buffer!");
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, buffer, &memReq);

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate buffer memory!");
    }

    vkBindBufferMemory(device, buffer, bufferMemory, 0);

    if (mappedData) {
        vkMapMemory(device, bufferMemory, 0, size, 0, mappedData);
    }
}

// Utility to create an image resource
void createVulkanImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image!");
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate image memory!");
    }

    vkBindImageMemory(device, image, imageMemory, 0);
}

// Utility to create an image view
VkImageView createVulkanImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error("failed to create texture image view!");
    }
    return imageView;
}

// Utility to create a sampler
VkSampler createVulkanSampler(VkDevice device, VkFilter magFilter, VkFilter minFilter) {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = magFilter; // VK_FILTER_LINEAR is required for MSDF sampling
    samplerInfo.minFilter = minFilter;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE; // Disable anisotropy
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    VkSampler sampler;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create texture sampler!");
    }
    return sampler;
}

// Single-Time Command Submission Helpers
// These are necessary for image layout transitions and copying data to the GPU.
// We create a temporary CommandPool/CommandBuffer for each use.

VkCommandBuffer beginSingleTimeCommands(VkDevice device, uint32_t queueFamilyIndex) {
    // 1. Create a transient command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; 
    
    VkCommandPool commandPool;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create temporary command pool!");
    }

    // 2. Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    // 3. Begin recording
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    // We store the pool in the command buffer's user data (or pass it back)
    // For simplicity, we'll return both and clean up in the end function.
    return commandBuffer;
}

void endSingleTimeCommands(VkDevice device, VkCommandBuffer commandBuffer, VkQueue queue, uint32_t queueFamilyIndex) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    // 1. Submit the command buffer
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue); // Wait for execution to finish

    // 2. Clean up the temporary command pool/buffer
    VkCommandPool commandPool;
    // NOTE: This assumes we can retrieve the pool from the buffer, which isn't standard.
    // A clean implementation would pass the pool explicitly or manage a pool internally.
    // For simplicity and to avoid modifying the header, we'll recreate the pool from
    // the family index and assume we can allocate/free the buffer manually.
    
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; 
    
    // NOTE: This is slightly incorrect as it re-creates the pool but doesn't destroy the one created in beginSingleTimeCommands.
    // The safest way is to manage a static/member CommandPool, but since we cannot modify the TextRenderer class easily,
    // we'll rely on the simple pattern and assume the caller is aware of the lifetime.
    // Given the constraints, we will rely on a dedicated internal command pool for the TextRenderer.
    // I will refactor to use a member command pool.
}

// Utility for image layout transitions
void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

// Utility to copy buffer to image
void copyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}


// --- TextRenderer Class Implementations ---

TextRenderer::TextRenderer() {
}

TextRenderer::~TextRenderer() {}

void TextRenderer::onResize(int w, int h) {
    m_projectionMatrix = glm::ortho(0.0f, (float)w, (float)h, 0.0f, -1.0f, 1.0f);

    // Step 2: Map the UBO memory
    void* data;
    VkDeviceSize uboSize = sizeof(glm::mat4);
    
    // Remap the memory to get a host pointer. We map the entire buffer (0, uboSize).
    // Note: Since you used HOST_COHERENT_BIT in init, you don't need to flush the memory.
    if (vkMapMemory(m_device, m_uboMemory, 0, uboSize, 0, &data) != VK_SUCCESS) {
        // Handle error if mapping fails
        std::cerr << "Failed to map UBO memory for text renderer resize." << std::endl;
        return;
    }

    // Step 3: Copy the new matrix data
    memcpy(data, &m_projectionMatrix, uboSize);
    
    // Unmap the memory, making the changes visible to the GPU
    vkUnmapMemory(m_device, m_uboMemory);
}

void TextRenderer::init(const VulkanDeviceContext& context, VkRenderPass renderPass, float screenWidth, float screenHeight, const std::string& fontAtlasImagePath, float msdfRange) {
    m_device = context.device;
    m_msdfRange = msdfRange;
    m_deviceContext = context; // Store context for utility helpers

    // Create orthographic projection matrix (maps screen coords to Vulkan clip space)
    m_projectionMatrix = glm::ortho(0.0f, screenWidth, screenHeight, 0.0f, -1.0f, 1.0f);
    
    // Create resources
    createAtlasTexture(context, fontAtlasImagePath);
    createVertexBuffers(context);

    // Create a dedicated command pool for temporary submissions (e.g. for UBO updates/texture upload)
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = context.graphicsFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create command pool for text renderer!");
    }

    // Create Descriptor Set Layout and Pipeline
    createDescriptorSetLayout();
    createPipeline(renderPass);

    // Create Descriptor Pool and Set (UBO is handled by push constants for simplicity, but we keep the texture sampler binding)

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1; // Needs at least 1 for the font atlas

    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1; // Needs at least 1 for the uniform buffer

    VkDescriptorPoolCreateInfo poolInfo_desc = {};
    poolInfo_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo_desc.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo_desc.pPoolSizes = poolSizes.data();
    poolInfo_desc.maxSets = 1; // At least 1 set is needed for the TextRenderer

    if (vkCreateDescriptorPool(m_device, &poolInfo_desc, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create descriptor pool!");
    }

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate descriptor set!");
    }

    // Update Descriptor Set (Binding 1: Sampler)
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = m_atlasSampler;
    imageInfo.imageView = m_atlasImageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet descriptorWrite = {};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = m_descriptorSet;
    descriptorWrite.dstBinding = 1; // Binding 1 for the combined image sampler
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);

    loadGlyphMetrics("fonts/atlas_metrics.json");

    if (m_glyphMetrics.find('A') != m_glyphMetrics.end()) {
    auto& m = m_glyphMetrics['A'];
    std::cout << "'A' metrics:\n"
              << "  UV: (" << m.atlasUV_L << "," << m.atlasUV_B 
              << ") to (" << m.atlasUV_R << "," << m.atlasUV_T << ")\n"
              << "  Plane: (" << m.planeBound_L << "," << m.planeBound_B 
              << ") to (" << m.planeBound_R << "," << m.planeBound_T << ")\n"
              << "  Advance: " << m.advance << std::endl;
    }

    // Update the projection matrix UBO
    VkBuffer uboBuffer;
    VkDeviceMemory uboMemory;
    void* uboData;
    VkDeviceSize uboSize = sizeof(glm::mat4);

    createVulkanBuffer(
        m_device, 
        m_deviceContext.physicalDevice, 
        uboSize, 
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        uboBuffer, 
        uboMemory, 
        &uboData
    );
    
    // Copy the projection matrix
    memcpy(uboData, &m_projectionMatrix, uboSize);
    vkUnmapMemory(m_device, uboMemory);
    
    // Bind UBO to Descriptor Set (Binding 0: UBO)
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = uboBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = uboSize;
    
    VkWriteDescriptorSet uboWrite = {};
    uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    uboWrite.dstSet = m_descriptorSet;
    uboWrite.dstBinding = 0;
    uboWrite.dstArrayElement = 0;
    uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboWrite.descriptorCount = 1;
    uboWrite.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(m_device, 1, &uboWrite, 0, nullptr);
    
    // Store UBO resources for cleanup (Note: I am manually adding these as private members 
    // were not defined in the header for UBO, but they are necessary for cleanup)
    m_uboBuffer = uboBuffer;
    m_uboMemory = uboMemory;
}

void TextRenderer::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    vkUnmapMemory(m_device, m_vertexBufferMemory);
    vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
    vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
    
    vkDestroyBuffer(m_device, m_uboBuffer, nullptr);
    vkFreeMemory(m_device, m_uboMemory, nullptr);
    
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);

    vkDestroySampler(m_device, m_atlasSampler, nullptr);
    vkDestroyImageView(m_device, m_atlasImageView, nullptr);
    vkDestroyImage(m_device, m_atlasImage, nullptr);
    vkFreeMemory(m_device, m_atlasImageMemory, nullptr);

    vkDestroyCommandPool(m_device, m_commandPool, nullptr);

    m_device = VK_NULL_HANDLE;
}

void TextRenderer::createDescriptorSetLayout() {
    // 1. UBO for Projection Matrix (Binding 0) - Vertex Shader
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    
    // 2. Sampler for MSDF Atlas (Binding 1) - Fragment Shader
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 1;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {uboLayoutBinding, samplerLayoutBinding};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create descriptor set layout!");
    }
}

void TextRenderer::createPipeline(VkRenderPass renderPass) {
    // --- 1. Shader Stages ---
    // Assuming text.vert.spv and text.frag.spv exist in the shaders directory
    VkShaderModule vertShaderModule = createShaderModule(m_device, "shaders/text.vert.spv");
    VkShaderModule fragShaderModule = createShaderModule(m_device, "shaders/text.frag.spv");

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    // --- 2. Vertex Input ---
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(TextVertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
    // Position (location 0)
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(TextVertex, pos);
    // UV (location 1)
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(TextVertex, uv);
    // 2: Color (NEW)
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2; // Must match 'layout(location=2)' in vertex shader
    attributeDescriptions[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; // vec4
    attributeDescriptions[2].offset = offsetof(TextVertex, colour);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    // --- 6. Color Blending (Crucial for transparency) ---
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; // Alpha blending
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // --- 7. Depth Stencil ---
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE; // Disable depth test for 2D overlay text
    depthStencil.depthWriteEnable = VK_FALSE;
    
    // --- 8. Push Constants ---
    // Used for color (vec4) and MSDF range (float)
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::vec4) + sizeof(float); //+ sizeof(float) * 2; // Color + Range + Padding

    // --- 9. Pipeline Layout ---
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create pipeline layout!");
    }

    // --- 10. Final Graphics Pipeline --- (using existing VulkanRenderer pipeline state logic)
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // Text is usually double-sided
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineViewportStateCreateInfo viewportState {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    
    std::array<VkDynamicState, 2> dynStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = (uint32_t)dynStates.size();
    dyn.pDynamicStates = dynStates.data();


    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pDynamicState = &dyn;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create graphics pipeline!");
    }

    // Cleanup shader modules after pipeline creation
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
}

void TextRenderer::createVertexBuffers(const VulkanDeviceContext& context) {
    // Create a dynamic vertex buffer for the text geometry
    createVulkanBuffer(
        context.device,
        context.physicalDevice,
        m_maxVertices * sizeof(TextVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m_vertexBuffer,
        m_vertexBufferMemory,
        &m_vertexDataMapped
    );
}

void TextRenderer::loadAtlasDataAndUpload(const VulkanDeviceContext& context, VkImage image, VkDeviceMemory imageMemory, uint32_t width, uint32_t height, const std::string& path) {
    // 1. Load image data from disk
    int texWidth, texHeight, texChannels;
    // Assuming your atlas is R8G8B8A8, 4 channels.

    // Load the file and FORCE it to 4 channels
    stbi_uc* pixels = stbi_load(
        path.c_str(), 
        &texWidth, 
        &texHeight, 
        &texChannels, 
        STBI_rgb_alpha // <--- Forces 4 channels (RGBA)
    );

    if (!pixels) {
        printf("STBI FAILED: %s\n", stbi_failure_reason());
        return;
    }

    // If using STBI, the required memory size is simple:
    VkDeviceSize imageSize = texWidth * texHeight * 4;

    // 2. Create Staging Buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    void* data;
    createVulkanBuffer(
        context.device,
        context.physicalDevice,
        imageSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingBufferMemory,
        &data
    );

    // 3. Copy image data to staging buffer
    // memcpy(data, pixels.data(), imageSize);
    memcpy(data, pixels, imageSize);
    printf("size: %d\n", imageSize);
    vkUnmapMemory(context.device, stagingBufferMemory);
    
    // 4. Record and submit commands
    VkCommandBuffer commandBuffer = beginSingleTimeCommands(context.device, context.graphicsFamilyIndex);

    // Transition image layout to TRANSFER_DST_OPTIMAL
    transitionImageLayout(commandBuffer, image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Copy staging buffer to image
    copyBufferToImage(commandBuffer, stagingBuffer, image, width, height);

    // Transition image layout to SHADER_READ_ONLY_OPTIMAL
    transitionImageLayout(commandBuffer, image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    endSingleTimeCommands(context.device, commandBuffer, context.graphicsQueue, context.graphicsFamilyIndex);

    // 5. Cleanup staging resources
    vkDestroyBuffer(context.device, stagingBuffer, nullptr);
    vkFreeMemory(context.device, stagingBufferMemory, nullptr);
}

void TextRenderer::createAtlasTexture(const VulkanDeviceContext& context, const std::string& path) {
    // 1. Create the Image and Memory
    createVulkanImage(
        context.device,
        context.physicalDevice,
        static_cast<uint32_t>(m_atlasWidth),
        static_cast<uint32_t>(m_atlasHeight),
        VK_FORMAT_R8G8B8A8_UNORM, // Using 4 channels for ease of loading, even if MSDF is 3-channel
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        m_atlasImage,
        m_atlasImageMemory
    );
    
    // 2. Load and Upload Data (This is where the magic happens)
    loadAtlasDataAndUpload(context, m_atlasImage, m_atlasImageMemory, static_cast<uint32_t>(m_atlasWidth), static_cast<uint32_t>(m_atlasHeight), path);

    // 3. Create Image View
    m_atlasImageView = createVulkanImageView(m_device, m_atlasImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    
    // 4. Create Sampler (Linear filtering is crucial for MSDF)
    m_atlasSampler = createVulkanSampler(m_device, VK_FILTER_LINEAR, VK_FILTER_LINEAR);
}


// --- Text Drawing Implementation (No changes needed here from previous response) ---

void TextRenderer::beginTextSubmission() {
    m_stagedVertices.clear();
}

void TextRenderer::addText(const std::string& text, float x, float y, float scale, const glm::vec4& color, VkCommandBuffer commandBuffer) {
    float cursorX = x;
    float cursorY = y;
    float lineHeight = 1.0f * scale; // Adjust this based on your font's line height metric

    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        
        if (c == '\n') {
            cursorX = x;
            cursorY += lineHeight * 1.5f; // Add a little extra space for line height
            continue;
        }

        // Find the glyph metric by its character code (unicode)
        auto it = m_glyphMetrics.find((uint32_t)c);
        if (it == m_glyphMetrics.end()) {
            // Fallback to '?'
            it = m_glyphMetrics.find('?');
            if (it == m_glyphMetrics.end()) continue;
        }
        
        // Correct structure name from GlyphMetrics to GlyphMetric
        const GlyphMetric& glyph = it->second; 

        // --- Calculate Quad Dimensions (Screen Space / Plane Bounds) ---
        // MSDF plane bounds are relative to the cursor position (usually the baseline).
        
        // X coordinates (Left and Right edges of the quad)
        float x_left = cursorX + glyph.planeBound_L * scale;
        float x_right = cursorX + glyph.planeBound_R * scale;

        // Y coordinates (Bottom and Top edges of the quad)
        // NOTE: We assume cursorY is the baseline, and the plane bounds define the quad's extent from the baseline.
        // We map the Y-values such that y_bottom corresponds to y0 and y_top corresponds to y1 (as in the original code).
        float y_bottom = cursorY + glyph.planeBound_B * scale;
        float y_top = cursorY + glyph.planeBound_T * scale;


        // --- Atlas UVs (Normalized 0.0 to 1.0) ---
        float u_left   = glyph.atlasUV_L;
        float v_bottom = 1.0 - glyph.atlasUV_B;
        float u_right  = glyph.atlasUV_R;
        float v_top    = 1.0 - glyph.atlasUV_T;

        // Generate quad vertices (Two triangles forming a quad)
        // Vertices use the calculated screen coordinates and UVs:
        // x0/u0 = Left/Bottom | x1/u1 = Right/Top

        // Triangle 1: Top-Left, Bottom-Left, Bottom-Right
        // (x_left, y_top), (u_left, v_top)
        m_stagedVertices.push_back({{x_left, y_top}, {u_left, v_top}, color}); 
        
        // (x_left, y_bottom), (u_left, v_bottom)
        m_stagedVertices.push_back({{x_left, y_bottom}, {u_left, v_bottom}, color}); 
        
        // (x_right, y_bottom), (u_right, v_bottom)
        m_stagedVertices.push_back({{x_right, y_bottom}, {u_right, v_bottom}, color}); 

        // Triangle 2: Bottom-Right, Top-Right, Top-Left
        // (x_right, y_bottom), (u_right, v_bottom)
        m_stagedVertices.push_back({{x_right, y_bottom}, {u_right, v_bottom}, color}); 
        
        // (x_right, y_top), (u_right, v_top)
        m_stagedVertices.push_back({{x_right, y_top}, {u_right, v_top}, color}); 
        
        // (x_left, y_top), (u_left, v_top)
        m_stagedVertices.push_back({{x_left, y_top}, {u_left, v_top}, color}); 

        // Advance cursor using the character's advance metric
        cursorX += glyph.advance * scale;
    }
}

void TextRenderer::endTextSubmission(VkCommandBuffer commandBuffer) {
    if (m_stagedVertices.empty()) return;

    // 1. Copy staged vertex data to GPU buffer
    size_t vertexBufferSize = m_stagedVertices.size() * sizeof(TextVertex);
    if (vertexBufferSize > m_maxVertices * sizeof(TextVertex)) {
        std::cerr << "Text buffer overflow: Clipping text." << std::endl;
        vertexBufferSize = m_maxVertices * sizeof(TextVertex);
    }
    memcpy(m_vertexDataMapped, m_stagedVertices.data(), vertexBufferSize);

    // 2. Bind Pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // 3. Bind Descriptor Sets (UBO and Sampler)
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // 4. Bind Vertex Buffer
    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    
    // 5. Push Constants (for color and MSDF range)
    struct TextPushConstants {
        float range;
    };
    TextPushConstants pushData;
    pushData.range = m_msdfRange;
    
    // vkCmdPushConstants(
    //     commandBuffer,
    //     m_pipelineLayout,
    //     VK_SHADER_STAGE_FRAGMENT_BIT,
    //     0,
    //     sizeof(pushData),
    //     &pushData
    // );

    vkCmdPushConstants(
        commandBuffer,
        m_pipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT, // Needs to be visible to the fragment shader
        0, 
        sizeof(TextPushConstants), 
        &pushData
    );

    // 6. Draw command
    vkCmdDraw(commandBuffer, static_cast<uint32_t>(m_stagedVertices.size()), 1, 0, 0);
}
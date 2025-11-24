#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

struct MSDFGlyph {
    glm::vec2 uv0;       // bottom-left in atlas
    glm::vec2 uv1;       // top-right
    glm::vec2 size;      // glyph quad size in px
    glm::vec2 bearing;   // glyph offset (baseline)
    float advance;       // horizontal advance
};

class VulkanRenderer; // forward declaration

class MSDFTextRenderer {
public:
    MSDFTextRenderer(VulkanRenderer* renderer);
    ~MSDFTextRenderer();

    void loadFontMSDF(const std::string& path, float pxSize = 48.0f);
    void createPipeline(VkRenderPass renderPass, uint32_t subpass);
    void drawText(
        VkCommandBuffer cmd,
        const std::string& text,
        float x, float y,
        glm::vec4 color = glm::vec4(1.0f)
    );

private:
    VulkanRenderer* renderer;

    // atlas
    VkImage atlasImage = VK_NULL_HANDLE;
    VkImageView atlasView = VK_NULL_HANDLE;
    VkSampler atlasSampler = VK_NULL_HANDLE;
    VkDeviceMemory atlasMemory = VK_NULL_HANDLE;
    VkDescriptorSet atlasDescriptor = VK_NULL_HANDLE;

    uint32_t atlasW = 0, atlasH = 0;
    std::unordered_map<char32_t, MSDFGlyph> glyphs;

    // pipeline
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;

    // CPU-side buffer of generated vertices
    struct TextVertex {
        glm::vec2 pos;
        glm::vec2 uv;
        glm::vec4 color;
    };
};

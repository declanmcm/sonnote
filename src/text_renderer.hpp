#ifndef TEXT_RENDERER_HPP
#define TEXT_RENDERER_HPP

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp> // Required for glm::ortho
#include <string>
#include <vector>
#include <map>
#include <stdexcept>

// Forward declarations for your Vulkan setup
struct VulkanDeviceContext {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsFamilyIndex = 0;
};

// --- Data Structures ---

// struct GlyphMetrics {
//     float advance = 0.0f;     // How far to move for the next character
//     float bearingX = 0.0f;    // Offset from cursor to start of glyph
//     float bearingY = 0.0f;    // Offset from baseline to top of glyph
//     float width = 0.0f;       // Pixel width of glyph image
//     float height = 0.0f;      // Pixel height of glyph image
//     glm::vec4 atlasUV;        // (u0, v0, u1, v1) texture coordinates in the atlas
// };

struct GlyphMetric {
    // Texture coordinates (normalized UVs, 0.0 to 1.0)
    float atlasUV_L; // Left U
    float atlasUV_B; // Bottom V
    float atlasUV_R; // Right U
    float atlasUV_T; // Top V
    
    // On-screen bounding box (plane bounds, scaled by font size)
    float planeBound_L;
    float planeBound_B;
    float planeBound_R;
    float planeBound_T;
    
    // Cursor advance after drawing
    float advance; 
};


struct TextVertex {
    glm::vec2 pos; // Screen-space position
    glm::vec2 uv;  // Texture coordinate
    glm::vec4 colour;
};

// --- Main Text Renderer Class ---

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    void init(
        const VulkanDeviceContext& context,
        VkRenderPass renderPass,
        float screenWidth,
        float screenHeight,
        const std::string& fontAtlasImagePath,
        float msdfRange
    );

    void destroy();

    void onResize(int w, int h);

    void beginTextSubmission(); // <-- MISSING DECLARATION FOR ERROR 
    void addText(
        const std::string& text,
        float x, float y,
        float scale,
        const glm::vec4& color,
        VkCommandBuffer commandBuffer
    ); // <-- MISSING DECLARATION FOR ERROR 
    void endTextSubmission(VkCommandBuffer commandBuffer); // <-- MISSING DECLARATION FOR ERROR

    //layout(binding = 1) uniform sampler2D msdfAtlasSampler;
private:
    // Core Vulkan Objects
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    
    // NEW: Context & Command Pool
    VulkanDeviceContext m_deviceContext;
    VkCommandPool m_commandPool = VK_NULL_HANDLE; 
    
    // Texture Atlas (unchanged)
    VkImage m_atlasImage = VK_NULL_HANDLE;
    VkDeviceMemory m_atlasImageMemory = VK_NULL_HANDLE;
    VkImageView m_atlasImageView = VK_NULL_HANDLE;
    VkSampler m_atlasSampler = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    
    // NEW: UBO for Projection Matrix
    VkBuffer m_uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_uboMemory = VK_NULL_HANDLE;

    // Geometry Buffers (unchanged)
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
    void* m_vertexDataMapped = nullptr;
    size_t m_maxVertices = 4096;
    std::vector<TextVertex> m_stagedVertices;

    // Misc (unchanged)
    glm::mat4 m_projectionMatrix;
    float m_msdfRange = 4.0f;
    float m_atlasWidth = 1024.0f;
    float m_atlasHeight = 1024.0f;
    
    // Font Data (unchanged)
    std::map<char, GlyphMetric> m_glyphMetrics;
    void loadGlyphMetrics(const std::string& jsonFilePath);
    
    // Internal Vulkan helpers
    void createDescriptorSetLayout();
    void createPipeline(VkRenderPass renderPass);
    void createVertexBuffers(const VulkanDeviceContext& context);
    void createAtlasTexture(const VulkanDeviceContext& context, const std::string& path);
    
    // Replaced placeholder implementation
    void loadAtlasDataAndUpload(const VulkanDeviceContext& context, VkImage image, VkDeviceMemory imageMemory, uint32_t width, uint32_t height, const std::string& path); 
};

#endif // TEXT_RENDERER_HPP
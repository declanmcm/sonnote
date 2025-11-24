#include "msdf_text.hpp"
#include "vulkan_renderer.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// msdfgen
#include "msdfgen.h"
#include "msdfgen-ext.h"

#include <vector>
#include <codecvt>

MSDFTextRenderer::MSDFTextRenderer(VulkanRenderer* r) : renderer(r) {}

MSDFTextRenderer::~MSDFTextRenderer() {
    VkDevice dev = renderer->getDevice();

    if (atlasSampler) vkDestroySampler(dev, atlasSampler, nullptr);
    if (atlasView) vkDestroyImageView(dev, atlasView, nullptr);
    if (atlasImage) vkDestroyImage(dev, atlasImage, nullptr);
    if (atlasMemory) vkFreeMemory(dev, atlasMemory, nullptr);
    if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
    if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
}

void MSDFTextRenderer::loadFontMSDF(const std::string& path, float pxSize) {
    using namespace msdfgen;

    FreetypeHandle* ft = initializeFreetype();
    if (!ft) throw std::runtime_error("Could not init FreeType");

    FontHandle* font = loadFont(ft, path.c_str());
    if (!font) throw std::runtime_error("Could not load font");

    // characters to include
    const int firstChar = 32;
    const int lastChar  = 126;

    std::vector<Bitmap<float, 3>> bitmaps;
    bitmaps.reserve(lastChar - firstChar + 1);

    // Determine atlas size
    int glyphSize = (int)pxSize + 10;
    int cols = 16;
    int rows = (lastChar - firstChar + cols) / cols;

    atlasW = cols * glyphSize;
    atlasH = rows * glyphSize;

    std::vector<float> atlasPixels(atlasW * atlasH * 3, 0.0f);

    for (int i = firstChar; i <= lastChar; i++) {
        Shape shape;
        if (!loadGlyph(shape, font, i)) continue;

        shape.normalize();

        // Edge coloring
        edgeColoringSimple(shape, 3.0);

        Bitmap<float, 3> msdf(glyphSize, glyphSize);
        generateMSDF(
            msdf, shape, 4.0, 1.0, Vector2(pxSize, pxSize)
        );
        bitmaps.push_back(std::move(msdf));
    }

    // Blit into atlas
    int index = 0;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            if (index >= bitmaps.size()) break;
            auto& bmp = bitmaps[index];
            int gx = x * glyphSize;
            int gy = y * glyphSize;

            for (int j = 0; j < glyphSize; j++) {
                for (int i = 0; i < glyphSize; i++) {
                    const float* px = bmp(i, j); // pointer to 3 floats: {r,g,b}

                    int dst = 3 * ((gy + j) * atlasW + (gx + i));

                    atlasPixels[dst + 0] = px[0];
                    atlasPixels[dst + 1] = px[1];
                    atlasPixels[dst + 2] = px[2];
                }
            }

            // Store UV + metrics
            double advance;double adv = 0;
            getAdvance(adv, font, firstChar + index);

            double left, bottom, right, top;
            getBoundingBox(left, bottom, right, top, font, firstChar + index);

            // convert to float
            float glyphLeft   = float(left);
            float glyphBottom = float(bottom);
            float glyphWidth  = float(right - left);
            float glyphHeight = float(top - bottom);


            glyphs[firstChar + index] = {
                glm::vec2((float)gx / atlasW, (float)gy / atlasH),
                glm::vec2((float)(gx + glyphSize) / atlasW, (float)(gy + glyphSize) / atlasH),
                glm::vec2(glyphWidth, glyphHeight),
                glm::vec2(glyphLeft, glyphBottom),
                float(adv)
            };
            index++;
        }
    }

    // Upload atlas to Vulkan using existing renderer helpers
    renderer->createImageFromPixels(
        atlasPixels.data(), atlasW, atlasH, 3,
        &atlasImage, &atlasMemory
    );
    atlasView = renderer->createImageView(atlasImage);
    atlasSampler = renderer->createSampler();
    atlasDescriptor = renderer->createTextureDescriptor(atlasView, atlasSampler);

    destroyFont(font);
    deinitializeFreetype(ft);
}

void MSDFTextRenderer::createPipeline(VkRenderPass rp, uint32_t subpass) {
    // Uses your renderer's shader loader
    VkShaderModule vert = renderer->loadShader("shaders/msdf.vert.spv");
    VkShaderModule frag = renderer->loadShader("shaders/msdf.frag.spv");

    // Descriptor layout identical to your texture system
    VkDescriptorSetLayout dsl = renderer->textureDescriptorLayout;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;

    vkCreatePipelineLayout(renderer->device, &plci, nullptr, &layout);

    // Create a standard textured-quad pipeline using your helper
    pipeline = renderer->createTextPipeline(rp, subpass, layout, vert, frag);

    vkDestroyShaderModule(renderer->device, vert, nullptr);
    vkDestroyShaderModule(renderer->device, frag, nullptr);
}

void MSDFTextRenderer::drawText(
    VkCommandBuffer cmd,
    const std::string& text,
    float x, float y,
    glm::vec4 color
) {
    std::vector<TextVertex> verts;

    float penX = x;
    float penY = y;

    // convert UTF-8 → UTF-32
    std::u32string u32 = std::wstring_convert<
        std::codecvt_utf8<char32_t>, char32_t
    >{}.from_bytes(text);

    for (char32_t c : u32) {
        auto it = glyphs.find(c);
        if (it == glyphs.end()) continue;
        const MSDFGlyph& g = it->second;

        float x0 = penX + g.bearing.x;
        float y0 = penY - g.bearing.y;
        float x1 = x0 + g.size.x;
        float y1 = y0 + g.size.y;

        glm::vec2 uv0 = g.uv0;
        glm::vec2 uv1 = g.uv1;

        // 2 triangles
        verts.push_back({{x0,y0}, {uv0.x,uv1.y}, color});
        verts.push_back({{x1,y0}, {uv1.x,uv1.y}, color});
        verts.push_back({{x1,y1}, {uv1.x,uv0.y}, color});
        verts.push_back({{x0,y0}, {uv0.x,uv1.y}, color});
        verts.push_back({{x1,y1}, {uv1.x,uv0.y}, color});
        verts.push_back({{x0,y1}, {uv0.x,uv0.y}, color});

        penX += g.advance;
    }

    if (verts.empty()) return;

    renderer->bindPipeline(cmd, pipeline);
    renderer->bindDescriptor(cmd, atlasDescriptor, 0);

    renderer->drawTextVertices(cmd, verts.data(), verts.size());
}

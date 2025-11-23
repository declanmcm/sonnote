#include "vulkan_renderer.cpp"

struct Rect {
    float x;
    float y;
    float w;
    float h;
    float r;
    float g;
    float b;
};

int main()  {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Vulkan Triangle", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          WIDTH, HEIGHT, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    VulkanRenderer renderer = VulkanRenderer(window);

    bool running = true;
    bool waitingSecondClick = false;
    int x1 = 0, y1 = 0;
    std::vector<Rect> rects;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                running = false;


            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = (float)renderer.getWindowHeight() - e.button.y;

                int dw, dh;
                SDL_Vulkan_GetDrawableSize(window, &dw, &dh);

                // scale from logical → drawable
                float sx = float(mx) * float(dw) / float(renderer.getWindowWidth());
                float sy = float(my) * float(dh) / float(renderer.getWindowHeight());

                if (!waitingSecondClick) {
                    // First click
                    x1 = sx;
                    y1 = sy;
                    waitingSecondClick = true;
                    std::cout << "First click: " << x1 << "," << y1 << "\n";
                } else {
                    // Second click
                    int x2 = sx;
                    int y2 = sy;
                    waitingSecondClick = false;

                    float x = std::min(x1, x2) / (float)dw;
                    float y = std::min(y1, y2) / (float)dh;
                    float w = std::abs(x2 - x1) / (float)dw;
                    float h = std::abs(y2 - y1) / (float)dh;

                    rects.push_back({x, y, w, h, 0.2f, 0.7f, 0.9f});
                    std::cout << "Second click: " << x2 << "," << y2 << " -> Rect added\n";
                }
            }

            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                printf("changed size\n");
                renderer.onResize(window);
            }

        }

        // Clear vertex list and rebuild from rectangles
        renderer.vertices.clear();
        for (auto& r : rects) {
            int dw, dh;
            SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
            float x = r.x * dw;
            float y = r.y * dh;
            float w = r.w * dw;
            float h = r.h * dh;
            renderer.addRect(x, y, w, h, 0.2f, 0.7f, 0.9f); // bluish rectangles
        }

        renderer.updateVertexData();
        renderer.drawFrame();
    }

    SDL_Quit();
    return 0;
}
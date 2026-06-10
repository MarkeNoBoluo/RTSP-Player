#pragma once

struct AVFrame;

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool init(int width, int height) = 0;
    virtual void displayFrame(AVFrame* frame) = 0;
    virtual void setWindowSize(int w, int h) = 0;
    virtual void destroy() = 0;
};

#include <SDL.h>

class SDLRenderer : public IRenderer {
public:
    SDLRenderer(const char* title, int w, int h);
    ~SDLRenderer() override;

    bool init(int width, int height) override;
    void displayFrame(AVFrame* frame) override;
    void setWindowSize(int w, int h) override;
    void destroy() override;

    SDL_Window* window() const { return m_window; }

private:
    void recreateTexture(int width, int height);
    void updateDisplayRect();

    SDL_Window*   m_window   = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_texture  = nullptr;
    SDL_Rect      m_dstRect{0, 0, 0, 0};

    const char*   m_title;
    int m_texW = 0;
    int m_texH = 0;
    int m_winW = 0;
    int m_winH = 0;
};

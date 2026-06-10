#include "SDLRenderer.h"
#include "Common.h"
#include "logger/Logger.h"

#include <SDL.h>

SDLRenderer::SDLRenderer(const char* title, int w, int h)
    : m_title(title)
    , m_winW(w)
    , m_winH(h)
{
    m_window = SDL_CreateWindow(title,
                                SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                w, h,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!m_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
    if (!m_renderer) {
        LOG_ERROR("SDL_CreateRenderer failed: %s", SDL_GetError());
        return;
    }

    LOG_INFO("SDL renderer created: %dx%d", w, h);
}

SDLRenderer::~SDLRenderer() {
    destroy();
}

bool SDLRenderer::init(int width, int height) {
    recreateTexture(width, height);
    updateDisplayRect();
    return m_texture != nullptr;
}

void SDLRenderer::displayFrame(AVFrame* frame) {
    if (!m_renderer || !frame || !frame->data[0]) return;

    if (frame->width != m_texW || frame->height != m_texH || !m_texture) {
        recreateTexture(frame->width, frame->height);
        updateDisplayRect();
    }

    if (!m_texture) return;

    SDL_UpdateYUVTexture(m_texture, nullptr,
                         frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1],
                         frame->data[2], frame->linesize[2]);

    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, nullptr, &m_dstRect);
    SDL_RenderPresent(m_renderer);
}

void SDLRenderer::setWindowSize(int w, int h) {
    m_winW = w;
    m_winH = h;
    updateDisplayRect();
}

void SDLRenderer::destroy() {
    if (m_texture)  { SDL_DestroyTexture(m_texture);   m_texture  = nullptr; }
    if (m_renderer) { SDL_DestroyRenderer(m_renderer); m_renderer = nullptr; }
    if (m_window)   { SDL_DestroyWindow(m_window);     m_window   = nullptr; }
}

void SDLRenderer::recreateTexture(int width, int height) {
    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }

    m_texture = SDL_CreateTexture(m_renderer,
                                  SDL_PIXELFORMAT_IYUV,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  width, height);
    if (!m_texture) {
        LOG_ERROR("SDL_CreateTexture failed: %s", SDL_GetError());
        return;
    }

    m_texW = width;
    m_texH = height;
    LOG_INFO("Texture created: %dx%d", width, height);
}

void SDLRenderer::updateDisplayRect() {
    if (m_texW <= 0 || m_texH <= 0 || m_winW <= 0 || m_winH <= 0) {
        m_dstRect = {0, 0, m_winW, m_winH};
        return;
    }

    double vidAspect = (double)m_texW / m_texH;
    double winAspect = (double)m_winW / m_winH;

    if (vidAspect > winAspect) {
        m_dstRect.w = m_winW;
        m_dstRect.h = (int)(m_winW / vidAspect);
        m_dstRect.x = 0;
        m_dstRect.y = (m_winH - m_dstRect.h) / 2;
    } else {
        m_dstRect.h = m_winH;
        m_dstRect.w = (int)(m_winH * vidAspect);
        m_dstRect.x = (m_winW - m_dstRect.w) / 2;
        m_dstRect.y = 0;
    }
}

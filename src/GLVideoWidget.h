#pragma once

#include "Common.h"
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <mutex>
#include <cstdint>

class PlayerStats;

class GLVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit GLVideoWidget(QWidget* parent = nullptr);
    ~GLVideoWidget() override;

    void setDisplayFrame(const VideoFrame* frame);

    int videoWidth()  const { return m_videoWidth; }
    int videoHeight() const { return m_videoHeight; }

    void setStats(PlayerStats* stats) { m_stats = stats; }

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private:
    void setupTextures(int width, int height);
    void uploadTextures(AVFrame* f);

    GLuint m_textures[3] = {0, 0, 0};
    QOpenGLShaderProgram* m_program = nullptr;
    QOpenGLBuffer m_vbo;

    AVFrame* m_localFrame = nullptr;
    std::mutex m_frameMutex;
    PlayerStats* m_stats = nullptr;
    int64_t m_lastPaintUs = 0;

    int m_videoWidth  = 0;
    int m_videoHeight = 0;
    int m_texWidth    = 0;
    int m_texHeight   = 0;

    int m_uniformTexY = 0;
    int m_uniformTexU = 0;
    int m_uniformTexV = 0;
};

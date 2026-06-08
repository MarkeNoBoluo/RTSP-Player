#include "GLVideoWidget.h"
#include "PlayerStats.h"
#include <QDebug>
#include "logger/Logger.h"

extern "C" {
#include <libavutil/time.h>
}

static const float kVertices[] = {
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
};

GLVideoWidget::GLVideoWidget(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_vbo(QOpenGLBuffer::VertexBuffer)
{
    setUpdateBehavior(NoPartialUpdate);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
}

GLVideoWidget::~GLVideoWidget() {
    makeCurrent();
    if (m_localFrame) {
        av_frame_free(&m_localFrame);
    }
    if (m_program) {
        delete m_program;
        m_program = nullptr;
    }
    if (m_vbo.isCreated()) {
        m_vbo.destroy();
    }
    glDeleteTextures(3, m_textures);
    doneCurrent();
}

void GLVideoWidget::setDisplayFrame(const VideoFrame* frame) {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    av_frame_unref(m_localFrame);
    if (frame && frame->frame && frame->frame->data[0]) {
        av_frame_ref(m_localFrame, frame->frame);
    }
}

void GLVideoWidget::initializeGL() {
    initializeOpenGLFunctions();

    LOG_INFO("OpenGL initialized");

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    m_program = new QOpenGLShaderProgram(this);
    m_program->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/yuv420p.vert");
    m_program->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/yuv420p.frag");
    m_program->bindAttributeLocation("position", 0);
    m_program->bindAttributeLocation("texcoord", 1);
    if (!m_program->link()) {
        LOG_ERROR("Shader link failed: %s", m_program->log().toUtf8().constData());
    }

    m_uniformTexY = m_program->uniformLocation("tex_y");
    m_uniformTexU = m_program->uniformLocation("tex_u");
    m_uniformTexV = m_program->uniformLocation("tex_v");

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(kVertices, sizeof(kVertices));
    m_vbo.release();

    glGenTextures(3, m_textures);
    m_localFrame = av_frame_alloc();

    LOG_INFO("OpenGL setup complete, textures created");

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void GLVideoWidget::paintGL() {
    int64_t now = av_gettime_relative();

    // Record paint interval
    if (m_lastPaintUs > 0 && m_stats) {
        m_stats->recordPaintInterval(now - m_lastPaintUs);
    }
    m_lastPaintUs = now;

    // Record paint latency: time from commit to paint
    if (m_stats) {
        int64_t commitUs = m_stats->lastCommitUs.load();
        if (commitUs > 0) {
            m_stats->recordPaintLatency(now - commitUs);
        }
    }

    std::lock_guard<std::mutex> lock(m_frameMutex);

    auto* f = m_localFrame;
    if (!f || !f->data[0]) return;

    int w = f->width;
    int h = f->height;
    if (w <= 0 || h <= 0) return;

    if (w != m_videoWidth || h != m_videoHeight) {
        glClear(GL_COLOR_BUFFER_BIT);
        setupTextures(w, h);
        m_videoWidth  = w;
        m_videoHeight = h;
        LOG_DEBUG("Video resolution changed: %dx%d", w, h);
    }

    uploadTextures(f);

    m_program->bind();
    m_program->setUniformValue(m_uniformTexY, 0);
    m_program->setUniformValue(m_uniformTexU, 1);
    m_program->setUniformValue(m_uniformTexV, 2);

    m_vbo.bind();
    m_program->enableAttributeArray(0);
    m_program->enableAttributeArray(1);
    m_program->setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * sizeof(float));
    m_program->setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_program->disableAttributeArray(0);
    m_program->disableAttributeArray(1);
    m_vbo.release();

    m_program->release();
}

void GLVideoWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void GLVideoWidget::setupTextures(int width, int height) {
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width / 2, height / 2, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width / 2, height / 2, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_texWidth  = width;
    m_texHeight = height;
}

void GLVideoWidget::uploadTextures(AVFrame* f) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width, f->height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[0]);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width / 2, f->height / 2,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[1]);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width / 2, f->height / 2,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[2]);
}

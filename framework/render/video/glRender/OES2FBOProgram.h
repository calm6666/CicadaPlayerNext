//
// Created by SuperMan on 8/20/21.
//

#ifndef SOURCE_OES2FBOPROGRAM_H
#define SOURCE_OES2FBOPROGRAM_H


#include <GLES2/gl2.h>

class OES2FBOProgram {

public:
    OES2FBOProgram();

    ~OES2FBOProgram();

    int initProgram();

    bool updateFrameBuffer(int width, int height);

    GLuint getFrameBuffer();

    GLuint getFrameTexture();

    void useProgram();

    void enableDrawRegion(GLfloat drawRegion[12]);

    void enableFlipCoords(GLfloat flipCoords[8]);

    void disableDrawRegion();

    void disableFlipCoords();

    void uniform1i();

    /*
     * 【色觉辅助滤镜 / 回退点 S2】OES → FBO 这条中间路（开了视频后处理/超分时走）自己的
     * program 也要带颜色矩阵，否则"开了后处理 ⇒ 滤镜失效"。矩阵只由
     * OESProgramContext::updateColorMatrix 转发进来，最终上屏那一步用的还是 OESProgram
     * 自己那一份（两个 program 各自持有同一个矩阵值，不会双重叠加）。
     */
    void updateColorMatrix(const float matrix[9]);

private:
    void destroyFrameBuffer();

private:
    int mInitRet = 0;

    GLuint mDisProgram = {0};
    GLuint mDisVertShader{0};
    GLuint mDisFragmentShader{0};
    GLuint mDisPositionLocation{0};
    GLuint mDisTexCoordLocation{0};
    GLint mDisTextureLocation{0};
    /* 【色觉辅助滤镜 / 回退点 S2】 */
    GLint mDisColorMatrixLocation{0};
    GLfloat mDisColorMatrix[9] = {1.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f};
    GLuint *mFrameBuffers{nullptr};
    GLuint *mFrameBufferTextures{nullptr};

    int mFrameWidth{0};
    int mFrameHeight{0};
};


#endif//SOURCE_OES2FBOPROGRAM_H

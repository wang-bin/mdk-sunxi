/*
 * Copyright (c) 2018 WangBin <wbsecg1 at gmail.com>
 */
#include "mdk/VideoBuffer.h"
#include "mdk/VideoFrame.h"
#include "NativeVideoBufferTemplate.h"
#include "video/opengl/GLGlue.h"
#include "ugl/gl_api.h" // egl_api.h is included if HAVE_EGL_CAPI is defined
#include "ugl/context.h"
#include <iostream>
#include <memory>
#include <mutex>
extern "C" {
#include <libcedarv/libcedarv.h>
}
using namespace std;
using namespace UGL_NS::opengl;
#define FFALIGN(x, a) (((x)+(a)-1)&~((a)-1))

MDK_NS_BEGIN
class CedarVBufferPool final : public NativeVideoBufferPool {
public:
    NativeVideoBufferRef getBuffer(void* opaque, std::function<void()> cleanup = nullptr) override;
    bool transfer_begin(cedarv_picture_t* buf, NativeVideoBuffer::GLTextureArray* ma, NativeVideoBuffer::MapParameter *mp);
    void transfer_end() {}
    bool transfer_to_host(cedarv_picture_t* buf, NativeVideoBuffer::MemoryArray* ma, NativeVideoBuffer::MapParameter *mp);
private:
    Context* updateContext() {
        Context* c = Context::current();
        if (c == ctx_)
            return c;
        ctx_ = c;
        ctx_res_ = nullptr;
        return c;
    }

    bool ensureGL(const VideoFormat& fmt, int* w, int* h);
    struct ctx_res_t {
        GLuint tex[2] = {};
        GLenum format[4]{};
        GLenum data_type[4]{};
        int count = 2;
    };
    Context *ctx_ = nullptr; // used to check context change
    ctx_res_t* ctx_res_ = nullptr; // current cls value used

    std::mutex hos_mutex_;
    VideoFrame host_;

    Context::Local<ctx_res_t> res = {[](ctx_res_t& r){
        std::clog << "release CedarV-GL interop resources" << std::endl;
        for (auto& t : r.tex) {
            if (t)
                gl().DeleteTextures(t, &t);
        }
    }};
};

typedef shared_ptr<CedarVBufferPool> PoolRef;
using CedarVBuffer = NativeVideoBufferImpl<cedarv_picture_t*, CedarVBufferPool>;

NativeVideoBufferRef CedarVBufferPool::getBuffer(void* opaque, std::function<void()> cleanup)
{
    return std::make_shared<CedarVBuffer>(static_pointer_cast<CedarVBufferPool>(shared_from_this()), static_cast<cedarv_picture_t*>(opaque), cleanup);    
}


bool CedarVBufferPool::ensureGL(const VideoFormat& fmt, int* w, int* h)
{
    if (ctx_res_->tex[0])
        return true;
    ctx_res_->count = fmt.planeCount();
    GLint tex_ifmt[4]{};
    toGL(fmt, tex_ifmt, ctx_res_->format, ctx_res_->data_type);
    const auto& gl = UGL_NS::opengl::gl(); // *ctx_->gl()
    gl.GenTextures(ctx_res_->count, ctx_res_->tex);
    std::clog << "CedarV-GL interop: upload tiled image to gl texture directly" << std::endl;
    for (size_t i = 0; i < ctx_res_->count; ++i) {
        GLuint& t = ctx_res_->tex[i];
        gl.BindTexture(GL_TEXTURE_2D, t);
        if (w[i] <= 0)
            w[i] = fmt.width(w[0], i);
        if (h[i] <= 0)
            h[i] = fmt.height(h[0], i);
        gl.TexImage2D(GL_TEXTURE_2D, 0, tex_ifmt[i], w[i], h[i], 0, ctx_res_->format[i], ctx_res_->data_type[i], nullptr);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); //GL_NEAREST?
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.BindTexture(GL_TEXTURE_2D, 0);
    }
    return true;
}

bool CedarVBufferPool::transfer_begin(cedarv_picture_t* buf, NativeVideoBuffer::GLTextureArray* ma, NativeVideoBuffer::MapParameter *mp)
{
    if (ma->id[0] > 0)
        return true;
    if (!updateContext())
        return false;
    if (!ctx_res_) {
        ctx_res_ = &res.get(ctx_);
    }
    const VideoFormat fmt = PixelFormat::NV12;
    buf->display_height = FFALIGN(buf->display_height, 8);
    mp->width[0] = FFALIGN(buf->display_width, 16);
    mp->height[0] = FFALIGN(buf->display_height, 2); // already aligned to 8!
    if (!ensureGL(fmt, mp->width, mp->height))
        return false;
    const auto& gl = *ctx_->gl();
    const void* bits[] = {buf->y, buf->u};
    for (int i = 0; i < ctx_res_->count; ++i) {
        gl.BindTexture(GL_TEXTURE_2D, ctx_res_->tex[i]);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fmt.width(mp->width[0], i), fmt.height(mp->height[0], i), ctx_res_->format[i], ctx_res_->data_type[i], bits[i]);
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        ma->id[i] = ctx_res_->tex[i];
        //printf("ma->id[%d]: %d, %dx%d @%p\n", i, ma->id[i], mp->width[i], mp->height[i], bits[i]);
    }
    ma->target = GL_TEXTURE_2D;
    mp->format = fmt;
    return true;
}

// source data is colum major. every block is 32x32
static void map32x32_to_yuv_Y(void* srcY, void* tarY, unsigned int dst_pitch, unsigned int coded_width, unsigned int coded_height)
{
    unsigned long offset;
    unsigned char *ptr = (unsigned char *)srcY;
    const unsigned int mb_width = (coded_width+15) >> 4;
    const unsigned int mb_height = (coded_height+15) >> 4;
    const unsigned int twomb_line = (mb_height+1) >> 1;
    const unsigned int recon_width = (mb_width+1) & 0xfffffffe;

    for (unsigned int i = 0; i < twomb_line; i++) {
        const unsigned int M = 32*i;
        for (unsigned int j = 0; j < recon_width; j+=2) {
            const unsigned int n = j*16;
            offset = M*dst_pitch + n;
            for (unsigned int l = 0; l < 32; l++) {
                if (M+l < coded_height) {
                    if (n+16 < coded_width) {
                        //1st & 2nd mb
                        memcpy((unsigned char *)tarY+offset, ptr, 32);
                    } else if (n<coded_width) {
                        // 1st mb
                        memcpy((unsigned char *)tarY+offset, ptr, 16);
                    }
                    offset += dst_pitch;
                }
                ptr += 32;
            }
        }
    }
}

static void map32x32_to_yuv_C(void* srcC, void* tarCb, void* tarCr, unsigned int dst_pitch, unsigned int coded_width, unsigned int coded_height)
{
    coded_width /= 2; // libvdpau-sunxi compatible
    unsigned char line[32];
    unsigned long offset;
    unsigned char *ptr = (unsigned char *)srcC;
    const unsigned int mb_width = (coded_width+7) >> 3;
    const unsigned int mb_height = (coded_height+7) >> 3;
    const unsigned int fourmb_line = (mb_height+3) >> 2;
    const unsigned int recon_width = (mb_width+1) & 0xfffffffe;

    for (unsigned int i = 0; i < fourmb_line; i++) {
        const int M = i*32;
        for (unsigned int j = 0; j < recon_width; j+=2) {
            const unsigned int n = j*8;
            offset = M*dst_pitch + n;
            for (unsigned int l = 0; l < 32; l++) {
                if (M+l < coded_height) {
                    if (n+8 < coded_width) {
                        // 1st & 2nd mb
                        memcpy(line, ptr, 32);
                        //unsigned char *line = ptr;
                        for (int k = 0; k < 16; k++) {
                            *((unsigned char *)tarCb + offset + k) = line[2*k];
                            *((unsigned char *)tarCr + offset + k) = line[2*k+1];
                        }
                    } else if (n < coded_width) {
                        // 1st mb
                        memcpy(line, ptr, 16);
                        //unsigned char *line = ptr;
                        for (int k = 0; k < 8; k++) {
                            *((unsigned char *)tarCb + offset + k) = line[2*k];
                            *((unsigned char *)tarCr + offset + k) = line[2*k+1];
                        }
                    }
                    offset += dst_pitch;
                }
                ptr += 32;
            }
        }
    }
}

typedef void (*map_y_t)(void* src, void* dst, unsigned int dst_pitch, unsigned int w, unsigned int h);
map_y_t map_y_ = map32x32_to_yuv_Y;
typedef void (*map_c_t)(void* src, void* dst1, void* dst2, unsigned int dst_pitch, unsigned int w, unsigned int h);
map_c_t map_c_ = map32x32_to_yuv_C;

bool CedarVBufferPool::transfer_to_host(cedarv_picture_t* buf, NativeVideoBuffer::MemoryArray* ma, NativeVideoBuffer::MapParameter *mp)
{
    if (ma->data[0]) // can be reused
        return true;
    buf->display_height = FFALIGN(buf->display_height, 8);
    const int display_h_align = FFALIGN(buf->display_height, 2); // already aligned to 8!
    const int display_w_align = FFALIGN(buf->display_width, 16);
    const int dst_y_stride = display_w_align;
    //const int dst_c_stride = FFALIGN(buf->display_width/2, 16);
    mp->stride[0] = dst_y_stride;
    mp->stride[1] = dst_y_stride; // uv plane
    std::lock_guard<std::mutex> lock(hos_mutex_);
    if (host_.width() != display_w_align || host_.height() != display_h_align)
        host_ = VideoFrame(display_w_align, display_h_align, PixelFormat::NV12, mp->stride);
    ma->data[0] = host_.buffer(0)->data();
    ma->data[1] = host_.buffer(1)->data();
    mp->format = PixelFormat::NV12;
    if (map_y_) {
        map_y_(buf->y, (void*)ma->data[0], display_w_align, mp->stride[0], display_h_align);
        map_y_(buf->u, (void*)ma->data[1], mp->stride[1], display_w_align, display_h_align/2);
    } else {
        memcpy((void*)ma->data[0], buf->y, mp->stride[0]*display_h_align);
        memcpy((void*)ma->data[1], buf->u, mp->stride[1]*display_h_align/2);
    }
    return true;
}

void register_native_buffer_pool_cedarv() {
    NativeVideoBufferPool::registerOnce("CedarV", []{
        return std::make_shared<CedarVBufferPool>();
    });
}
namespace { // DCE
static const struct register_at_load_time_if_no_dce {
    inline register_at_load_time_if_no_dce() { register_native_buffer_pool_cedarv();}
} s;
}
MDK_NS_END

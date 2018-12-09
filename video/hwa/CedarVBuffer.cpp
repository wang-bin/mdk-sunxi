/*
 * Copyright (c) 2018 WangBin <wbsecg1 at gmail.com>
 */
// env: EGLIMAGE_UMP=0/1/2(0: no ump, 1: input is host output is ump, 2:input is ump). GL_TILE=0/1, SIMD_TILE=0/1
// EGLIMAGE_MEM=1 if EGLIMAGE_UMP==0: use host memory as fbdev_pixmap
#include "mdk/VideoBuffer.h"
#include "mdk/VideoFrame.h"
#include "NativeVideoBufferTemplate.h"
#include "video/opengl/GLGlue.h"
#include "ugl/gl_api.h" // egl_api.h is included if HAVE_EGL_CAPI is defined
#include "ugl/egl_api.h"
#include "ugl/context.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
extern "C" {
#include <libcedarv/libcedarv.h> // TODO: remove
#include <ump/ump.h>
#include <ump/ump_ref_drv.h>
#include <EGL/fbdev_window.h>

#include "sunxi_disp_ioctl.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
}
using namespace std;
using namespace UGL_NS::opengl;
#define FFALIGN(x, a) (((x)+(a)-1)&~((a)-1))

MDK_NS_BEGIN
PFNEGLCREATEIMAGEKHRPROC eglCreateImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImage = nullptr;
// TODO: move tile->linear code to an individual file.
// TODO: rename to UMPBuffer which can be used for other platforms
// TODO: no libcedarv.h dependency, use generic struct contains ptrs, and is_ump flag. if picture is ump, no copy
// TODO: x11 mali egl does not support fbdev_pixmap. try x11 pixmap using xputimage to update pixmap
static void map32x32_to_yuv_Y(const void* srcY, void* tarY, unsigned int dst_pitch, unsigned int coded_width, unsigned int coded_height);
static void map32x32_to_yuv_C(const void* srcC, void* tarCb, void* tarCr, unsigned int dst_pitch, unsigned int coded_width, unsigned int coded_height);
extern "C" {
void neon_tiled_to_planar(const void *src, void *dst, unsigned int dst_pitch, unsigned int width, unsigned int height);
void neon_tiled_deinterleave_to_planar(const void *src, void *dst1, void *dst2, unsigned int dst_pitch, unsigned int width, unsigned int height);
}

typedef void (*map_y_t)(const void* src, void* dst, unsigned int dst_pitch, unsigned int w, unsigned int h);
map_y_t map_y_ = map32x32_to_yuv_Y;
typedef void (*map_c_t)(const void* src, void* dst1, void* dst2, unsigned int dst_pitch, unsigned int w, unsigned int h);
map_c_t map_c_ = map32x32_to_yuv_C;

class CedarVBufferPool final : public NativeVideoBufferPool {
public:
    CedarVBufferPool() {
        ump_open();
        const char* env = getenv("EGLIMAGE_UMP");
        if (env)
            gl_ump_ = atoi(env);
        env = getenv("EGLIMAGE_MEM");
        if (env)
            egl_mem_ = atoi(env);
        env = getenv("GL_TILE");
        gl_tile_ = env && atoi(env); // default is true if tile to linear is supported by shader
        env = getenv("SIMD_TILE");
        if (!env || atoi(env)) {
            map_y_ = neon_tiled_to_planar;
            map_c_ = neon_tiled_deinterleave_to_planar;
        } else {
            map_y_ = map32x32_to_yuv_Y;
            map_c_ = map32x32_to_yuv_C;   
        }
        env = getenv("DISP_TILE");
        if (env && atoi(env))
            disp_fd_ = ::open("/dev/disp", O_RDWR);
        if (disp_fd_ >= 0) {
            unsigned long screen = 0;
            int scaler = ioctl(disp_fd_, DISP_CMD_SCALER_REQUEST, &screen);
            std::clog << "scaler from display engine in screen " << screen << ": " << scaler << std::endl;
            if (scaler == -1) {
                std::clog << "failed to request scaler from display engine " << disp_fd_ << std::endl;
                ::close(disp_fd_);
                disp_fd_ = -1;
            } else {
                unsigned long arg[] = {screen, (unsigned long)scaler};
                ioctl(disp_fd_, DISP_CMD_SCALER_RELEASE, arg);
                std::clog << "using scaler from display engine " << disp_fd_ << std::endl;
            }
        }
    }
    ~CedarVBufferPool() override {
        if (disp_fd_ >= 0)
            ::close(disp_fd_);
        ump_close();
    }

    NativeVideoBufferRef getBuffer(void* opaque, std::function<void()> cleanup = nullptr) override;
    bool transfer_begin(cedarv_picture_t* buf, NativeVideoBuffer::GLTextureArray* ma, NativeVideoBuffer::MapParameter *mp);
    void transfer_end();
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
        int count = 2;
        GLuint tex[2] = {};
        GLenum format[4]{};
        GLenum data_type[4]{};
        fbdev_pixmap pixmap[2]{};
        ump_handle ump[2]{};
        EGLImageKHR img[2]{};
    };
    Context *ctx_ = nullptr; // used to check context change
    ctx_res_t* ctx_res_ = nullptr; // current cls value used

    bool gl_tile_ = false;
    bool egl_mem_ = false;
    int gl_ump_ = 1; // better performance. on sun4i 1080p bbb cpu load is about 50%, while host memory cpu is ~95%
    int disp_fd_ = -1;
    std::mutex hos_mutex_;
    VideoFrame host_;

    Context::Local<ctx_res_t> res = {[](ctx_res_t& r){
        std::clog << "release CedarV-GL interop resources" << std::endl;
        for (auto& t : r.tex) {
            if (t)
                gl().DeleteTextures(t, &t);
        }
        for (auto& img : r.img) {
            if (img)
                EGL_WARN(eglDestroyImage(eglGetCurrentDisplay(), img));
        }
        for (auto ump : r.ump) {
            if (ump)
                ump_reference_release(ump);
        }
    }};
};

typedef shared_ptr<CedarVBufferPool> PoolRef;
using CedarVBuffer = NativeVideoBufferImpl<cedarv_picture_t*, CedarVBufferPool>;

NativeVideoBufferRef CedarVBufferPool::getBuffer(void* opaque, std::function<void()> cleanup)
{
    return std::make_shared<CedarVBuffer>(static_pointer_cast<CedarVBufferPool>(shared_from_this()), static_cast<cedarv_picture_t*>(opaque), cleanup);    
}

static bool disp_tiled_to_linear(int fd, int width, int height, const void* y, const void* uv, void* dst)
{
    unsigned long arg[4]{}; 
    // arg[0]: screen 0/1, https://linux-sunxi.org/Sunxi_disp_driver_interface/IOCTL#Scaler_IOCTLs
    int scaler = ioctl(fd, DISP_CMD_SCALER_REQUEST, (unsigned long)arg);
    if (scaler == -1) {
        std::clog << "failed to request scaler from display engine " << fd << std::endl;
        return false;
    }
    arg[1] = scaler;
    __disp_scaler_para_t p{};
    p.input_fb.addr[0] = (__u32)y;
    p.input_fb.addr[1] = (__u32)uv;
    p.input_fb.size.width = width;
    p.input_fb.size.height = height;
    p.input_fb.format = DISP_FORMAT_YUV420;
    p.input_fb.seq = DISP_SEQ_UVUV;
    p.input_fb.mode = DISP_MOD_MB_UV_COMBINED;
    p.input_fb.br_swap = 0;
    p.input_fb.cs_mode = DISP_BT601; // TODO 709?
    p.source_regn.x = 0;
    p.source_regn.y = 0;
    p.source_regn.width = width;
    p.source_regn.height = height;
    p.output_fb.addr[0] = (__u32)dst;
    p.output_fb.size.width = width;
    p.output_fb.size.height = height;
    // https://github.com/linux-sunxi/linux-sunxi/blob/sunxi-3.4/drivers/video/sunxi/disp/disp_scaler.c#L893
    // mode+format: DISP_MOD_NON_MB_PLANAR + DISP_FB_TYPE_YUV
    // mode+format: DISP_MOD_INTERLEAVED+DISP_FORMAT_ARGB8888, or (planar rgb?)DISP_MOD_NON_MB_PLANAR + DISP_FORMAT_RGB888|DISP_FORMAT_ARGB8888
    p.output_fb.format = DISP_FORMAT_ARGB8888;//DISP_FORMAT_YUV420;
    p.output_fb.seq = DISP_SEQ_BGRA;//DISP_SEQ_P3210/*yuv420 p*/;//DISP_SEQ_UVUV;
    p.output_fb.mode = DISP_MOD_INTERLEAVED;//DISP_MOD_NON_MB_PLANAR; // DISP_MOD_NON_MB_UV_COMBINED: invalid in Display_Scaler_Start. https://github.com/linux-sunxi/linux-sunxi/blob/sunxi-3.4/drivers/video/sunxi/disp/disp_scaler.c#L894
    p.output_fb.br_swap = 0;
    p.output_fb.cs_mode = DISP_BT601;
    arg[2] = (unsigned long)&p;
    int ret = ioctl(fd, DISP_CMD_SCALER_EXECUTE, (unsigned long)arg);
    if (ret < 0) // need to release scaler too
        std::clog << " failed to scale video by display engine, errno " << errno << std::endl;
    if (ioctl(fd, DISP_CMD_SCALER_RELEASE, (unsigned long)arg) != 0) {
        std::clog << errno << " failed to release scaler from display engine " << fd << std::endl;
        return false;
    }
    return ret >= 0;
}

static void fill_pixmap(fbdev_pixmap *pm, const void* mem, ump_handle umph, int width, int height, int bpp)
{
    memset(pm, 0, sizeof(*pm));
    pm->bytes_per_pixel = bpp/8;
    pm->buffer_size = bpp;
    if (bpp >= 24) { // rgb
        pm->red_size = 8;
        pm->green_size = 8;
        pm->blue_size = 8;
        pm->alpha_size = bpp - 24;
    } else {
        pm->luminance_size = 8;
    }
    pm->alpha_size = bpp - pm->red_size - pm->green_size - pm->blue_size - pm->luminance_size; // GL_LUMINANCE_ALPHA
    pm->width = width;
    pm->height = height;
    // FBDEV_PIXMAP_DEFAULT is slower then UMP, but faster than normal texture uploading
    pm->flags = umph ? FBDEV_PIXMAP_SUPPORTS_UMP : FBDEV_PIXMAP_DEFAULT;
// https://github.com/agx/gst-plugins-bad/blob/master/ext/eglgles/video_platform_wrapper.c#L499-L569
// https://github.com/peak3d/egltest
    //pm->data = (decltype(pm->data))cedarv_virt2phys(mem); // cedarv_virt2phys()?
    pm->data = (short unsigned int*)(umph ? umph : mem); // pixmapHandle =  ump_ref_drv_allocate( sizeOfBuffer  , UMP_REF_DRV_CONSTRAINT_NONE);
    pm->format = 0;
    if (umph)
        ump_reference_add(umph);
    //ump_cpu_msync_now(umph, UMP_MSYNC_CLEAN_AND_INVALIDATE, 0, ump_size_get(umph));
    //printf("fbdev pixmap bpp=%d rgba=(%d,%d,%d,%d), l: %d, ", pm->bytes_per_pixel, pm->red_size, pm->green_size, pm->blue_size, pm->alpha_size, pm->luminance_size);
}

bool CedarVBufferPool::ensureGL(const VideoFormat& fmt, int* w, int* h)
{
    if (ctx_res_->tex[0])
        return true;
    if (!eglCreateImage) {
        eglCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    }

    ctx_res_->count = fmt.planeCount();
    GLint tex_ifmt[4]{};
    toGL(fmt, tex_ifmt, ctx_res_->format, ctx_res_->data_type);
    const auto& gl = UGL_NS::opengl::gl(); // *ctx_->gl()
    gl.GenTextures(ctx_res_->count, ctx_res_->tex);
    std::clog << "CedarV-GL interop" << std::endl;
    for (size_t i = 0; i < ctx_res_->count; ++i) {
        GLuint& t = ctx_res_->tex[i];
        gl.BindTexture(GL_TEXTURE_2D, t);
        gl.TexImage2D(GL_TEXTURE_2D, 0, tex_ifmt[i], w[i], h[i], 0, ctx_res_->format[i], ctx_res_->data_type[i], nullptr);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); //GL_NEAREST?
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (gl_ump_ == 1) {
            ctx_res_->ump[i] = ump_ref_drv_allocate(fmt.bytesForPlane(w[0], h[0], i), ump_alloc_constraints(UMP_REF_DRV_CONSTRAINT_PHYSICALLY_LINEAR|UMP_REF_DRV_CONSTRAINT_USE_CACHE)); //UMP_REF_DRV_CONSTRAINT_USE_CACHE
            fill_pixmap(&ctx_res_->pixmap[i], nullptr, ctx_res_->ump[i], w[i], h[i], fmt.bitsPerPixel(i));
            const EGLint imgattr[] = {
                EGL_IMAGE_PRESERVED_KHR, EGL_FALSE, 
                EGL_NONE
            };
            EGL_WARN(ctx_res_->img[i] = eglCreateImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)&ctx_res_->pixmap[i], imgattr));
            gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, ctx_res_->img[i]);
            std::clog << "CedarV-UMP-EGLImage interop. plane " << i << " ump: " << ctx_res_->ump[i] << " +" << ump_size_get(ctx_res_->ump[i]) << ", eglimage: " << ctx_res_->img[i] << std::endl;
        }
        gl.BindTexture(GL_TEXTURE_2D, 0);
    }
    if (gl_ump_)
        std::clog << "CedarV-EGLImage from UMP interop" << std::endl;
    if (gl_ump_ == 0 && egl_mem_)
        std::clog << "CedarV-EGLImage from host memory interop" << std::endl;
    return true;
}

bool CedarVBufferPool::transfer_begin(cedarv_picture_t* buf, NativeVideoBuffer::GLTextureArray* ma, NativeVideoBuffer::MapParameter *mp)
{
    if (!updateContext())
        return false;
    if (ma->id[0] > 0 && ma->test_set_glctx(ctx_->id()))
        return true;
    if (!ctx_res_) {
        ctx_res_ = &res.get(ctx_);
    }
    const VideoFormat fmt = disp_fd_ >= 0 ? PixelFormat::RGBA : PixelFormat::NV12T32x32;
    buf->display_height = FFALIGN(buf->display_height, 8);
    mp->width[0] = FFALIGN(buf->display_width, 16);
    mp->height[0] = FFALIGN(buf->display_height, 2); // already aligned to 8!
    mp->width[1] = fmt.width(mp->width[0], 1);
    mp->height[1] = fmt.height(mp->height[0], 1);
    mp->stride[0] = mp->width[0];
    mp->stride[1] = mp->width[0]; // uv plane
    if (!ensureGL(fmt, mp->width, mp->height))
        return false;
    ma->target = GL_TEXTURE_2D;
    mp->format = fmt;
    const auto& gl = *ctx_->gl();
    const void* bits[] = {buf->y, buf->u};
    if (disp_fd_ >= 0) {
        void* dst = (void*)ump_mapped_pointer_get(ctx_res_->ump[0]);
        const bool ret = disp_tiled_to_linear(disp_fd_, mp->width[0], mp->height[0], bits[0], bits[1], dst);
        ump_mapped_pointer_release(ctx_res_->ump[0]);
        if (ret) {
            ma->id[0] = ctx_res_->tex[0];
            ma->id[1] = ctx_res_->tex[1];
            return true;
        } else {
            std::clog << "Failed to convert tiled to linear nv12 by disp device. Fallback to cpu or gl shader conversion" << std::endl;
            ::close(disp_fd_);
            disp_fd_ = -1;
        }
    }

    for (int i = 0; i < ctx_res_->count; ++i) {
        if (gl_ump_ == 1) {
            //ump_switch_hw_usage(ctx_res_->ump[i], UMP_USED_BY_CPU);
            //ump_lock(ctx_res_->ump[i], UMP_READ_WRITE);
            if (gl_tile_) {
                ump_write(ctx_res_->ump[i], 0, bits[i], fmt.bytesForPlane(mp->width[0], mp->height[0], i));
            } else {
                map_y_(bits[i], (void*)ump_mapped_pointer_get(ctx_res_->ump[i]), mp->stride[i], mp->width[0] /* because use map_y_*/, mp->height[i]);
                ump_mapped_pointer_release(ctx_res_->ump[i]);
            }
//            ump_unlock(ctx_res_->ump[i]);
            //ump_switch_hw_usage(ctx_res_->ump[i], UMP_USED_BY_MALI);
        } else {
            gl.BindTexture(GL_TEXTURE_2D, ctx_res_->tex[i]);
            if (gl_ump_ > 1 || egl_mem_) {
                if (gl_ump_ > 1) {
                    ctx_res_->ump[i] = (ump_handle)buf->ump[i];
                    ump_reference_add(ctx_res_->ump[i]);
                }
                fill_pixmap(&ctx_res_->pixmap[i], bits[i], ctx_res_->ump[i], mp->width[i],  mp->height[i], fmt.bitsPerPixel(i));
                const EGLint imgattr[] = {
                    EGL_IMAGE_PRESERVED_KHR, EGL_FALSE, 
                    EGL_NONE
                };
                EGL_WARN(ctx_res_->img[i] = eglCreateImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)&ctx_res_->pixmap[i], imgattr));
                gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, ctx_res_->img[i]);
            } else {
                gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mp->width[i], mp->height[i], ctx_res_->format[i], ctx_res_->data_type[i], bits[i]);
            }
            gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        ma->id[i] = ctx_res_->tex[i];
        //printf("ma->id[%d]: %d, %dx%d @%p\n", i, ma->id[i], mp->width[i], mp->height[i], bits[i]);
    }
    return true;
}

void CedarVBufferPool::transfer_end()
{
    if (gl_ump_ < 2 && (gl_ump_ != 0 || !egl_mem_))
        return;
    for (auto& img : ctx_res_->img) {
        if (img != EGL_NO_IMAGE_KHR)
            EGL_WARN(eglDestroyImage(eglGetCurrentDisplay(), img));
        img = EGL_NO_IMAGE_KHR;
    }
    for (auto ump : ctx_res_->ump) {
        if (ump)
            ump_reference_release(ump);
        ump = nullptr;
    }
}
// source data is colum major. every block is 32x32
void map32x32_to_yuv_Y(const void* srcY, void* tarY, unsigned int dst_pitch, unsigned int coded_width, unsigned int coded_height)
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

void map32x32_to_yuv_C(const void* srcC, void* tarCb, void* tarCr, unsigned int dst_pitch, unsigned int coded_width, unsigned int coded_height)
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

bool CedarVBufferPool::transfer_to_host(cedarv_picture_t* buf, NativeVideoBuffer::MemoryArray* ma, NativeVideoBuffer::MapParameter *mp)
{
    if (ma->data[0]) // can be reused
        return true;
    buf->display_height = FFALIGN(buf->display_height, 8);
    const int w = FFALIGN(buf->display_width, 16);
    const int h = FFALIGN(buf->display_height, 2); // already aligned to 8!
    const int dst_y_stride = w;
    //const int dst_c_stride = FFALIGN(buf->display_width/2, 16);
    mp->stride[0] = dst_y_stride;
    mp->stride[1] = dst_y_stride; // uv plane
    std::lock_guard<std::mutex> lock(hos_mutex_);
    const VideoFormat fmt = PixelFormat::NV12;
    mp->format = fmt;
    if (host_.width() != w || host_.height() != h)
        host_ = VideoFrame(w, h, fmt, mp->stride);
    const void* bits[] = {buf->y, buf->u};
    for (int i = 0; i < fmt.planeCount(); ++i) {
        ma->data[i] = host_.buffer(i)->data();
        if (gl_tile_)
            memcpy(ma->data[i], bits[i], fmt.bytesForPlane(w, h, i));
        else
            map_y_(bits[i], ma->data[i], mp->stride[i], fmt.bytesPerLine(w, i)/*because use map_y_*/, fmt.height(h, i));
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

/*
 * Copyright (c) 2018 WangBin <wbsecg1 at gmail.com>
 */
#include "mdk/VideoBuffer.h"
#include "mdk/VideoFrame.h"
#include "NativeVideoBufferTemplate.h"
#include <iostream>
#include <memory>
#include <mutex>
extern "C" {
#include <libcedarv/libcedarv.h>
}
using namespace std;
#define FFALIGN(x, a) (((x)+(a)-1)&~((a)-1))

MDK_NS_BEGIN
class CedarVBufferPool final : public NativeVideoBufferPool {
public:
    NativeVideoBufferRef getBuffer(void* opaque, std::function<void()> cleanup = nullptr) override;
    bool transfer_to_host(cedarv_picture_t* buf, NativeVideoBuffer::MemoryArray* ma, NativeVideoBuffer::MapParameter *mp);
    bool transfer_begin(cedarv_picture_t* buf, NativeVideoBuffer::GLTextureArray* ma, NativeVideoBuffer::MapParameter *mp) {return false;}
    void transfer_end() {}
private:
    std::mutex hos_mutex_;
    VideoFrame host_;
};

typedef shared_ptr<CedarVBufferPool> PoolRef;
using CedarVBuffer = NativeVideoBufferImpl<cedarv_picture_t*, CedarVBufferPool>;

NativeVideoBufferRef CedarVBufferPool::getBuffer(void* opaque, std::function<void()> cleanup)
{
    return std::make_shared<CedarVBuffer>(static_pointer_cast<CedarVBufferPool>(shared_from_this()), static_cast<cedarv_picture_t*>(opaque), cleanup);    
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
    // TODO: upload as texture directly and convert tile in shader
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

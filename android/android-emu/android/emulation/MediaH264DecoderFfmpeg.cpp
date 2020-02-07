// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/emulation/MediaH264DecoderFfmpeg.h"
#include "android/base/system/System.h"
#include "android/emulation/H264NaluParser.h"
#include "android/emulation/YuvConverter.h"

#include <cstdint>
#include <string>
#include <vector>

#include <stdio.h>
#include <string.h>

#define MEDIA_H264_DEBUG 0

#if MEDIA_H264_DEBUG
#define H264_DPRINT(fmt,...) fprintf(stderr, "h264-ffmpeg-dec: %s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);
#else
#define H264_DPRINT(fmt,...)
#endif

namespace android {
namespace emulation {

MediaH264DecoderFfmpeg::~MediaH264DecoderFfmpeg() {
    H264_DPRINT("destroyed MediaH264DecoderFfmpeg %p", this);
    destroyH264Context();
}

void MediaH264DecoderFfmpeg::reset(unsigned int width,
                                                  unsigned int height,
                                                  unsigned int outWidth,
                                                  unsigned int outHeight,
                                                  PixelFormat outPixFmt) {
    H264_DPRINT("TODO: %s %d %p", __func__, __LINE__, this);
}

void MediaH264DecoderFfmpeg::initH264Context(unsigned int width,
                                                  unsigned int height,
                                                  unsigned int outWidth,
                                                  unsigned int outHeight,
                                                  PixelFormat outPixFmt) {
    H264_DPRINT("%s(w=%u h=%u out_w=%u out_h=%u pixfmt=%u)",
                __func__, width, height, outWidth, outHeight, (uint8_t)outPixFmt);
    mOutputWidth = outWidth;
    mOutputHeight = outHeight;
    mOutPixFmt = outPixFmt;
    mOutBufferSize = outWidth * outHeight * 3 / 2;

    mIsInFlush = false;

    if (mDecodedFrame) {
      delete [] mDecodedFrame;
    }

    mDecodedFrame = new uint8_t[mOutBufferSize];

    // standard ffmpeg codec stuff
    avcodec_register_all();
    if(0){
        AVCodec* current_codec = NULL;

        current_codec = av_codec_next(current_codec);
        while (current_codec != NULL)
        {
            if (av_codec_is_decoder(current_codec))
            {
                H264_DPRINT("codec decoder found %s long name %s", current_codec->name, current_codec->long_name);
            }
            current_codec = av_codec_next(current_codec);
        }
    }

    mCodec = NULL;
    auto useCuvidEnv = android::base::System::getEnvironmentVariable(
            "ANDROID_EMU_CODEC_USE_FFMPEG_CUVID_DECODER");
    if (useCuvidEnv != "") {
        mCodec = avcodec_find_decoder_by_name("h264_cuvid");
        if (mCodec) {
            mIsSoftwareDecoder = false;
            H264_DPRINT("Found h264_cuvid decoder, using it");
        } else {
            H264_DPRINT("Cannot find h264_cuvid decoder");
        }
    }
    if (!mCodec) {
        mCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
        H264_DPRINT("Using default software h264 decoder");
    }
    mCodecCtx = avcodec_alloc_context3(mCodec);

    avcodec_open2(mCodecCtx, mCodec, 0);
    mFrame = av_frame_alloc();

    H264_DPRINT("Successfully created software h264 decoder context %p", mCodecCtx);
}

MediaH264DecoderPlugin* MediaH264DecoderFfmpeg::clone() {
    H264_DPRINT("clone MediaH264DecoderFfmpeg %p with version %d", this,
                (int)mVersion);
    return new MediaH264DecoderFfmpeg(mVersion);
}

MediaH264DecoderFfmpeg::MediaH264DecoderFfmpeg(uint32_t version)
    : mVersion(version) {
    H264_DPRINT("allocated MediaH264DecoderFfmpeg %p with version %d", this,
                (int)mVersion);
}
void MediaH264DecoderFfmpeg::destroyH264Context() {
    H264_DPRINT("Destroy context %p", this);
    if (mCodecCtx) {
        avcodec_close(mCodecCtx);
        av_free(mCodecCtx);
        mCodecCtx = NULL;
    }
    if (mFrame) {
        av_frame_free(&mFrame);
        mFrame = NULL;
    }
    if (mDecodedFrame) {
      delete [] mDecodedFrame;
      mDecodedFrame = nullptr;
    }
}

static void* getReturnAddress(void* ptr) {
    uint8_t* xptr = (uint8_t*)ptr;
    void* pint = (void*)(xptr + 256);
    return pint;
}

void MediaH264DecoderFfmpeg::resetDecoder() {
    mNumDecodedFrame = 0;
    avcodec_close(mCodecCtx);
    av_free(mCodecCtx);
    mCodecCtx = avcodec_alloc_context3(mCodec);
    avcodec_open2(mCodecCtx, mCodec, 0);
}

bool MediaH264DecoderFfmpeg::checkWhetherConfigChanged(const uint8_t* frame, size_t szBytes) {
    // get frame type
    // if the frame is none SPS/PPS, return false
    // otherwise, check both SPS/PPS and return true
    const uint8_t* currNalu = H264NaluParser::getNextStartCodeHeader(frame, szBytes);
    if (currNalu == nullptr) {
        // should not happen
        H264_DPRINT("Found bad frame");
        return false;
    }

    size_t remaining = szBytes - (currNalu - frame);
    size_t currNaluSize = remaining;
    H264NaluParser::H264NaluType currNaluType = H264NaluParser::getFrameNaluType(currNalu, currNaluSize, NULL);
    if (currNaluType != H264NaluParser::H264NaluType::SPS) {
        return false;
    }

    H264_DPRINT("found SPS\n");

    const uint8_t* nextNalu = H264NaluParser::getNextStartCodeHeader(currNalu + 3, remaining - 3);

    if (nextNalu == nullptr) {
        // only one nalu, cannot have configuration change
        H264_DPRINT("frame has only one Nalu unit, cannot be configuration change\n");
        return false;
    }

    if (mNumDecodedFrame == 0) {
        H264_DPRINT("have not decoded anything yet, cannot be config change");
        return false;
    }
    // pretty sure it is config change
    H264_DPRINT("\n\nDetected stream configuration change !!!\n\n");
    return true;
}

void MediaH264DecoderFfmpeg::decodeFrame(void* ptr,
                                              const uint8_t* frame,
                                              size_t szBytes,
                                              uint64_t inputPts) {
    H264_DPRINT("%s(frame=%p, sz=%zu)", __func__, frame, szBytes);
    Err h264Err = Err::NoErr;
    // TODO: move this somewhere else
    // First return parameter is the number of bytes processed,
    // Second return parameter is the error code
    uint8_t* retptr = (uint8_t*)getReturnAddress(ptr);
    uint64_t* retSzBytes = (uint64_t*)retptr;
    int32_t* retErr = (int32_t*)(retptr + 8);

    if (!mIsSoftwareDecoder) {
        bool configChanged = checkWhetherConfigChanged(frame, szBytes);
        if (configChanged) {
            resetDecoder();
        }
    }
    av_init_packet(&mPacket);
    mPacket.data = (unsigned char*)frame;
    mPacket.size = szBytes;
    mPacket.pts = inputPts;
    avcodec_send_packet(mCodecCtx, &mPacket);
    int retframe = avcodec_receive_frame(mCodecCtx, mFrame);
    *retSzBytes = szBytes;
    *retErr = (int32_t)h264Err;
    mIsInFlush = false;
    if (retframe != 0) {
        H264_DPRINT("decodeFrame has nonzero return value %d", retframe);
        if (retframe == AVERROR_EOF) {
            H264_DPRINT("EOF returned from decoder");
            H264_DPRINT("EOF returned from decoder reset context now");
            resetDecoder();
        } else if (retframe == AVERROR(EAGAIN)) {
            H264_DPRINT("EAGAIN returned from decoder");
        } else {
            H264_DPRINT("unknown value %d", retframe);
        }
        return;
    }
    H264_DPRINT("new w %d new h %d, old w %d old h %d",
                mFrame->width, mFrame->height,
                mOutputWidth, mOutputHeight);
    mFrameFormatChanged = false;
    if(mIsSoftwareDecoder) {
        if (mFrame->width != mOutputWidth || mFrame->height != mOutputHeight) {
        mOutputHeight = mFrame->height;
        mOutputWidth = mFrame->width;
        mFrameFormatChanged = true;
        H264_DPRINT("%s: does not got frame in decode mode, format changed", __func__);
        *retErr = static_cast<int>(Err::DecoderRestarted);
        return;
    }
    }
    ++mNumDecodedFrame;
    copyFrame();
    mOutputPts = mFrame->pts;
    H264_DPRINT("%s: got frame in decode mode", __func__);
    mImageReady = true;
}

void MediaH264DecoderFfmpeg::copyFrame() {
    int w = mFrame->width;
    int h = mFrame->height;
    if (w != mOutputWidth || h != mOutputHeight) {
        mOutputWidth = w;
        mOutputHeight= h;
        delete [] mDecodedFrame;
        mOutBufferSize = mOutputWidth * mOutputHeight * 3 / 2;
        mDecodedFrame = new uint8_t[mOutBufferSize];
    }
    H264_DPRINT("w %d h %d Y line size %d U line size %d V line size %d", w, h,
            mFrame->linesize[0], mFrame->linesize[1], mFrame->linesize[2]);
    for (int i = 0; i < h; ++i) {
      memcpy(mDecodedFrame + i * w, mFrame->data[0] + i * mFrame->linesize[0], w);
    }
    H264_DPRINT("format is %d and NV21 is %d  12 is %d", mFrame->format, (int)AV_PIX_FMT_NV21,
            (int)AV_PIX_FMT_NV12);
    if (mFrame->format == AV_PIX_FMT_NV12) {
        for (int i=0; i < h / 2; ++i) {
            memcpy(w * h + mDecodedFrame + i * w, mFrame->data[1] + i * mFrame->linesize[1], w);
        }
        YuvConverter<uint8_t> convert8(mOutputWidth, mOutputHeight);
        convert8.UVInterleavedToPlanar(mDecodedFrame);
    } else {
        for (int i=0; i < h / 2; ++i) {
            memcpy(w * h + mDecodedFrame + i * w/2, mFrame->data[1] + i * mFrame->linesize[1], w / 2);
        }
        for (int i=0; i < h / 2; ++i) {
            memcpy(w * h + w * h / 4 + mDecodedFrame + i * w/2, mFrame->data[2] + i * mFrame->linesize[2], w / 2);
        }
    }
    mColorPrimaries = mFrame->color_primaries;
    mColorRange = mFrame->color_range;
    mColorTransfer = mFrame->color_trc;
    mColorSpace = mFrame->colorspace;
    H264_DPRINT("copied Frame and it has presentation time at %lld", (long long)(mFrame->pts));
    H264_DPRINT("Frame primary %d range %d transfer %d space %d", mFrame->color_primaries,
            mFrame->color_range, mFrame->color_trc, mFrame->colorspace);
}

void MediaH264DecoderFfmpeg::flush(void* ptr) {
    H264_DPRINT("Flushing...");
    mIsInFlush = true;
    H264_DPRINT("Flushing done");
}

static uint8_t* getDst(void* ptr) {
    // Guest will pass us the offset from the start address + 8 for where to
    // write the image data.
    uint8_t* xptr = (uint8_t*)ptr;
    uint64_t offset = *(uint64_t*)(xptr + 8);
    return (uint8_t*)ptr + offset;
}

static uint32_t getHostColorBufferId(void* ptr) {
    // Guest will pass us the hsot color buffer id to send decoded frame to
    uint8_t* xptr = (uint8_t*)ptr;
    uint32_t colorBufferId = *(uint32_t*)(xptr + 16);
    return colorBufferId;
}

void MediaH264DecoderFfmpeg::getImage(void* ptr) {
    H264_DPRINT("getImage %p", ptr);
    uint8_t* retptr = (uint8_t*)getReturnAddress(ptr);
    int* retErr = (int*)(retptr);
    uint32_t* retWidth = (uint32_t*)(retptr + 8);
    uint32_t* retHeight = (uint32_t*)(retptr + 16);
    uint32_t* retPts = (uint32_t*)(retptr + 24);
    uint32_t* retColorPrimaries = (uint32_t*)(retptr + 32);
    uint32_t* retColorRange = (uint32_t*)(retptr + 40);
    uint32_t* retColorTransfer = (uint32_t*)(retptr + 48);
    uint32_t* retColorSpace = (uint32_t*)(retptr + 56);

    static int numbers=0;
    //H264_DPRINT("calling getImage %d", numbers++);
    if (!mDecodedFrame) {
        H264_DPRINT("%s: frame is null", __func__);
        *retErr = static_cast<int>(Err::NoDecodedFrame);
        return;
    }
    if (!mImageReady) {
        if (mFrameFormatChanged) {
            *retWidth = mOutputWidth;
            *retHeight = mOutputHeight;
            *retErr = static_cast<int>(Err::DecoderRestarted);
            return;
        }
        if (mIsInFlush) {
            // guest be in flush mode, so try to get a frame
            avcodec_send_packet(mCodecCtx, NULL);
            int retframe = avcodec_receive_frame(mCodecCtx, mFrame);
            if (retframe == AVERROR(EAGAIN) || retframe == AVERROR_EOF) {
                H264_DPRINT("%s: frame is null", __func__);
                *retErr = static_cast<int>(Err::NoDecodedFrame);
                return;
            }

            if (retframe != 0) {
                char tmp[1024];
                av_strerror(retframe, tmp, sizeof(tmp));
                H264_DPRINT("WARNING: some unknown error %d: %s", retframe,
                            tmp);
                *retErr = static_cast<int>(Err::NoDecodedFrame);
                return;
            }
            H264_DPRINT("%s: got frame in flush mode retrun code %d", __func__, retframe);
            //now copy to mDecodedFrame
            copyFrame();
            mOutputPts = mFrame->pts;
            mImageReady = true;
        } else {
            H264_DPRINT("%s: no new frame yet", __func__);
            *retErr = static_cast<int>(Err::NoDecodedFrame);
            return;
        }
    }

    *retWidth = mOutputWidth;
    *retHeight = mOutputHeight;
    *retPts = mOutputPts;
    *retColorPrimaries = mColorPrimaries;
    *retColorRange = mColorRange;
    *retColorTransfer = mColorTransfer;
    *retColorSpace = mColorSpace;


    if (mVersion == 100) {
      uint8_t* dst =  getDst(ptr);
      memcpy(dst, mDecodedFrame, mOutBufferSize);
    } else if (mVersion == 200) {
        mRenderer.renderToHostColorBuffer(getHostColorBufferId(ptr),
                                          mOutputWidth, mOutputHeight,
                                          mDecodedFrame);
    }

    mImageReady = false;
    *retErr = mOutBufferSize;
}

}  // namespace emulation
}  // namespace android

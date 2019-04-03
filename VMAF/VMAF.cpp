/*
  MIT License

  Copyright (c) 2018-2019 HolyWu

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <VapourSynth.h>
#include <VSHelper.h>

#include <libvmaf.h>

struct VMAFData {
    VSNodeRef * reference, * distorted;
    const VSVideoInfo * vi;
    const VSAPI * vsapi;
    const VSFrameRef * ref, * main;
    double vmafScore;
    char * fmt, * logFmt, * pool;
    std::unique_ptr<char[]> modelPath, logPath;
    bool ssim, ms_ssim, ci, frameSet, eof;
    int numThreads, error;
    float divisor;
    std::thread vmafThread;
    std::mutex mtx;
    std::condition_variable cond;
};

template<typename T>
static int readFrame(float * VS_RESTRICT refData, float * VS_RESTRICT mainData, float * VS_RESTRICT tempData, const int strideByte, void * userData) noexcept {
    VMAFData * const VS_RESTRICT d = static_cast<VMAFData *>(userData);

    std::unique_lock<std::mutex> lck{ d->mtx };
    while (!d->frameSet && !d->eof)
        d->cond.wait(lck);

    if (d->frameSet) {
        const int width = d->vsapi->getFrameWidth(d->ref, 0);
        const int height = d->vsapi->getFrameHeight(d->ref, 0);
        const int srcStride = d->vsapi->getStride(d->ref, 0) / sizeof(T);
        const int dstStride = strideByte / sizeof(float);
        const T * refp = reinterpret_cast<const T *>(d->vsapi->getReadPtr(d->ref, 0));
        const T * mainp = reinterpret_cast<const T *>(d->vsapi->getReadPtr(d->main, 0));

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                refData[x] = refp[x] / d->divisor;
                mainData[x] = mainp[x] / d->divisor;
            }

            refp += srcStride;
            mainp += srcStride;
            refData += dstStride;
            mainData += dstStride;
        }
    }

    const bool ret = !d->frameSet;

    d->vsapi->freeFrame(d->ref);
    d->vsapi->freeFrame(d->main);
    d->ref = nullptr;
    d->main = nullptr;
    d->frameSet = false;

    d->cond.notify_one();

    return ret ? 2 : 0;
}

static void callVMAF(VMAFData * const VS_RESTRICT d) noexcept {
    if (d->vi->format->bytesPerSample == 1)
        d->error = compute_vmaf(&d->vmafScore, d->fmt, d->vi->width, d->vi->height, readFrame<uint8_t>, d, d->modelPath.get(), d->logPath.get(), d->logFmt, 0, 0, 0, 0, 0, d->ssim, d->ms_ssim, d->pool, d->numThreads, 1, d->ci);
    else
        d->error = compute_vmaf(&d->vmafScore, d->fmt, d->vi->width, d->vi->height, readFrame<uint16_t>, d, d->modelPath.get(), d->logPath.get(), d->logFmt, 0, 0, 0, 0, 0, d->ssim, d->ms_ssim, d->pool, d->numThreads, 1, d->ci);

    if (d->error) {
        d->mtx.lock();
        d->cond.notify_one();
        d->mtx.unlock();
    }
}

static void VS_CC vmafInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    VMAFData * d = static_cast<VMAFData *>(*instanceData);
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *VS_CC vmafGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    VMAFData * d = static_cast<VMAFData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->reference, frameCtx);
        vsapi->requestFrameFilter(n, d->distorted, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        std::unique_lock<std::mutex> lck{ d->mtx };
        while (d->frameSet && !d->error)
            d->cond.wait(lck);

        if (d->error) {
            vsapi->setFilterError("VMAF: libvmaf error", frameCtx);
            return nullptr;
        }

        const VSFrameRef * ref = vsapi->getFrameFilter(n, d->reference, frameCtx);
        const VSFrameRef * main = vsapi->getFrameFilter(n, d->distorted, frameCtx);
        d->ref = vsapi->cloneFrameRef(ref);
        d->main = vsapi->cloneFrameRef(main);
        d->frameSet = true;

        d->cond.notify_one();

        vsapi->freeFrame(main);
        return ref;
    }

    return nullptr;
}

static void VS_CC vmafFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    VMAFData * d = static_cast<VMAFData *>(instanceData);

    d->mtx.lock();
    d->eof = true;
    d->cond.notify_one();
    d->mtx.unlock();

    d->vmafThread.join();

    vsapi->freeNode(d->reference);
    vsapi->freeNode(d->distorted);

    delete d;
}

static void VS_CC vmafCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<VMAFData> d = std::make_unique<VMAFData>();
    int err;

    d->reference = vsapi->propGetNode(in, "reference", 0, nullptr);
    d->distorted = vsapi->propGetNode(in, "distorted", 0, nullptr);
    d->vi = vsapi->getVideoInfo(d->reference);
    d->vsapi = vsapi;

    try {
        if (!isConstantFormat(d->vi) || d->vi->format->sampleType != stInteger || d->vi->format->bitsPerSample > 16)
            throw std::string{ "only constant format 8-16 bit integer input supported" };

        if (d->vi->format->colorFamily == cmRGB)
            throw std::string{ "RGB color family is not supported" };

        if (!isSameFormat(vsapi->getVideoInfo(d->distorted), d->vi))
            throw std::string{ "both clips must have the same dimensions and be the same format" };

        if (vsapi->getVideoInfo(d->distorted)->numFrames != d->vi->numFrames)
            throw std::string{ "both clips' number of frames don't match" };

        const int model = int64ToIntS(vsapi->propGetInt(in, "model", 0, &err));

        const char * logPath = vsapi->propGetData(in, "log_path", 0, &err);

        const int logFmt = int64ToIntS(vsapi->propGetInt(in, "log_fmt", 0, &err));

        d->ssim = !!vsapi->propGetInt(in, "ssim", 0, &err);

        d->ms_ssim = !!vsapi->propGetInt(in, "ms_ssim", 0, &err);

        int pool = int64ToIntS(vsapi->propGetInt(in, "pool", 0, &err));
        if (err)
            pool = 1;

        d->ci = !!vsapi->propGetInt(in, "ci", 0, &err);

        if (model < 0 || model > 1)
            throw std::string{ "model must be 0 or 1" };

        if (logFmt < 0 || logFmt > 1)
            throw std::string{ "log_fmt must be 0 or 1" };

        if (pool < 0 || pool > 2)
            throw std::string{ "pool must be 0, 1, or 2" };

        d->fmt = const_cast<char *>("yuv420p");

        const std::string pluginPath{ vsapi->getPluginPath(vsapi->getPluginById("com.holywu.vmaf", core)) };
        std::string modelPath{ pluginPath.substr(0, pluginPath.find_last_of('/')) };
        if (model == 0)
            modelPath += d->ci ? "/model/vmaf_b_v0.6.3/vmaf_b_v0.6.3.pkl" : "/model/vmaf_v0.6.1.pkl";
        else
            modelPath += d->ci ? "/model/vmaf_4k_rb_v0.6.2/vmaf_4k_rb_v0.6.2.pkl" : "/model/vmaf_4k_v0.6.1.pkl";
        d->modelPath = std::make_unique<char[]>(modelPath.length() + 1);
        std::strcpy(d->modelPath.get(), modelPath.c_str());

        if (logPath) {
            d->logPath = std::make_unique<char[]>(vsapi->propGetDataSize(in, "log_path", 0, nullptr) + 1);
            std::strcpy(d->logPath.get(), logPath);
        }

        if (logFmt == 0)
            d->logFmt = const_cast<char *>("xml");
        else
            d->logFmt = const_cast<char *>("json");

        if (pool == 0)
            d->pool = const_cast<char *>("mean");
        else if (pool == 1)
            d->pool = const_cast<char *>("harmonic_mean");
        else
            d->pool = const_cast<char *>("min");

        d->numThreads = vsapi->getCoreInfo(core)->numThreads;

        d->divisor = 1 << (d->vi->format->bitsPerSample - 8);

        d->vmafThread = std::thread{ callVMAF, d.get() };
    } catch (const std::string & error) {
        vsapi->setError(out, ("VMAF: " + error).c_str());
        vsapi->freeNode(d->reference);
        vsapi->freeNode(d->distorted);
        return;
    }

    vsapi->createFilter(in, out, "VMAF", vmafInit, vmafGetFrame, vmafFree, fmSerial, nfMakeLinear, d.release(), core);
}

//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("com.holywu.vmaf", "vmaf", "Video Multi-Method Assessment Fusion", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("VMAF",
                 "reference:clip;"
                 "distorted:clip;"
                 "model:int:opt;"
                 "log_path:data:opt;"
                 "log_fmt:int:opt;"
                 "ssim:int:opt;"
                 "ms_ssim:int:opt;"
                 "pool:int:opt;"
                 "ci:int:opt;",
                 vmafCreate, nullptr, plugin);
}

/*
  MIT License

  Copyright (c) 2018-2021 HolyWu

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

#include <cstring>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <VapourSynth.h>
#include <VSHelper.h>

extern "C" {
#include <libvmaf.h>
}

using namespace std::literals;

static constexpr const char* modelName[]{ "vmaf", "vmaf_neg", "vmaf_b", "vmaf_4k" };
static constexpr const char* modelVersion[]{ "vmaf_v0.6.1", "vmaf_v0.6.1neg", "vmaf_b_v0.6.3", "vmaf_4k_v0.6.1" };

static constexpr const char* featureName[]{ "psnr", "psnr_hvs", "float_ssim", "float_ms_ssim", "ciede" };

struct VMAFData final {
    VSNodeRef* reference;
    VSNodeRef* distorted;
    const VSVideoInfo* vi;
    std::string logPath;
    VmafOutputFormat logFormat;
    std::vector<VmafModel*> model;
    std::vector<VmafModelCollection*> modelCollection;
    VmafContext* vmaf;
    VmafPixelFormat pixelFormat;
    bool chroma;
};

static void VS_CC vmafInit([[maybe_unused]] VSMap* in, [[maybe_unused]] VSMap* out, void** instanceData, VSNode* node, [[maybe_unused]] VSCore* core, const VSAPI* vsapi) {
    auto d{ static_cast<const VMAFData*>(*instanceData) };
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef* VS_CC vmafGetFrame(int n, int activationReason, void** instanceData, [[maybe_unused]] void** frameData, VSFrameContext* frameCtx, [[maybe_unused]] VSCore* core, const VSAPI* vsapi) {
    auto d{ static_cast<const VMAFData*>(*instanceData) };

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->reference, frameCtx);
        vsapi->requestFrameFilter(n, d->distorted, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        auto reference{ vsapi->getFrameFilter(n, d->reference, frameCtx) };
        auto distorted{ vsapi->getFrameFilter(n, d->distorted, frameCtx) };

        VmafPicture ref{}, dist{};

        try {
            if (vmaf_picture_alloc(&ref, d->pixelFormat, d->vi->format->bitsPerSample, d->vi->width, d->vi->height) ||
                vmaf_picture_alloc(&dist, d->pixelFormat, d->vi->format->bitsPerSample, d->vi->width, d->vi->height))
                throw "failed to allocate picture";

            for (auto plane{ 0 }; plane < d->vi->format->numPlanes; plane++) {
                if (plane && !d->chroma)
                    break;

                vs_bitblt(ref.data[plane],
                          ref.stride[plane],
                          vsapi->getReadPtr(reference, plane),
                          vsapi->getStride(reference, plane),
                          vsapi->getFrameWidth(reference, plane) * d->vi->format->bytesPerSample,
                          vsapi->getFrameHeight(reference, plane));

                vs_bitblt(dist.data[plane],
                          dist.stride[plane],
                          vsapi->getReadPtr(distorted, plane),
                          vsapi->getStride(distorted, plane),
                          vsapi->getFrameWidth(distorted, plane) * d->vi->format->bytesPerSample,
                          vsapi->getFrameHeight(distorted, plane));
            }

            if (vmaf_read_pictures(d->vmaf, &ref, &dist, n))
                throw "failed to read pictures";
        } catch (const char* error) {
            vsapi->setFilterError(("VMAF: "s + error).c_str(), frameCtx);

            vsapi->freeFrame(reference);
            vsapi->freeFrame(distorted);

            vmaf_picture_unref(&ref);
            vmaf_picture_unref(&dist);

            return nullptr;
        }

        vsapi->freeFrame(distorted);
        return reference;
    }

    return nullptr;
}

static void VS_CC vmafFree(void* instanceData, [[maybe_unused]] VSCore* core, const VSAPI* vsapi) {
    auto d{ static_cast<VMAFData*>(instanceData) };

    vsapi->freeNode(d->reference);
    vsapi->freeNode(d->distorted);

    if (vmaf_read_pictures(d->vmaf, nullptr, nullptr, 0))
        vsapi->logMessage(mtCritical, "failed to flush context");

    for (auto&& m : d->model)
        if (double score; vmaf_score_pooled(d->vmaf, m, VMAF_POOL_METHOD_MEAN, &score, 0, d->vi->numFrames - 1))
            vsapi->logMessage(mtCritical, "failed to generate pooled VMAF score");

    for (auto&& m : d->modelCollection)
        if (VmafModelCollectionScore score; vmaf_score_pooled_model_collection(d->vmaf, m, VMAF_POOL_METHOD_MEAN, &score, 0, d->vi->numFrames - 1))
            vsapi->logMessage(mtCritical, "failed to generate pooled VMAF score");

    if (vmaf_write_output(d->vmaf, d->logPath.c_str(), d->logFormat))
        vsapi->logMessage(mtCritical, "failed to write VMAF stats");

    for (auto&& m : d->model)
        vmaf_model_destroy(m);
    for (auto&& m : d->modelCollection)
        vmaf_model_collection_destroy(m);
    vmaf_close(d->vmaf);

    delete d;
}

static void VS_CC vmafCreate(const VSMap* in, VSMap* out, [[maybe_unused]] void* userData, VSCore* core, const VSAPI* vsapi) {
    auto d{ std::make_unique<VMAFData>() };

    try {
        d->reference = vsapi->propGetNode(in, "reference", 0, nullptr);
        d->distorted = vsapi->propGetNode(in, "distorted", 0, nullptr);
        d->vi = vsapi->getVideoInfo(d->reference);
        int err;

        if (!isConstantFormat(d->vi) ||
            d->vi->format->colorFamily != cmYUV ||
            d->vi->format->sampleType != stInteger ||
            d->vi->format->bitsPerSample > 16)
            throw "only constant YUV format 8-16 bit integer input supported";

        if (!((d->vi->format->subSamplingW == 1 && d->vi->format->subSamplingH == 1) ||
              (d->vi->format->subSamplingW == 1 && d->vi->format->subSamplingH == 0) ||
              (d->vi->format->subSamplingW == 0 && d->vi->format->subSamplingH == 0)))
            throw "only 420/422/444 chroma subsampling is supported";

        if (!isSameFormat(vsapi->getVideoInfo(d->distorted), d->vi))
            throw "both clips must have the same format and dimensions";

        if (vsapi->getVideoInfo(d->distorted)->numFrames != d->vi->numFrames)
            throw "both clips' number of frames does not match";

        d->logPath = vsapi->propGetData(in, "log_path", 0, nullptr);

        auto logFormat{ int64ToIntS(vsapi->propGetInt(in, "log_format", 0, &err)) };

        std::unique_ptr<int64_t[]> model;
        auto numModel{ vsapi->propNumElements(in, "model") };
        if (numModel <= 0) {
            model = std::make_unique<int64_t[]>(1);
            model[0] = 0;
            numModel = 1;
        } else {
            model = std::make_unique<int64_t[]>(numModel);
            for (auto i{ 0 }; i < numModel; i++)
                model[i] = vsapi->propGetInt(in, "model", i, nullptr);
        }
        d->model.resize(numModel);

        auto feature{ vsapi->propGetIntArray(in, "feature", &err) };
        auto numFeature{ vsapi->propNumElements(in, "feature") };

        if (logFormat < 0 || logFormat > 3)
            throw "log_format must be 0, 1, 2, or 3";

        d->logFormat = static_cast<VmafOutputFormat>(logFormat + 1);

        VSCoreInfo info;
        vsapi->getCoreInfo2(core, &info);

        VmafConfiguration configuration{};
        configuration.log_level = VMAF_LOG_LEVEL_INFO;
        configuration.n_threads = info.numThreads;
        configuration.n_subsample = 1;
        configuration.cpumask = 0;

        if (vmaf_init(&d->vmaf, configuration))
            throw "failed to initialize VMAF context";

        for (auto i{ 0 }; i < numModel; i++) {
            if (model[i] < 0 || model[i] > 3)
                throw "model must be 0, 1, 2, or 3";

            if (std::count(model.get(), model.get() + numModel, model[i]) > 1)
                throw "duplicate model specified";

            VmafModelConfig modelConfig{};
            modelConfig.name = modelName[model[i]];
            modelConfig.flags = VMAF_MODEL_FLAGS_DEFAULT;

            if (vmaf_model_load(&d->model[i], &modelConfig, modelVersion[model[i]])) {
                d->modelCollection.resize(d->modelCollection.size() + 1);

                if (vmaf_model_collection_load(&d->model[i], &d->modelCollection[d->modelCollection.size() - 1], &modelConfig, modelVersion[model[i]]))
                    throw ("failed to load model: "s + modelVersion[model[i]]).c_str();

                if (vmaf_use_features_from_model_collection(d->vmaf, d->modelCollection[d->modelCollection.size() - 1]))
                    throw ("failed to load feature extractors from model collection: "s + modelVersion[model[i]]).c_str();

                continue;
            }

            if (vmaf_use_features_from_model(d->vmaf, d->model[i]))
                throw ("failed to load feature extractors from model: "s + modelVersion[model[i]]).c_str();
        }

        for (auto i{ 0 }; i < numFeature; i++) {
            if (feature[i] < 0 || feature[i] > 4)
                throw "feature must be 0, 1, 2, 3, or 4";

            if (std::count(feature, feature + numFeature, feature[i]) > 1)
                throw "duplicate feature specified";

            if (vmaf_use_feature(d->vmaf, featureName[feature[i]], nullptr))
                throw ("failed to load feature extractor: "s + featureName[feature[i]]).c_str();

            if (!std::strcmp(featureName[feature[i]], "psnr") ||
                !std::strcmp(featureName[feature[i]], "psnr_hvs") ||
                !std::strcmp(featureName[feature[i]], "ciede"))
                d->chroma = true;
        }

        if (d->vi->format->subSamplingW == 1 && d->vi->format->subSamplingH == 1)
            d->pixelFormat = VMAF_PIX_FMT_YUV420P;
        else if (d->vi->format->subSamplingW == 1 && d->vi->format->subSamplingH == 0)
            d->pixelFormat = VMAF_PIX_FMT_YUV422P;
        else
            d->pixelFormat = VMAF_PIX_FMT_YUV444P;
    } catch (const char* error) {
        vsapi->setError(out, ("VMAF: "s + error).c_str());

        vsapi->freeNode(d->reference);
        vsapi->freeNode(d->distorted);

        for (auto&& m : d->model)
            vmaf_model_destroy(m);
        for (auto&& m : d->modelCollection)
            vmaf_model_collection_destroy(m);
        vmaf_close(d->vmaf);

        return;
    }

    vsapi->createFilter(in, out, "VMAF", vmafInit, vmafGetFrame, vmafFree, fmSerial, nfMakeLinear, d.release(), core);
}

//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin* plugin) {
    configFunc("com.holywu.vmaf", "vmaf", "Video Multi-Method Assessment Fusion", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("VMAF",
                 "reference:clip;"
                 "distorted:clip;"
                 "log_path:data;"
                 "log_format:int:opt;"
                 "model:int[]:opt;"
                 "feature:int[]:opt;",
                 vmafCreate, nullptr, plugin);
}

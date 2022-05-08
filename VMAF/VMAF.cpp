/*
    MIT License

    Copyright (c) 2018-2022 HolyWu

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
#include <array>
#include <charconv>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <VapourSynth4.h>
#include <VSHelper4.h>

extern "C" {
#include <libvmaf.h>
}

using namespace std::literals;

static constexpr const char* modelName[]{ "vmaf", "vmaf_neg", "vmaf_b", "vmaf_4k" };
static constexpr const char* modelVersion[]{ "vmaf_v0.6.1", "vmaf_v0.6.1neg", "vmaf_b_v0.6.3", "vmaf_4k_v0.6.1" };

static constexpr const char* featureName[]{ "psnr", "psnr_hvs", "float_ssim", "float_ms_ssim", "ciede" };

struct VMAFData final {
    std::string filterName;
    VSNode* reference;
    VSNode* distorted;
    const VSVideoInfo* vi;
    std::string logPath;
    VmafOutputFormat logFormat;
    std::vector<VmafModel*> model;
    std::vector<VmafModelCollection*> modelCollection;
    VmafContext* vmaf;
    VmafPixelFormat pixelFormat;
    bool chroma;
};

static const VSFrame* VS_CC vmafGetFrame(int n, int activationReason, void* instanceData, [[maybe_unused]] void** frameData, VSFrameContext* frameCtx, [[maybe_unused]] VSCore* core, const VSAPI* vsapi) {
    auto d{ static_cast<const VMAFData*>(instanceData) };

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->reference, frameCtx);
        if (d->filterName == "VMAF")
            vsapi->requestFrameFilter(n, d->distorted, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        auto reference{ vsapi->getFrameFilter(n, d->reference, frameCtx) };
        auto distorted{ d->filterName == "VMAF" ? vsapi->getFrameFilter(n, d->distorted, frameCtx) : vsapi->addFrameRef(reference) };

        VmafPicture ref;
        VmafPicture dist;

        try {
            if (vmaf_picture_alloc(&ref, d->pixelFormat, d->vi->format.bitsPerSample, d->vi->width, d->vi->height) ||
                vmaf_picture_alloc(&dist, d->pixelFormat, d->vi->format.bitsPerSample, d->vi->width, d->vi->height))
                throw "failed to allocate picture";

            for (auto plane{ 0 }; plane < d->vi->format.numPlanes; plane++) {
                if (plane && !d->chroma)
                    break;

                vsh::bitblt(ref.data[plane],
                            ref.stride[plane],
                            vsapi->getReadPtr(reference, plane),
                            vsapi->getStride(reference, plane),
                            vsapi->getFrameWidth(reference, plane) * d->vi->format.bytesPerSample,
                            vsapi->getFrameHeight(reference, plane));

                vsh::bitblt(dist.data[plane],
                            dist.stride[plane],
                            vsapi->getReadPtr(distorted, plane),
                            vsapi->getStride(distorted, plane),
                            vsapi->getFrameWidth(distorted, plane) * d->vi->format.bytesPerSample,
                            vsapi->getFrameHeight(distorted, plane));
            }

            if (vmaf_read_pictures(d->vmaf, &ref, &dist, n))
                throw "failed to read pictures";
        } catch (const char* error) {
            vsapi->setFilterError((d->filterName + ": " + error).c_str(), frameCtx);

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

static void VS_CC vmafFree(void* instanceData, VSCore* core, const VSAPI* vsapi) {
    auto d{ static_cast<VMAFData*>(instanceData) };

    vsapi->freeNode(d->reference);
    vsapi->freeNode(d->distorted);

    static auto logMessage = [&](const char* msg) noexcept {
        vsapi->logMessage(mtCritical, (d->filterName + ": " + msg).c_str(), core);
    };

    if (vmaf_read_pictures(d->vmaf, nullptr, nullptr, 0))
        logMessage("failed to flush context");

    for (auto&& m : d->model)
        if (double score; vmaf_score_pooled(d->vmaf, m, VMAF_POOL_METHOD_MEAN, &score, 0, d->vi->numFrames - 1))
            logMessage("failed to generate pooled VMAF score");

    for (auto&& m : d->modelCollection)
        if (VmafModelCollectionScore score; vmaf_score_pooled_model_collection(d->vmaf, m, VMAF_POOL_METHOD_MEAN, &score, 0, d->vi->numFrames - 1))
            logMessage("failed to generate pooled VMAF score");

    if (vmaf_write_output(d->vmaf, d->logPath.c_str(), d->logFormat))
        logMessage("failed to write VMAF stats");

    for (auto&& m : d->model)
        vmaf_model_destroy(m);
    for (auto&& m : d->modelCollection)
        vmaf_model_collection_destroy(m);
    vmaf_close(d->vmaf);

    delete d;
}

static void VS_CC vmafCreate(const VSMap* in, VSMap* out, void* userData, VSCore* core, const VSAPI* vsapi) {
    auto d{ std::make_unique<VMAFData>() };

    try {
        d->filterName = static_cast<const char*>(userData);

        if (d->filterName == "VMAF") {
            d->reference = vsapi->mapGetNode(in, "reference", 0, nullptr);
            d->distorted = vsapi->mapGetNode(in, "distorted", 0, nullptr);
        } else {
            d->reference = vsapi->mapGetNode(in, "clip", 0, nullptr);
        }
        d->vi = vsapi->getVideoInfo(d->reference);
        int err;

        if (!vsh::isConstantVideoFormat(d->vi) ||
            d->vi->format.colorFamily != cfYUV ||
            d->vi->format.sampleType != stInteger ||
            d->vi->format.bitsPerSample > 16)
            throw "only constant YUV format 8-16 bit integer input supported"s;

        if (!((d->vi->format.subSamplingW == 1 && d->vi->format.subSamplingH == 1) ||
              (d->vi->format.subSamplingW == 1 && d->vi->format.subSamplingH == 0) ||
              (d->vi->format.subSamplingW == 0 && d->vi->format.subSamplingH == 0)))
            throw "only 420/422/444 chroma subsampling is supported"s;

        d->logPath = vsapi->mapGetData(in, "log_path", 0, nullptr);
        auto logFormat{ vsapi->mapGetIntSaturated(in, "log_format", 0, &err) };

        if (logFormat < 0 || logFormat > 3)
            throw "log_format must be 0, 1, 2, or 3"s;

        d->logFormat = static_cast<VmafOutputFormat>(logFormat + 1);

        VSCoreInfo info;
        vsapi->getCoreInfo(core, &info);

        VmafConfiguration configuration{};
        configuration.log_level = VMAF_LOG_LEVEL_INFO;
        configuration.n_threads = info.numThreads;
        configuration.n_subsample = 1;
        configuration.cpumask = 0;

        if (vmaf_init(&d->vmaf, configuration))
            throw "failed to initialize VMAF context"s;

        if (d->filterName == "VMAF") {
            if (!vsh::isSameVideoInfo(vsapi->getVideoInfo(d->distorted), d->vi))
                throw "both clips must have the same format and dimensions"s;

            if (vsapi->getVideoInfo(d->distorted)->numFrames != d->vi->numFrames)
                throw "both clips' number of frames do not match"s;

            auto model{ vsapi->mapGetIntArray(in, "model", &err) };
            auto numModels{ vsapi->mapNumElements(in, "model") };
            auto feature{ vsapi->mapGetIntArray(in, "feature", &err) };
            auto numFeatures{ vsapi->mapNumElements(in, "feature") };

            if (numModels > 0)
                d->model.resize(numModels);

            for (auto i{ 0 }; i < numModels; i++) {
                if (model[i] < 0 || model[i] > 3)
                    throw "model must be 0, 1, 2, or 3"s;

                if (std::count(model, model + numModels, model[i]) > 1)
                    throw "duplicate model specified"s;

                VmafModelConfig modelConfig{};
                modelConfig.name = modelName[model[i]];
                modelConfig.flags = VMAF_MODEL_FLAGS_DEFAULT;

                if (vmaf_model_load(&d->model[i], &modelConfig, modelVersion[model[i]])) {
                    d->modelCollection.resize(d->modelCollection.size() + 1);

                    if (vmaf_model_collection_load(&d->model[i], &d->modelCollection[d->modelCollection.size() - 1], &modelConfig, modelVersion[model[i]]))
                        throw "failed to load model: "s + modelVersion[model[i]];

                    if (vmaf_use_features_from_model_collection(d->vmaf, d->modelCollection[d->modelCollection.size() - 1]))
                        throw "failed to load feature extractors from model collection: "s + modelVersion[model[i]];

                    continue;
                }

                if (vmaf_use_features_from_model(d->vmaf, d->model[i]))
                    throw "failed to load feature extractors from model: "s + modelVersion[model[i]];
            }

            for (auto i{ 0 }; i < numFeatures; i++) {
                if (feature[i] < 0 || feature[i] > 4)
                    throw "feature must be 0, 1, 2, 3, or 4"s;

                if (std::count(feature, feature + numFeatures, feature[i]) > 1)
                    throw "duplicate feature specified"s;

                if (vmaf_use_feature(d->vmaf, featureName[feature[i]], nullptr))
                    throw "failed to load feature extractor: "s + featureName[feature[i]];

                if (!std::strcmp(featureName[feature[i]], "psnr") ||
                    !std::strcmp(featureName[feature[i]], "psnr_hvs") ||
                    !std::strcmp(featureName[feature[i]], "ciede"))
                    d->chroma = true;
            }
        } else {
            std::array<char, 32> str{};
            VmafFeatureDictionary* featureDictionary{};

            static auto setFeatureDictionary = [&](auto val, const char* key) {
                if (auto [ptr, ec] = std::to_chars(str.data(), str.data() + str.size(), val); ec == std::errc())
                    if (vmaf_feature_dictionary_set(&featureDictionary, key, std::string(str.data(), ptr).c_str()))
                        throw "failed to set feature option: "s + key;
            };

            auto window_size{ vsapi->mapGetIntSaturated(in, "window_size", 0, &err) };
            if (!err) {
                if (window_size < 15 || window_size > 127)
                    throw "window_size must be between 15 and 127 (inclusive)"s;

                setFeatureDictionary(window_size, "window_size");
            }

            auto topk{ vsapi->mapGetFloat(in, "topk", 0, &err) };
            if (!err) {
                if (topk < 0.0001 || topk > 1.0)
                    throw "topk must be between 0.0001 and 1.0 (inclusive)"s;

                setFeatureDictionary(topk, "topk");
            }

            auto tvi_threshold{ vsapi->mapGetFloat(in, "tvi_threshold", 0, &err) };
            if (!err) {
                if (tvi_threshold < 0.0001 || tvi_threshold > 1.0)
                    throw "tvi_threshold must be between 0.0001 and 1.0 (inclusive)"s;

                setFeatureDictionary(tvi_threshold, "tvi_threshold");
            }

            auto max_log_contrast{ vsapi->mapGetIntSaturated(in, "max_log_contrast", 0, &err) };
            if (!err) {
                if (max_log_contrast < 0 || max_log_contrast > 5)
                    throw "max_log_contrast must be between 0 and 5 (inclusive)"s;

                setFeatureDictionary(max_log_contrast, "max_log_contrast");
            }

            auto enc_width{ vsapi->mapGetIntSaturated(in, "enc_width", 0, &err) };
            if (!err)
                setFeatureDictionary(enc_width, "enc_width");

            auto enc_height{ vsapi->mapGetIntSaturated(in, "enc_height", 0, &err) };
            if (!err)
                setFeatureDictionary(enc_height, "enc_height");

            if (vmaf_use_feature(d->vmaf, "cambi", featureDictionary)) {
                vmaf_feature_dictionary_free(&featureDictionary);
                throw "failed to load feature extractor: cambi"s;
            }
        }

        if (d->vi->format.subSamplingW == 1 && d->vi->format.subSamplingH == 1)
            d->pixelFormat = VMAF_PIX_FMT_YUV420P;
        else if (d->vi->format.subSamplingW == 1 && d->vi->format.subSamplingH == 0)
            d->pixelFormat = VMAF_PIX_FMT_YUV422P;
        else
            d->pixelFormat = VMAF_PIX_FMT_YUV444P;
    } catch (const std::string& error) {
        vsapi->mapSetError(out, (d->filterName + ": " + error).c_str());

        vsapi->freeNode(d->reference);
        vsapi->freeNode(d->distorted);

        for (auto&& m : d->model)
            vmaf_model_destroy(m);
        for (auto&& m : d->modelCollection)
            vmaf_model_collection_destroy(m);
        vmaf_close(d->vmaf);

        return;
    }

    std::vector<VSFilterDependency> deps;
    deps.push_back({ d->reference, rpStrictSpatial });
    if (d->filterName == "VMAF")
        deps.push_back({ d->distorted, rpStrictSpatial });

    vsapi->createVideoFilter(out, d->filterName.c_str(), d->vi, vmafGetFrame, vmafFree, fmFrameState, deps.data(), deps.size(), d.get(), core);
    d.release();
}

//////////////////////////////////////////
// Metric

struct MetricData final {
    VSNode* reference;
    VSNode* distorted;
    const VSVideoInfo* vi;
    std::vector<int> feature;
    std::vector<const char*> featureScoreName;
    VmafConfiguration configuration;
    VmafPixelFormat pixelFormat;
    bool chroma;
};

static const VSFrame* VS_CC metricGetFrame(int n, int activationReason, void* instanceData, [[maybe_unused]] void** frameData, VSFrameContext* frameCtx, VSCore* core, const VSAPI* vsapi) {
    auto d{ static_cast<const MetricData*>(instanceData) };

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->reference, frameCtx);
        vsapi->requestFrameFilter(n, d->distorted, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        auto reference{ vsapi->getFrameFilter(n, d->reference, frameCtx) };
        auto distorted{ vsapi->getFrameFilter(n, d->distorted, frameCtx) };
        auto dst{ vsapi->copyFrame(distorted, core) };
        auto props{ vsapi->getFramePropertiesRW(dst) };

        VmafContext* vmaf{};
        VmafPicture ref{};
        VmafPicture dist{};

        try {
            if (vmaf_init(&vmaf, d->configuration))
                throw "failed to initialize VMAF context";

            for (auto&& f : d->feature)
                if (vmaf_use_feature(vmaf, featureName[f], nullptr))
                    throw ("failed to load feature extractor: "s + featureName[f]).c_str();

            if (vmaf_picture_alloc(&ref, d->pixelFormat, d->vi->format.bitsPerSample, d->vi->width, d->vi->height) ||
                vmaf_picture_alloc(&dist, d->pixelFormat, d->vi->format.bitsPerSample, d->vi->width, d->vi->height))
                throw "failed to allocate picture";

            for (auto plane{ 0 }; plane < d->vi->format.numPlanes; plane++) {
                if (plane && !d->chroma)
                    break;

                vsh::bitblt(ref.data[plane],
                            ref.stride[plane],
                            vsapi->getReadPtr(reference, plane),
                            vsapi->getStride(reference, plane),
                            vsapi->getFrameWidth(reference, plane) * d->vi->format.bytesPerSample,
                            vsapi->getFrameHeight(reference, plane));

                vsh::bitblt(dist.data[plane],
                            dist.stride[plane],
                            vsapi->getReadPtr(distorted, plane),
                            vsapi->getStride(distorted, plane),
                            vsapi->getFrameWidth(distorted, plane) * d->vi->format.bytesPerSample,
                            vsapi->getFrameHeight(distorted, plane));
            }

            if (vmaf_read_pictures(vmaf, &ref, &dist, 0))
                throw "failed to read pictures";

            if (vmaf_read_pictures(vmaf, nullptr, nullptr, 0))
                throw "failed to flush context";

            for (auto&& f : d->featureScoreName) {
                double score;

                if (vmaf_feature_score_at_index(vmaf, f, &score, 0))
                    throw ("failed to fetch feature score: "s + f).c_str();

                vsapi->mapSetFloat(props, f, score, maReplace);
            }

            vmaf_close(vmaf);
        } catch (const char* error) {
            vsapi->setFilterError(("Metric: "s + error).c_str(), frameCtx);

            vsapi->freeFrame(reference);
            vsapi->freeFrame(distorted);
            vsapi->freeFrame(dst);

            vmaf_close(vmaf);
            vmaf_picture_unref(&ref);
            vmaf_picture_unref(&dist);

            return nullptr;
        }

        vsapi->freeFrame(reference);
        vsapi->freeFrame(distorted);
        return dst;
    }

    return nullptr;
}

static void VS_CC metricFree(void* instanceData, [[maybe_unused]] VSCore* core, const VSAPI* vsapi) {
    auto d{ static_cast<MetricData*>(instanceData) };
    vsapi->freeNode(d->reference);
    vsapi->freeNode(d->distorted);
    delete d;
}

static void VS_CC metricCreate(const VSMap* in, VSMap* out, [[maybe_unused]] void* userData, VSCore* core, const VSAPI* vsapi) {
    auto d{ std::make_unique<MetricData>() };

    try {
        d->reference = vsapi->mapGetNode(in, "reference", 0, nullptr);
        d->distorted = vsapi->mapGetNode(in, "distorted", 0, nullptr);
        d->vi = vsapi->getVideoInfo(d->reference);

        if (!vsh::isConstantVideoFormat(d->vi) ||
            d->vi->format.colorFamily != cfYUV ||
            d->vi->format.sampleType != stInteger ||
            d->vi->format.bitsPerSample > 16)
            throw "only constant YUV format 8-16 bit integer input supported";

        if (!((d->vi->format.subSamplingW == 1 && d->vi->format.subSamplingH == 1) ||
              (d->vi->format.subSamplingW == 1 && d->vi->format.subSamplingH == 0) ||
              (d->vi->format.subSamplingW == 0 && d->vi->format.subSamplingH == 0)))
            throw "only 420/422/444 chroma subsampling is supported";

        if (!vsh::isSameVideoInfo(vsapi->getVideoInfo(d->distorted), d->vi))
            throw "both clips must have the same format and dimensions";

        if (vsapi->getVideoInfo(d->distorted)->numFrames != d->vi->numFrames)
            throw "both clips' number of frames do not match";

        VSCoreInfo info;
        vsapi->getCoreInfo(core, &info);

        d->configuration.log_level = VMAF_LOG_LEVEL_INFO;
        d->configuration.n_threads = info.numThreads;
        d->configuration.n_subsample = 1;
        d->configuration.cpumask = 0;

        for (auto i{ 0 }; i < vsapi->mapNumElements(in, "feature"); i++) {
            d->feature.emplace_back(vsapi->mapGetIntSaturated(in, "feature", i, nullptr));

            if (d->feature[i] < 0 || d->feature[i] > 4)
                throw "feature must be 0, 1, 2, 3, or 4";

            if (std::count(d->feature.cbegin(), d->feature.cend(), d->feature[i]) > 1)
                throw "duplicate feature specified";

            switch (d->feature[i]) {
            case 0:
                d->featureScoreName.emplace_back("psnr_y");
                d->featureScoreName.emplace_back("psnr_cb");
                d->featureScoreName.emplace_back("psnr_cr");
                d->chroma = true;
                break;
            case 1:
                d->featureScoreName.emplace_back("psnr_hvs_y");
                d->featureScoreName.emplace_back("psnr_hvs_cb");
                d->featureScoreName.emplace_back("psnr_hvs_cr");
                d->featureScoreName.emplace_back("psnr_hvs");
                d->chroma = true;
                break;
            case 2:
                d->featureScoreName.emplace_back("float_ssim");
                break;
            case 3:
                d->featureScoreName.emplace_back("float_ms_ssim");
                break;
            case 4:
                d->featureScoreName.emplace_back("ciede2000");
                d->chroma = true;
                break;
            }
        }

        if (d->vi->format.subSamplingW == 1 && d->vi->format.subSamplingH == 1)
            d->pixelFormat = VMAF_PIX_FMT_YUV420P;
        else if (d->vi->format.subSamplingW == 1 && d->vi->format.subSamplingH == 0)
            d->pixelFormat = VMAF_PIX_FMT_YUV422P;
        else
            d->pixelFormat = VMAF_PIX_FMT_YUV444P;
    } catch (const char* error) {
        vsapi->mapSetError(out, ("Metric: "s + error).c_str());
        vsapi->freeNode(d->reference);
        vsapi->freeNode(d->distorted);
        return;
    }

    VSFilterDependency deps[]{ {d->reference, rpStrictSpatial}, {d->distorted, rpStrictSpatial} };
    vsapi->createVideoFilter(out, "Metric", d->vi, metricGetFrame, metricFree, fmParallel, deps, 2, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
    vspapi->configPlugin("com.holywu.vmaf", "vmaf", "Video Multi-Method Assessment Fusion", VS_MAKE_VERSION(9, 0), VAPOURSYNTH_API_VERSION, 0, plugin);

    vspapi->registerFunction("VMAF",
                             "reference:vnode;"
                             "distorted:vnode;"
                             "log_path:data;"
                             "log_format:int:opt;"
                             "model:int[]:opt;"
                             "feature:int[]:opt;",
                             "clip:vnode;",
                             vmafCreate, const_cast<char*>("VMAF"), plugin);

    vspapi->registerFunction("CAMBI",
                             "clip:vnode;"
                             "log_path:data;"
                             "log_format:int:opt;"
                             "window_size:int:opt;"
                             "topk:float:opt;"
                             "tvi_threshold:float:opt;"
                             "max_log_contrast:int:opt;"
                             "enc_width:int:opt;"
                             "enc_height:int:opt;",
                             "clip:vnode;",
                             vmafCreate, const_cast<char*>("CAMBI"), plugin);

    vspapi->registerFunction("Metric",
                             "reference:vnode;"
                             "distorted:vnode;"
                             "feature:int[];",
                             "clip:vnode;",
                             metricCreate, nullptr, plugin);
}

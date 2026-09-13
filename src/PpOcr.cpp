#include "PpOcr.h"

#include "AppPaths.h"
#include "Logger.h"
#include "OcrNormalize.h"
#include "OcrSelection.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Gdiplus;

namespace PpOcr {
namespace {

Options g_options;

// ---------------------------------------------------------------------------
// onnxruntime, loaded at run time with LoadLibraryW so the app starts fine
// when the DLL is absent (the Windows engine takes over).
// ---------------------------------------------------------------------------

#define ORT_CHECK(call)                                                    \
    do {                                                                   \
        if (OrtStatus* st_ = (call)) {                                     \
            const std::string msg_ = g_api->GetErrorMessage(st_);          \
            g_api->ReleaseStatus(st_);                                     \
            t_lastError = L"onnxruntime: " + Widen(msg_);                  \
            return false;                                                  \
        }                                                                  \
    } while (0)

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
    std::wstring out(need, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), need);
    return out;
}

const OrtApi* g_api = nullptr;

// The error channel is thread-local: recognition rows run across a pool,
// and several workers can fail in the same call - concurrent operator= on
// one shared wstring is a data race that has corrupted heaps for less.
// Each failing thread records into its own copy; the calling thread
// harvests the message it cares about after joining (see Recognize).
// Startup failures happen on the first caller's thread, so they are also
// mirrored into g_startupError for EnsureLoaded readers on other threads.
thread_local std::wstring t_lastError;
std::mutex g_errorMutex;
std::wstring g_startupError;

struct Session {
    OrtSession* session = nullptr;
    std::string inputName;
    std::string outputName;

    Session() = default;
    ~Session() {
        if (session && g_api) g_api->ReleaseSession(session);
    }

    // An OrtSession has exactly one owner. Declaring the destructor used to
    // be the whole story - but it also suppressed the implicit moves, so
    // `e.rec = std::move(fresh)` silently compiled as a COPY: two Session
    // objects held one OrtSession, `fresh`'s destructor released it on
    // return, and the next inference ran on freed heap (the 0xFEEEFEEE
    // access violation inside onnxruntime). Copies are now gone for good,
    // and moves steal the pointer and disarm the source.
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&& other) noexcept
        : session(other.session),
          inputName(std::move(other.inputName)),
          outputName(std::move(other.outputName)) {
        other.session = nullptr;
    }

    Session& operator=(Session&& other) noexcept {
        if (this != &other) {
            if (session && g_api) g_api->ReleaseSession(session);
            session = other.session;
            inputName = std::move(other.inputName);
            outputName = std::move(other.outputName);
            other.session = nullptr;
        }
        return *this;
    }
};

struct Engine {
    // Declared first so it is destroyed last: the sessions must be released
    // before the environment they were created from - the reverse order
    // leaves every inference running against a dead env.
    OrtEnv* env = nullptr;
    Session det;
    Session rec;
    std::vector<std::wstring> charset;  // index 0 = blank, last = space
    int loadedRec = -1;  // kRecModels index the running recognizer came from
    int loadedDet = -1;  // kDetModels index, same idea

    // The escalation recognizer: a heavier model held alongside the fast one
    // and run only on rows the fast one is unsure about (see recEscalate*).
    // Loaded lazily on the first escalation, never on the hot path.
    Session recHeavy;
    std::vector<std::wstring> charsetHeavy;
    int loadedRecHeavy = -1;
    bool heavyTried = false;

    ~Engine() {
        if (env && g_api) g_api->ReleaseEnv(env);
    }
};

std::unique_ptr<Engine> g_engine;
std::once_flag g_once;

// Guards the engine's recognizer state (rec/charset/loadedRec) and
// g_options. LoadRecognizer releases the previous OrtSession when it swaps
// one in, so any thread that could be inside Run on that session must be
// excluded first: Recognize holds this lock across its detect+recognize
// pipeline, which makes an in-flight inference and a hot-swap mutually
// exclusive. Concurrent Recognize calls serialize on it - they were never
// safe against hot-swaps before, and throughput still comes from ORT's own
// intra-op threading.
std::mutex g_stateMutex;

bool FillTensor(Session& s, const std::vector<int64_t>& shape,
                const std::vector<float>& data, OrtValue** out) {
    OrtAllocator* allocator = nullptr;
    ORT_CHECK(g_api->GetAllocatorWithDefaultOptions(&allocator));
    ORT_CHECK(g_api->CreateTensorAsOrtValue(
        allocator, shape.data(), static_cast<int>(shape.size()),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, out));
    float* mutableData = nullptr;
    ORT_CHECK(g_api->GetTensorMutableData(*out,
                                          reinterpret_cast<void**>(&mutableData)));
    memcpy(mutableData, data.data(), data.size() * sizeof(float));
    return true;
}

bool RunSession(Session& s, const std::vector<int64_t>& shape,
                const std::vector<float>& input, std::vector<int64_t>* outShape,
                std::vector<float>* outData) {
    OrtValue* inputTensor = nullptr;
    if (!FillTensor(s, shape, input, &inputTensor)) return false;

    const char* inputNames[] = {s.inputName.c_str()};
    const char* outputNames[] = {s.outputName.c_str()};
    OrtValue* outputTensor = nullptr;
    OrtStatus* st = g_api->Run(s.session, nullptr, inputNames,
                               const_cast<OrtValue* const*>(&inputTensor), 1,
                               outputNames, 1, &outputTensor);
    g_api->ReleaseValue(inputTensor);
    if (st) {
        const std::string msg = g_api->GetErrorMessage(st);
        g_api->ReleaseStatus(st);
        t_lastError = L"inference: " + Widen(msg);
        return false;
    }

    OrtTensorTypeAndShapeInfo* info = nullptr;
    ORT_CHECK(g_api->GetTensorTypeAndShape(outputTensor, &info));
    size_t dims = 0;
    ORT_CHECK(g_api->GetDimensionsCount(info, &dims));
    outShape->resize(dims);
    ORT_CHECK(g_api->GetDimensions(info, outShape->data(), dims));
    g_api->ReleaseTensorTypeAndShapeInfo(info);

    float* raw = nullptr;
    ORT_CHECK(g_api->GetTensorMutableData(outputTensor,
                                          reinterpret_cast<void**>(&raw)));
    int64_t count = 1;
    for (int64_t d : *outShape) count *= d;
    outData->assign(raw, raw + count);
    g_api->ReleaseValue(outputTensor);
    return true;
}

bool LoadSession(OrtEnv* env, const std::wstring& path, Session& out) {
    OrtSessionOptions* opts = nullptr;
    ORT_CHECK(g_api->CreateSessionOptions(&opts));
    // Two intra-op threads per session: recognition rows also run across a
    // small pool (see Recognize), and 2x4 stays inside a typical core count
    // where 4x4 thrashed.
    g_api->SetIntraOpNumThreads(opts, 2);
    const OrtStatus* st = g_api->CreateSession(env, path.c_str(), opts,
                                               &out.session);
    g_api->ReleaseSessionOptions(opts);
    if (st) {
        const std::string msg = g_api->GetErrorMessage(st);
        g_api->ReleaseStatus(const_cast<OrtStatus*>(st));
        t_lastError = L"loading model: " + Widen(msg);
        return false;
    }

    OrtAllocator* allocator = nullptr;
    ORT_CHECK(g_api->GetAllocatorWithDefaultOptions(&allocator));
    char* name = nullptr;
    ORT_CHECK(g_api->SessionGetInputName(out.session, 0, allocator, &name));
    out.inputName = name;
    g_api->AllocatorFree(allocator, name);
    ORT_CHECK(g_api->SessionGetOutputName(out.session, 0, allocator, &name));
    out.outputName = name;
    g_api->AllocatorFree(allocator, name);
    return true;
}

// ---------------------------------------------------------------------------
// Image plumbing (BGRA packed buffers, resized through GDI+).
// ---------------------------------------------------------------------------

struct Img {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> bgra;
};

Img FromBitmap(Bitmap* bmp) {
    Img img;
    img.w = static_cast<int>(bmp->GetWidth());
    img.h = static_cast<int>(bmp->GetHeight());
    img.bgra.resize(static_cast<size_t>(img.w) * img.h * 4);
    BitmapData data;
    const Rect full(0, 0, img.w, img.h);
    if (bmp->LockBits(&full, ImageLockModeRead, PixelFormat32bppARGB, &data) ==
        Ok) {
        for (int y = 0; y < img.h; ++y) {
            memcpy(img.bgra.data() + static_cast<size_t>(y) * img.w * 4,
                   static_cast<const uint8_t*>(data.Scan0) +
                       static_cast<size_t>(y) * data.Stride,
                   static_cast<size_t>(img.w) * 4);
        }
        bmp->UnlockBits(&data);
    }
    return img;
}

Img ResizeImg(const Img& src, int newW, int newH) {
    Img out;
    out.w = newW;
    out.h = newH;
    out.bgra.resize(static_cast<size_t>(newW) * newH * 4);

    Bitmap srcBmp(src.w, src.h, PixelFormat32bppARGB);
    {
        BitmapData srcData;
        const Rect full(0, 0, src.w, src.h);
        srcBmp.LockBits(&full, ImageLockModeWrite, PixelFormat32bppARGB,
                        &srcData);
        for (int y = 0; y < src.h; ++y) {
            memcpy(static_cast<uint8_t*>(srcData.Scan0) +
                       static_cast<size_t>(y) * srcData.Stride,
                   src.bgra.data() + static_cast<size_t>(y) * src.w * 4,
                   static_cast<size_t>(src.w) * 4);
        }
        srcBmp.UnlockBits(&srcData);
    }

    Bitmap dstBmp(newW, newH, PixelFormat32bppARGB);
    {
        Graphics g(&dstBmp);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        ImageAttributes attr;
        attr.SetWrapMode(WrapModeTileFlipXY);
        g.DrawImage(&srcBmp, Rect(0, 0, newW, newH), 0, 0, src.w, src.h,
                    UnitPixel, &attr);
    }

    BitmapData dstData;
    const Rect dstFull(0, 0, newW, newH);
    dstBmp.LockBits(&dstFull, ImageLockModeRead, PixelFormat32bppARGB,
                    &dstData);
    for (int y = 0; y < newH; ++y) {
        memcpy(out.bgra.data() + static_cast<size_t>(y) * newW * 4,
               static_cast<const uint8_t*>(dstData.Scan0) +
                   static_cast<size_t>(y) * dstData.Stride,
               static_cast<size_t>(newW) * 4);
    }
    dstBmp.UnlockBits(&dstData);
    return out;
}

// ---------------------------------------------------------------------------
// Detection (DBNet): resize short side to >= 736 (multiple of 32),
// normalize, threshold at 0.3, dilate 2x2, connected components, score >
// 0.5, unclip by area*1.6/perimeter, scale back to source pixels.
// ---------------------------------------------------------------------------

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
};

bool Detect(Engine& e, const Img& src, std::vector<Box>* boxes) {
    const int limitSide = (std::max)(256, g_options.detShortSide);
    double ratio = 1.0;
    const int minSide = (std::min)(src.w, src.h);
    if (minSide < limitSide) {
        ratio = static_cast<double>(limitSide) / minSide;
    }
    // The short-side rule above can explode an extreme-aspect strip - a
    // 1600x52 crop would land at ~22640x736, far outside anything the
    // detector saw in training and minutes of CPU for one band. Cap the
    // longer side; the reference stack bounds its input the same way.
    const int cap = (std::max)(256, g_options.detLongSideCap);
    const int longSide = (std::max)(src.w, src.h);
    if (static_cast<int>(longSide * ratio) > cap) {
        ratio = static_cast<double>(cap) / longSide;
    }
    int rw = static_cast<int>(src.w * ratio);
    int rh = static_cast<int>(src.h * ratio);
    rw = (std::max)(32, static_cast<int>(std::lround(rw / 32.0) * 32));
    rh = (std::max)(32, static_cast<int>(std::lround(rh / 32.0) * 32));

    const Img resized = ResizeImg(src, rw, rh);

    std::vector<float> input(static_cast<size_t>(3) * rw * rh);
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            const uint8_t* p =
                resized.bgra.data() + (static_cast<size_t>(y) * rw + x) * 4;
            const size_t base = static_cast<size_t>(y) * rw + x;
            const size_t plane = static_cast<size_t>(rw) * rh;
            const uint8_t blue = p[0];
            const uint8_t green = p[1];
            const uint8_t red = p[2];

            if (g_options.detNormalize == 1) {
                // PaddleOCR's DetResizeForTest/NormalizeImage: ImageNet
                // statistics applied in RGB channel order.
                input[base] = (red / 255.0f - 0.485f) / 0.229f;
                input[plane + base] = (green / 255.0f - 0.456f) / 0.224f;
                input[2 * plane + base] = (blue / 255.0f - 0.406f) / 0.225f;
            } else {
                input[base] = (red / 255.0f - 0.5f) / 0.5f;
                input[plane + base] = (green / 255.0f - 0.5f) / 0.5f;
                input[2 * plane + base] = (blue / 255.0f - 0.5f) / 0.5f;
            }
        }
    }

    std::vector<int64_t> outShape;
    std::vector<float> map;
    if (!RunSession(e.det, {1, 3, rh, rw}, input, &outShape, &map)) {
        return false;
    }
    if (outShape.size() < 3) {
        t_lastError = L"detection output has an unexpected shape";
        return false;
    }
    const int mh = static_cast<int>(outShape[outShape.size() - 2]);
    const int mw = static_cast<int>(outShape[outShape.size() - 1]);

    // Threshold, then dilate with the configured square kernel (anchored
    // bottom-right; the reference stack uses 2x2).
    std::vector<uint8_t> mask(static_cast<size_t>(mw) * mh, 0);
    const float mapThresh = g_options.detMapThresh > 0.0f
                                ? g_options.detMapThresh
                                : 0.3f;
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = map[i] > mapThresh ? 1 : 0;
    }
    std::vector<uint8_t> dilated(mask);
    if (g_options.detDilate >= 1) {
        for (int y = 1; y < mh; ++y) {
            for (int x = 1; x < mw; ++x) {
                const size_t i = static_cast<size_t>(y) * mw + x;
                dilated[i] = static_cast<uint8_t>(mask[i] | mask[i - 1] |
                                                  mask[i - mw] |
                                                  mask[i - mw - 1]);
            }
        }
    }

    // Connected components, 8-neighbour, iterative fill.
    std::vector<int> comp(static_cast<size_t>(mw) * mh, -1);
    std::vector<std::vector<int>> members;
    std::vector<int> stack;
    for (int y0 = 0; y0 < mh; ++y0) {
        for (int x0 = 0; x0 < mw; ++x0) {
            const size_t start = static_cast<size_t>(y0) * mw + x0;
            if (!dilated[start] || comp[start] >= 0) continue;
            const int id = static_cast<int>(members.size());
            members.emplace_back();
            stack.push_back(static_cast<int>(start));
            comp[start] = id;
            while (!stack.empty()) {
                const int idx = stack.back();
                stack.pop_back();
                members[id].push_back(idx);
                const int cy = idx / mw;
                const int cx = idx % mw;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int ny = cy + dy;
                        const int nx = cx + dx;
                        if (ny < 0 || ny >= mh || nx < 0 || nx >= mw) continue;
                        const size_t ni = static_cast<size_t>(ny) * mw + nx;
                        if (dilated[ni] && comp[ni] < 0) {
                            comp[ni] = id;
                            stack.push_back(static_cast<int>(ni));
                        }
                    }
                }
            }
        }
    }

    for (const auto& px : members) {
        int xmin = mw, xmax = 0, ymin = mh, ymax = 0;
        double scoreSum = 0;
        for (int idx : px) {
            const int x = idx % mw;
            const int y = idx / mw;
            xmin = (std::min)(xmin, x);
            xmax = (std::max)(xmax, x);
            ymin = (std::min)(ymin, y);
            ymax = (std::max)(ymax, y);
            scoreSum += map[idx];
        }
        const float score = static_cast<float>(scoreSum / px.size());
        if (score < g_options.detBoxThresh) continue;

        const int bw = xmax - xmin + 1;
        const int bh = ymax - ymin + 1;
        // Reject on size BEFORE or AFTER the unclip below. PaddleOCR's
        // DBPostProcess.boxes_from_bitmap does it after - it unclips the raw
        // contour and only then drops boxes whose short side is under
        // min_size + 2 - so a stroke two pixels wide is given its expansion
        // before being judged. Rejecting first is strictly less permissive
        // for exactly the class that dominates the remaining errors here:
        // "|", ":", backtick, "]" and the other thin glyphs.
        if (!g_options.detMinSizeAfterUnclip && (bw < 3 || bh < 3)) continue;

        const double area = static_cast<double>(bw) * bh;
        const double perimeter = 2.0 * (bw + bh);
        const int d = static_cast<int>(
            std::lround(area * g_options.detUnclipRatio / perimeter));

        Box box;
        box.x = static_cast<int>(std::lround(
            (std::max)(0, xmin - d) / static_cast<double>(mw) * src.w));
        box.y = static_cast<int>(std::lround(
            (std::max)(0, ymin - d) / static_cast<double>(mh) * src.h));
        box.w =
            static_cast<int>(std::lround(
                (std::min)(mw, xmax + d + 1) / static_cast<double>(mw) *
                    src.w)) -
            box.x;
        box.h =
            static_cast<int>(std::lround(
                (std::min)(mh, ymax + d + 1) / static_cast<double>(mh) *
                    src.h)) -
            box.y;
        // The reference's post-unclip gate is min_size + 2 = 5 on the
        // SHORT side of the expanded box, which is what the 5s below already
        // express in source-pixel space.
        if (g_options.detMinSizeAfterUnclip) {
            const int ubw = (std::min)(mw, xmax + d + 1) - (std::max)(0, xmin - d);
            const int ubh = (std::min)(mh, ymax + d + 1) - (std::max)(0, ymin - d);
            if ((std::min)(ubw, ubh) < 5) continue;
        }
        if (box.w >= 5 && box.h >= 5 && box.x >= 0 && box.y >= 0 &&
            box.x + box.w <= src.w && box.y + box.h <= src.h) {
            boxes->push_back(box);
        }
    }

    std::sort(boxes->begin(), boxes->end(), [](const Box& a, const Box& b) {
        return a.y < b.y;
    });
    return true;
}

// ---------------------------------------------------------------------------
// Recognition (SVTR + CTC): crop each box, resize to 48 rows, normalize,
// right-pad to >= 320 columns, argmax per timestep, collapse repeats, drop
// the blank class, split words on the timeline.
// ---------------------------------------------------------------------------

struct Line {
    std::wstring text;
    float conf = 0;
    Box box;
    std::vector<OcrSelection::CharCol> chars;
    // Source pixels per recognition timeline column - the mapping that
    // turns a decoded character's timestep into its place in the crop.
    // Computed against the FULL timeline (including blank steps and the
    // right-padding every crop carries), not against the decoded span:
    // dividing by the decoded span stretches word boxes toward the crop's
    // right edge and drags selection highlights off their words.
    double pixelsPerColumn = 0;
};

// Direct bilinear resampling from src BGRA crop into normalized RGB planar float tensor.
// Eliminates GDI+ Bitmap/Graphics allocations and multi-threaded lock contention.
void SampleCropBilinear(const Img& src, const Box& box, int resizedW, int targetW, int targetH,
                        std::vector<float>& outTensor) {
    outTensor.assign(static_cast<size_t>(3) * targetH * targetW, 0.0f);
    const size_t plane = static_cast<size_t>(targetH) * targetW;

    const double scaleX = static_cast<double>(box.w) / resizedW;
    const double scaleY = static_cast<double>(box.h) / targetH;

    for (int y = 0; y < targetH; ++y) {
        // Pixel center mapping with edge clamping
        const double srcY =
            (std::max)(0.0, (std::min)(static_cast<double>(box.h - 1),
                                       (y + 0.5) * scaleY - 0.5));
        const int y0 = static_cast<int>(srcY);
        const int y1 = (std::min)(y0 + 1, box.h - 1);
        const float fy = static_cast<float>(srcY - y0);
        const float invFy = 1.0f - fy;

        const size_t row0Offset =
            (static_cast<size_t>(box.y + y0) * src.w + box.x) * 4;
        const size_t row1Offset =
            (static_cast<size_t>(box.y + y1) * src.w + box.x) * 4;
        const uint8_t* row0 = src.bgra.data() + row0Offset;
        const uint8_t* row1 = src.bgra.data() + row1Offset;

        const size_t rowBase = static_cast<size_t>(y) * targetW;

        for (int x = 0; x < resizedW; ++x) {
            const double srcX =
                (std::max)(0.0, (std::min)(static_cast<double>(box.w - 1),
                                           (x + 0.5) * scaleX - 0.5));
            const int x0 = static_cast<int>(srcX);
            const int x1 = (std::min)(x0 + 1, box.w - 1);
            const float fx = static_cast<float>(srcX - x0);
            const float invFx = 1.0f - fx;

            const float w00 = invFx * invFy;
            const float w10 = fx * invFy;
            const float w01 = invFx * fy;
            const float w11 = fx * fy;

            const uint8_t* p00 = row0 + x0 * 4;
            const uint8_t* p10 = row0 + x1 * 4;
            const uint8_t* p01 = row1 + x0 * 4;
            const uint8_t* p11 = row1 + x1 * 4;

            // BGRA: p[0] is Blue, p[1] is Green, p[2] is Red
            const float b =
                w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0];
            const float g =
                w00 * p00[1] + w10 * p10[1] + w01 * p01[1] + w11 * p11[1];
            const float r =
                w00 * p00[2] + w10 * p10[2] + w01 * p01[2] + w11 * p11[2];

            // SVTR [-1, 1] normalization: (val / 255.0 - 0.5) / 0.5 = val / 127.5 - 1.0
            const size_t base = rowBase + x;
            outTensor[base] = r / 127.5f - 1.0f;
            outTensor[plane + base] = g / 127.5f - 1.0f;
            outTensor[2 * plane + base] = b / 127.5f - 1.0f;
        }
    }
}

bool RecognizeOne(Engine& e, const Img& src, const Box& box, Line* line,
                  bool heavy = false) {
    Session& recSession = heavy ? e.recHeavy : e.rec;
    const std::vector<std::wstring>& charset =
        heavy ? e.charsetHeavy : e.charset;
    line->box = box;

    // Defence in depth for the crop below: a degenerate or out-of-range box
    // must never turn into an out-of-bounds read.
    if (box.w <= 0 || box.h <= 0 || box.x < 0 || box.y < 0 ||
        box.x + box.w > src.w || box.y + box.h > src.h) {
        return false;
    }

    constexpr int kImgH = 48;
    const double aspect = static_cast<double>(box.w) / (std::max)(1, box.h);
    const int naturalW = static_cast<int>(std::ceil(kImgH * aspect));
    // 0 = unconstrained natural width (with an 8192 px memory guard).
    const int maxAllowed =
        g_options.recMaxWidth > 0 ? g_options.recMaxWidth : 8192;
    const int resizedW = (std::max)(1, (std::min)(maxAllowed, naturalW));
    const int targetW = (std::max)(320, resizedW);

    std::vector<float> input;
    SampleCropBilinear(src, box, resizedW, targetW, kImgH, input);

    std::vector<int64_t> outShape;
    std::vector<float> preds;
    if (!RunSession(recSession, {1, 3, kImgH, targetW}, input, &outShape,
                    &preds)) {
        return false;
    }
    if (outShape.size() < 3) {
        t_lastError = L"recognition output has an unexpected shape";
        return false;
    }
    const int T = static_cast<int>(outShape[outShape.size() - 2]);
    const int C = static_cast<int>(outShape[outShape.size() - 1]);

    // The crop was resized to kImgH rows and right-padded to targetW
    // columns; the timeline's T steps span that whole padded input. One
    // step therefore covers (targetW / T) input columns, each of which is
    // (box.w / resizedW) source pixels - and the content sits in the first
    // resizedW of those columns. The product maps a timestep straight into
    // source pixels within the crop.
    line->pixelsPerColumn =
        static_cast<double>(box.w) * targetW /
        (static_cast<double>(resizedW) * static_cast<double>(T));

    // CTC decoding with peak activation pooling: track each character run
    // across timesteps and record the peak probability and peak center column.
    int currentId = 0;
    float peakProb = 0.0f;
    int peakCol = -1;
    double confSum = 0;

    auto emitRun = [&](int id, float prob, int col) {
        if (id > 0 && id < static_cast<int>(charset.size())) {
            OcrSelection::CharCol cc;
            cc.c = charset[id];
            // Fold full-width forms as they are decoded, so word text,
            // space detection and the CJK test all see ordinary ASCII.
            if (cc.c.size() == 1) {
                cc.c[0] = OcrSelection::FoldFullWidthChar(cc.c[0]);
            }
            cc.col = col;
            line->chars.push_back(std::move(cc));
            confSum += prob;
        }
    };

    for (int t = 0; t < T; ++t) {
        const float* row = preds.data() + static_cast<size_t>(t) * C;
        int best = 0;
        float bestP = row[0];
        for (int c = 1; c < C; ++c) {
            if (row[c] > bestP) {
                bestP = row[c];
                best = c;
            }
        }

        if (best != currentId) {
            // The previous character run ended. Emit it with its peak probability and peak column.
            if (currentId != 0) {
                emitRun(currentId, peakProb, peakCol);
            }
            currentId = best;
            peakProb = bestP;
            peakCol = t;
        } else if (currentId != 0) {
            // Contiguous same character: update peak activation and column.
            if (bestP > peakProb) {
                peakProb = bestP;
                peakCol = t;
            }
        }
    }

    // Flush any pending trailing character run.
    if (currentId != 0) {
        emitRun(currentId, peakProb, peakCol);
    }

    for (const OcrSelection::CharCol& cc : line->chars) line->text += cc.c;
    line->conf =
        line->chars.empty() ? 0.0f : static_cast<float>(confSum / line->chars.size());
    return true;
}

std::wstring ExeRelative(const std::wstring& relative) {
    return AppPaths::Combine(AppPaths::GetExecutableFolder(), relative);
}

// Models live beside the exe (deployed builds) or back in third_party/ (the
// dev tree). First hit wins.
std::wstring ResolveModel(const wchar_t* name) {
    const std::wstring dirs[] = {
        ExeRelative(L"models\\ppocr\\"),
        ExeRelative(L"..\\..\\third_party\\ppocr\\"),
    };
    for (const std::wstring& dir : dirs) {
        const std::wstring path = dir + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    return {};
}

// Recognition models, strongest-first: the v6 heads (a larger vocabulary
// and materially better printed-text accuracy than v5), then the v5 mobile
// conversions, then the two v4 models this app started with.
// Options::recModel indexes this table.
constexpr const wchar_t* kRecModels[] = {
    L"PP-OCRv6_rec_medium.onnx",
    L"PP-OCRv6_rec_small.onnx",
    L"ch_PP-OCRv5_rec_mobile.onnx",
    L"en_PP-OCRv5_rec_mobile.onnx",
    L"en_PP-OCRv4_rec_mobile.onnx",
    L"ch_PP-OCRv4_rec_infer.onnx",
};
constexpr int kRecModelCount =
    static_cast<int>(sizeof(kRecModels) / sizeof(kRecModels[0]));
// Detection models in preference order; Options::detModel indexes this.
// DBNet postprocessing is identical across all of them.
constexpr const wchar_t* kDetModels[] = {
    L"PP-OCRv6_det_small.onnx",
    L"PP-OCRv6_det_medium.onnx",
    L"ch_PP-OCRv5_det_mobile.onnx",
    L"ch_PP-OCRv4_det_infer.onnx",
};
constexpr int kDetModelCount =
    static_cast<int>(sizeof(kDetModels) / sizeof(kDetModels[0]));

// The kRecModels slot a resolved path came from, for later hot-swaps.
int ResolveRecIndex(const std::wstring& path) {
    for (int i = 0; i < kRecModelCount; ++i) {
        const std::wstring name = kRecModels[i];
        if (path.size() >= name.size() &&
            path.compare(path.size() - name.size(), name.size(), name) == 0) {
            return i;
        }
    }
    return -1;
}

// Same idea against kDetModels.
int ResolveDetIndex(const std::wstring& path) {
    for (int i = 0; i < kDetModelCount; ++i) {
        const std::wstring name = kDetModels[i];
        if (path.size() >= name.size() &&
            path.compare(path.size() - name.size(), name.size(), name) == 0) {
            return i;
        }
    }
    return -1;
}

// Loads a detection session. Det models carry no charset, so this is just
// the session swap with its own bookkeeping. Lock discipline matches
// LoadRecognizer.
bool LoadDetector(Engine& e, const std::wstring& path, int modelIndex) {
    Session fresh;
    if (!LoadSession(e.env, path, fresh)) return false;
    e.det = std::move(fresh);
    e.loadedDet = modelIndex;
    Logger::Infof(L"PP-OCR: detector %s",
                  path.substr(path.find_last_of(L'\\') + 1).c_str());
    return true;
}

// Loads the recognition model `path` and its charset into `engine`. Called
// again when Options.recModel changes after startup (the benchmark sweep is
// the only caller that does this; the app itself never does).
//
// Lock discipline: callers must hold g_stateMutex - except during startup,
// where the engine is still a local that no other thread can reach. The
// swap at the bottom releases the previous OrtSession, so this must never
// run while another thread could be calling Run on it.
bool LoadRecognizer(Engine& e, const std::wstring& path, int modelIndex,
                    bool heavy = false) {
    Session fresh;
    if (!LoadSession(e.env, path, fresh)) return false;

    OrtModelMetadata* meta = nullptr;
    ORT_CHECK(g_api->SessionGetModelMetadata(fresh.session, &meta));
    OrtAllocator* allocator = nullptr;
    ORT_CHECK(g_api->GetAllocatorWithDefaultOptions(&allocator));
    char* raw = nullptr;
    ORT_CHECK(g_api->ModelMetadataLookupCustomMetadataMap(meta, allocator,
                                                          "character", &raw));
    g_api->ReleaseModelMetadata(meta);
    if (!raw) {
        t_lastError = L"the recognition model has no character table";
        return false;
    }
    const std::string blob(raw);
    g_api->AllocatorFree(allocator, raw);

    std::vector<std::wstring> charset;
    size_t start = 0;
    while (start <= blob.size()) {
        size_t end = blob.find('\n', start);
        if (end == std::string::npos) end = blob.size();
        charset.push_back(Widen(blob.substr(start, end - start)));
        if (end == blob.size()) break;
        start = end + 1;
    }
    // CTCLabelDecode's two special entries: blank at 0, space at the end.
    charset.push_back(L" ");
    charset.insert(charset.begin(), L"blank");

    // Only now swap the live session: a failed load must not leave the
    // engine half-replaced.
    if (heavy) {
        e.recHeavy = std::move(fresh);
        e.charsetHeavy = std::move(charset);
        e.loadedRecHeavy = modelIndex;
    } else {
        e.rec = std::move(fresh);
        e.charset = std::move(charset);
        e.loadedRec = modelIndex;
    }
    Logger::Infof(L"PP-OCR: %s recognizer %s (%u classes)",
                  heavy ? L"escalation" : L"primary",
                  path.substr(path.find_last_of(L'\\') + 1).c_str(),
                  static_cast<unsigned>(
                      (heavy ? e.charsetHeavy : e.charset).size()));
    return true;
}

bool EngineReady() {
    // The runtime DLL must load by name from beside the exe; the models sit
    // in third_party/ in the dev tree and in a models\ folder next to the
    // exe in a deployed build.
    HMODULE ort = LoadLibraryW(L"onnxruntime.dll");
    if (!ort) {
        t_lastError = L"onnxruntime.dll was not found next to the executable";
        return false;
    }
    const OrtApiBase* (*getBase)() = reinterpret_cast<const OrtApiBase* (*)()>(
        GetProcAddress(ort, "OrtGetApiBase"));
    if (!getBase || !(g_api = getBase()->GetApi(ORT_API_VERSION))) {
        t_lastError = L"onnxruntime.dll does not provide the expected API";
        return false;
    }

    OrtEnv* env = nullptr;
    ORT_CHECK(g_api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "ScreenshotApp", &env));
    // Ownership moves to the Engine below; the env must outlive the
    // sessions, which keep using it for every inference.

    std::wstring detPath;
    if (g_options.detModel >= 0 && g_options.detModel < kDetModelCount) {
        detPath = ResolveModel(kDetModels[g_options.detModel]);
    }
    if (detPath.empty()) {
        for (const wchar_t* name : kDetModels) {
            detPath = ResolveModel(name);
            if (!detPath.empty()) break;
        }
    }
    std::wstring recPath;
    if (g_options.recModel >= 0 && g_options.recModel < kRecModelCount) {
        recPath = ResolveModel(kRecModels[g_options.recModel]);
    }
    if (recPath.empty()) {
        for (const wchar_t* name : kRecModels) {
            recPath = ResolveModel(name);
            if (!recPath.empty()) break;
        }
    }
    if (detPath.empty() || recPath.empty()) {
        t_lastError = L"the PP-OCR models were not found";
        g_api->ReleaseEnv(env);
        return false;
    }
    auto engine = std::make_unique<Engine>();
    engine->env = env;
    if (!LoadDetector(*engine, detPath, ResolveDetIndex(detPath))) {
        return false;
    }
    if (!LoadRecognizer(*engine, recPath, ResolveRecIndex(recPath))) {
        return false;
    }

    g_engine = std::move(engine);
    return true;
}

}  // namespace

bool EnsureLoaded(std::wstring* error) {
    std::call_once(g_once, [] {
        if (!EngineReady()) {
            // EngineReady ran - and failed - on THIS thread, so the
            // thread-local holds the message; mirror it for readers on
            // other threads, whose own copy is empty.
            const std::lock_guard<std::mutex> guard(g_errorMutex);
            g_startupError = t_lastError;
            Logger::Warnf(L"PP-OCR unavailable (%s) - the Windows OCR engine "
                          L"will be used instead",
                          t_lastError.c_str());
        } else {
            Logger::Info(L"PP-OCR engine loaded");
        }
    });
    if (!g_engine) {
        // Another thread may have run the startup: its failure lives in the
        // mirrored copy, not this thread's thread-local.
        const std::lock_guard<std::mutex> guard(g_errorMutex);
        if (error) *error = g_startupError;
    } else if (error) {
        error->clear();
    }
    return g_engine != nullptr;
}

// Crops a band out of a packed BGRA buffer, horizontally or vertically.
Img CropImg(const Img& src, bool scanY, int offset, int length) {
    Img out;
    if (scanY) {
        out.w = src.w;
        out.h = length;
        out.bgra.resize(static_cast<size_t>(out.w) * out.h * 4);
        for (int y = 0; y < length; ++y) {
            memcpy(out.bgra.data() + static_cast<size_t>(y) * out.w * 4,
                   src.bgra.data() +
                       static_cast<size_t>(offset + y) * src.w * 4,
                   static_cast<size_t>(out.w) * 4);
        }
    } else {
        out.w = length;
        out.h = src.h;
        out.bgra.resize(static_cast<size_t>(out.w) * out.h * 4);
        for (int y = 0; y < src.h; ++y) {
            memcpy(out.bgra.data() + static_cast<size_t>(y) * out.w * 4,
                   src.bgra.data() +
                       (static_cast<size_t>(y) * src.w + offset) * 4,
                   static_cast<size_t>(out.w) * 4);
        }
    }
    return out;
}

Result Recognize(Bitmap* bmp, const std::atomic<bool>* cancelled) {
    Result result;
    std::wstring error;
    if (!EnsureLoaded(&error)) {
        result.error = error;
        return result;
    }
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        result.error = L"cancelled";
        return result;
    }

    // One lock for the whole pipeline: detection and recognition run on the
    // engine's sessions, and the hot-swap below releases one of them, so
    // nothing may Run on a session while another thread could swap it out.
    // EnsureLoaded stays outside - call_once already serializes startup.
    const std::lock_guard<std::mutex> guard(g_stateMutex);

    // The benchmark sweep changes Options.recModel/detModel between runs;
    // honour that by hot-swapping whichever model no longer matches what is
    // loaded (the app itself never does this). A failed reload keeps the
    // currently loaded model - the request falls back to whatever is live
    // rather than dropping recognition entirely.
    if (g_engine) {
        if (g_options.recModel >= 0 && g_options.recModel < kRecModelCount &&
            g_options.recModel != g_engine->loadedRec) {
            const std::wstring path =
                ResolveModel(kRecModels[g_options.recModel]);
            if (!path.empty()) {
                LoadRecognizer(*g_engine, path, g_options.recModel);
            }
        }
        if (g_options.detModel >= 0 && g_options.detModel < kDetModelCount &&
            g_options.detModel != g_engine->loadedDet) {
            const std::wstring path =
                ResolveModel(kDetModels[g_options.detModel]);
            if (!path.empty()) {
                LoadDetector(*g_engine, path, g_options.detModel);
            }
        }
    }

    if (!bmp || bmp->GetLastStatus() != Ok) {
        result.error = L"No image to read.";
        return result;
    }

    Img img = FromBitmap(bmp);
    if (img.w <= 0 || img.h <= 0) {
        result.error = L"The region has no pixels.";
        return result;
    }

    if (g_options.normalizePixels) {
        bool inverted = false;
        OcrNormalize::Apply(img.bgra, &inverted);
        if (inverted) {
            Logger::Debug(L"PP-OCR: dark background detected - normalized "
                          L"for recognition");
        }
    }

    const int srcW = static_cast<int>(bmp->GetWidth());
    const int srcH = static_cast<int>(bmp->GetHeight());

    // Regions longer than the detector's input regime are recognized in
    // overlapping bands along their long axis - the same tiling the Windows
    // engine uses, and what keeps a tall scroll capture's every pixel inside
    // the trained distribution instead of stretching the detector across
    // thousands of rows at once. A fitting region is one band covering all.
    const bool scanY = img.h >= img.w;
    const int longSide = scanY ? img.h : img.w;
    const int cap = (std::max)(256, g_options.detLongSideCap);
    const int limitSide = (std::max)(256, g_options.detShortSide);
    const std::vector<OcrSelection::RecognitionBand> plan =
        OcrSelection::PlanRecognitionBands(longSide, cap, 256);
    if (plan.size() > 1) {
        Logger::Infof(L"PP-OCR: recognizing in %u bands along %s",
                      static_cast<unsigned>(plan.size()),
                      scanY ? L"height" : L"width");
    }

    std::vector<Box> boxes;
    LARGE_INTEGER perfFreq, tDet0, tDet1, tRec0, tRec1;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&tDet0);
    for (const OcrSelection::RecognitionBand& band : plan) {
        if (band.coreStart >= band.coreEnd) continue;
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            result.error = L"cancelled";
            return result;
        }

        Img sub = CropImg(img, scanY, band.offset, band.height);

        // Small bands get the pre-upscale the detector benefits from -
        // per band, so a tall capture never holds an upscaled monster.
        // Skipped when the factor would cross the detector's own long-side
        // cap: Detect would shrink straight back down (its short-side rule
        // is clamped by the same cap), so the resample would be pure waste
        // plus one avoidable blur cycle.
        double upscale = 1.0;
        const float factor = (std::max)(1.0f, g_options.bandPreUpscale);
        // The size gate is a floor already satisfied on most real captures,
        // which is why the short-side sweep read as a flat plateau: on a
        // 821x654 window the knob was inert. detAlwaysMagnify drops it, so
        // magnification depends on glyph-independent geometry alone (the
        // long-side cap) rather than on how tall the window happened to be.
        const bool sizeGate =
            g_options.detAlwaysMagnify || (std::min)(sub.w, sub.h) < limitSide;
        if (factor > 1.0f && sizeGate &&
            static_cast<double>((std::max)(sub.w, sub.h)) * factor <= cap) {
            const int upW = static_cast<int>(std::lround(sub.w * factor));
            const int upH = static_cast<int>(std::lround(sub.h * factor));
            if (upW > 0 && upH > 0) {
                sub = ResizeImg(sub, upW, upH);
                upscale = factor;
            }
        }

        std::vector<Box> bandBoxes;
        if (!Detect(*g_engine, sub, &bandBoxes)) {
            result.error = t_lastError;
            Logger::Errorf(L"PP-OCR detection failed (%s)", t_lastError.c_str());
            return result;
        }

        for (Box& b : bandBoxes) {
            // Detect answered in the (possibly upscaled) BAND-LOCAL space:
            // first undo the upscale, then translate by the band offset into
            // working-image coordinates. Skipping the translation shoves
            // every later band's boxes down to its top edge, where their
            // centres land in this band's own seam guard and the ownership
            // test silently eats them - which is exactly a tall capture
            // losing all but its first band of rows.
            b.x = static_cast<int>(std::lround(b.x / upscale));
            b.y = static_cast<int>(std::lround(b.y / upscale));
            b.w = (std::max)(1, static_cast<int>(std::lround(b.w / upscale)));
            b.h = (std::max)(1, static_cast<int>(std::lround(b.h / upscale)));
            if (scanY) {
                b.y += band.offset;
            } else {
                b.x += band.offset;
            }
            // Rounding through the upscale can push an edge one pixel out
            // of the image; clamp before anything crops with these numbers.
            b.x = (std::max)(0, (std::min)(b.x, img.w - 1));
            b.y = (std::max)(0, (std::min)(b.y, img.h - 1));
            b.w = (std::max)(1, (std::min)(b.w, img.w - b.x));
            b.h = (std::max)(1, (std::min)(b.h, img.h - b.y));
            const int centre = scanY ? b.y + b.h / 2 : b.x + b.w / 2;
            // A line whose centre lies in this band's core is entirely
            // inside this band's pixels - the overlap is many times a line
            // height - so its intact copy belongs here and the neighbouring
            // band's duplicate drops out on the same test.
            if (!OcrSelection::BandOwnsCenter(band, centre)) continue;
            if (g_options.debugDump) {
                Logger::Debugf(L"DBG det box %d,%d %dx%d", b.x, b.y, b.w, b.h);
            }
            boxes.push_back(b);
        }
    }

    QueryPerformanceCounter(&tDet1);
    result.detectMs = 1000.0 * static_cast<double>(tDet1.QuadPart - tDet0.QuadPart) /
                      static_cast<double>(perfFreq.QuadPart);

    // One visual line sometimes arrives as side-by-side fragments; merge
    // them so reading order survives.
    std::vector<OcrSelection::TextRect> rowRects;
    for (const Box& b : boxes) {
        rowRects.push_back({b.x, b.y, b.w, b.h});
    }
    rowRects = OcrSelection::MergeRowBoxes(std::move(rowRects));
    if (g_options.debugDump) {
        for (const OcrSelection::TextRect& r : rowRects) {
            Logger::Debugf(L"DBG row %d,%d %dx%d", r.x, r.y, r.width, r.height);
        }
    }

    // One recognition call per merged row. The rows are independent - each
    // crops its own pixels from `img` - so they run across a small pool:
    // a tall scroll capture produces hundreds of rows, and serially that is
    // minutes of wall time for what a few threads do in seconds. ORT
    // sessions accept concurrent Run calls; intra-op threading was lowered
    // to match so the pool does not thrash the machine.
    struct RowResult {
        Line line;
        bool ok = false;
        std::wstring error;  // this row's thread's own message, harvested
                             // after join - pool threads must not write any
                             // shared std::string concurrently
    };
    std::vector<RowResult> rows(rowRects.size());
    QueryPerformanceCounter(&tRec0);
    {
        std::atomic<size_t> next{0};
        unsigned workers =
            std::thread::hardware_concurrency();
        workers = (std::max)(2u, (std::min)(4u, workers / 2));
        auto worker = [&] {
            for (;;) {
                if (cancelled &&
                    cancelled->load(std::memory_order_relaxed)) {
                    return;
                }
                const size_t i = next.fetch_add(1);
                if (i >= rowRects.size()) return;
                Box box{rowRects[i].x, rowRects[i].y, rowRects[i].width,
                        rowRects[i].height};
                rows[i].ok = RecognizeOne(*g_engine, img, box, &rows[i].line);
                if (!rows[i].ok) rows[i].error = t_lastError;
            }
        };
        std::vector<std::thread> pool;
        for (unsigned w = 1; w < workers; ++w) pool.emplace_back(worker);
        worker();
        for (std::thread& t : pool) t.join();
    }

    // Escalation. The fast recognizer's remaining errors are thin glyphs read
    // as digits ("/" -> "1", "]" -> "1"), dropped braces and O/0 confusions -
    // all recognition, none of them detection, so the only lever left is
    // model capacity. Running the heavy model over EVERYTHING was measured at
    // 97.13% and 10.4 s and is not even uniformly better: it breaks captures
    // the small model reads perfectly. So it runs only where the fast model
    // is unsure, and its answer is taken only when it is MORE confident than
    // the answer it would replace - which is what keeps the cases the small
    // model already gets right from regressing.
    if (g_options.recEscalateBelow > 0.0f && !rowRects.empty()) {
        std::vector<size_t> weak;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].ok && !rows[i].line.text.empty() &&
                rows[i].line.conf < g_options.recEscalateBelow) {
                weak.push_back(i);
            }
        }
        // Weakest first, so a row cap spends its budget where it buys most.
        std::sort(weak.begin(), weak.end(), [&](size_t a, size_t b) {
            return rows[a].line.conf < rows[b].line.conf;
        });
        const int cap = g_options.recEscalateMaxRows;
        if (cap > 0 && weak.size() > static_cast<size_t>(cap)) {
            weak.resize(static_cast<size_t>(cap));
        }

        if (!weak.empty() && g_engine->loadedRecHeavy < 0 &&
            !g_engine->heavyTried) {
            g_engine->heavyTried = true;  // one attempt, never per call
            const int want = g_options.recEscalateModel;
            if (want >= 0 && want < kRecModelCount) {
                const std::wstring path = ResolveModel(kRecModels[want]);
                if (!path.empty()) {
                    LoadRecognizer(*g_engine, path, want, /*heavy=*/true);
                }
            }
            if (g_engine->loadedRecHeavy < 0) {
                Logger::Warn(L"PP-OCR: escalation model unavailable");
            }
        }

        if (g_engine->loadedRecHeavy >= 0) {
            std::atomic<size_t> next{0};
            unsigned workers = std::thread::hardware_concurrency();
            workers = (std::max)(2u, (std::min)(4u, workers / 2));
            auto worker = [&] {
                for (;;) {
                    if (cancelled &&
                        cancelled->load(std::memory_order_relaxed)) {
                        return;
                    }
                    const size_t k = next.fetch_add(1);
                    if (k >= weak.size()) return;
                    const size_t i = weak[k];
                    Box box{rowRects[i].x, rowRects[i].y, rowRects[i].width,
                            rowRects[i].height};
                    Line better;
                    if (RecognizeOne(*g_engine, img, box, &better,
                                     /*heavy=*/true) &&
                        !better.text.empty() &&
                        better.conf > rows[i].line.conf) {
                        rows[i].line = std::move(better);
                    }
                }
            };
            std::vector<std::thread> pool;
            for (unsigned w = 1; w < workers; ++w) pool.emplace_back(worker);
            worker();
            for (std::thread& t : pool) t.join();
            Logger::Infof(L"PP-OCR: escalated %u of %u rows",
                          static_cast<unsigned>(weak.size()),
                          static_cast<unsigned>(rows.size()));
        }
    }

    bool hardError = false;
    QueryPerformanceCounter(&tRec1);
    result.recognizeMs = 1000.0 * static_cast<double>(tRec1.QuadPart - tRec0.QuadPart) /
                         static_cast<double>(perfFreq.QuadPart);
    for (size_t i = 0; i < rowRects.size(); ++i) {
        if (!rows[i].ok) {
            hardError = true;
            result.error = rows[i].error;
            break;
        }
        const Line& line = rows[i].line;
        if (g_options.debugDump) {
            std::wstring cols;
            for (const OcrSelection::CharCol& cc : line.chars) {
                if (!cols.empty()) cols += L' ';
                cols += cc.c + L"@" + std::to_wstring(cc.col);
            }
            Logger::Debugf(L"DBG line '%s' conf=%.2f cols[%u]=%s",
                           line.text.c_str(), line.conf,
                           static_cast<unsigned>(line.chars.size()),
                           cols.c_str());
        }
        if (line.text.empty() || line.conf < g_options.lineConfGate) continue;

        for (OcrSelection::WordBox& word : OcrSelection::WordsFromLine(
                 line.chars, rowRects[i], line.pixelsPerColumn)) {
            // Word geometry is already in this bitmap's pixel space; the
            // clamp keeps rounding from pushing an edge out of range.
            word.rect.x = (std::max)(0, (std::min)(word.rect.x, srcW));
            word.rect.y = (std::max)(0, (std::min)(word.rect.y, srcH));
            word.rect.width =
                (std::max)(0, (std::min)(word.rect.width, srcW - word.rect.x));
            word.rect.height =
                (std::max)(0, (std::min)(word.rect.height,
                                         srcH - word.rect.y));
            if (!word.text.empty()) result.words.push_back(word);
        }
    }
    if (hardError) {
        Logger::Errorf(L"PP-OCR recognition failed (%s)",
                       result.error.c_str());
        return result;
    }
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        result.error = L"cancelled";
        return result;
    }

    if (result.words.empty()) {
        Logger::Info(L"PP-OCR: no text found");
    } else {
        Logger::Infof(L"PP-OCR: %u words in %u lines",
                      static_cast<unsigned>(result.words.size()),
                      static_cast<unsigned>(rowRects.size()));
    }
    result.ok = true;
    return result;
}

void SetOptions(const Options& options) {
    const std::lock_guard<std::mutex> guard(g_stateMutex);
    g_options = options;
}

Options GetOptions() {
    const std::lock_guard<std::mutex> guard(g_stateMutex);
    return g_options;
}

LoadedModels GetLoadedModels() {
    const std::lock_guard<std::mutex> guard(g_stateMutex);
    LoadedModels loaded;
    if (g_engine) {
        loaded.rec = g_engine->loadedRec;
        loaded.det = g_engine->loadedDet;
    }
    return loaded;
}

}  // namespace PpOcr

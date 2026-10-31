// Glass-side probe for stream verification. Captures a viewer window with
// Windows.Graphics.Capture, reads the test-pattern frame counter out of the
// composed pixels, and logs each composition's SystemRelativeTime (QPC, 100 ns).
//
// parties_stream_probe --hwnd 0x1234 --rect X,Y,W,H --seconds 20 --out probe.jsonl
//                      [--pattern 1280x720] [--snapshot frame.bmp]
// --rect is the video element's content box in client pixels.

#include <client/test_pattern.h>

#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    HWND hwnd = nullptr;
    double rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;
    uint32_t pattern_w = 1280, pattern_h = 720;
    double seconds = 20;
    std::string out;
    std::string snapshot;
};

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "stream_probe: %s\n", message);
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!value) fail(("missing value for " + arg).c_str());
        if (arg == "--hwnd") o.hwnd = reinterpret_cast<HWND>(std::strtoull(value, nullptr, 0));
        else if (arg == "--rect") {
            if (sscanf_s(value, "%lf,%lf,%lf,%lf", &o.rect_x, &o.rect_y, &o.rect_w, &o.rect_h) != 4)
                fail("--rect must be X,Y,W,H");
        } else if (arg == "--pattern") {
            if (sscanf_s(value, "%ux%u", &o.pattern_w, &o.pattern_h) != 2) fail("--pattern must be WxH");
        } else if (arg == "--seconds") o.seconds = std::atof(value);
        else if (arg == "--out") o.out = value;
        else if (arg == "--snapshot") o.snapshot = value;
        else fail(("unknown argument " + arg).c_str());
        ++i;
    }
    if (!o.hwnd || !IsWindow(o.hwnd)) fail("--hwnd must name a live window");
    if (o.rect_w <= 0 || o.rect_h <= 0) fail("--rect needs a positive size");
    if (o.out.empty()) fail("--out is required");
    return o;
}

bool save_bmp(const std::string& path, const uint8_t* bgra, size_t pitch, uint32_t w, uint32_t h) {
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || !file) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = -static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = w * h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    std::fwrite(&fh, sizeof(fh), 1, file);
    std::fwrite(&ih, sizeof(ih), 1, file);
    for (uint32_t y = 0; y < h; ++y) std::fwrite(bgra + y * pitch, 1, static_cast<size_t>(w) * 4, file);
    std::fclose(file);
    return true;
}

}

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    init_apartment(apartment_type::multi_threaded);
    if (!GraphicsCaptureSession::IsSupported()) fail("Windows.Graphics.Capture is not supported");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
        fail("D3D11 device creation failed");
    ComPtr<IDXGIDevice> dxgi_device;
    device.As(&dxgi_device);
    com_ptr<::IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put()));
    const auto winrt_device = inspectable.as<IDirect3DDevice>();

    const auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    check_hresult(interop->CreateForWindow(options.hwnd, guid_of<GraphicsCaptureItem>(), put_abi(item)));

    // The capture surface covers the window's extended frame bounds; the
    // video rectangle is given in client coordinates.
    RECT frame_bounds{};
    DwmGetWindowAttribute(options.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame_bounds, sizeof(frame_bounds));
    POINT client_origin{0, 0};
    ClientToScreen(options.hwnd, &client_origin);
    const double client_dx = client_origin.x - frame_bounds.left;
    const double client_dy = client_origin.y - frame_bounds.top;

    const double pattern_aspect = static_cast<double>(options.pattern_w) / options.pattern_h;
    double fit_w = options.rect_w, fit_h = options.rect_h;
    if (pattern_aspect > fit_w / fit_h) fit_h = fit_w / pattern_aspect;
    else fit_w = fit_h * pattern_aspect;
    const double fit_x = client_dx + options.rect_x + (options.rect_w - fit_w) / 2;
    const double fit_y = client_dy + options.rect_y + (options.rect_h - fit_h) / 2;

    FILE* out = nullptr;
    if (fopen_s(&out, options.out.c_str(), "wb") != 0 || !out) fail("cannot open --out");

    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    double refresh_hz = 0;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.rateRefresh.uiDenominator)
        refresh_hz = static_cast<double>(timing.rateRefresh.uiNumerator) / timing.rateRefresh.uiDenominator;
    std::fprintf(out,
        "{\"ev\":\"probe\",\"refresh_hz\":%.3f,\"fit\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f},"
        "\"pattern\":{\"w\":%u,\"h\":%u}}\n",
        refresh_hz, fit_x, fit_y, fit_w, fit_h, options.pattern_w, options.pattern_h);

    std::mutex mutex;
    ComPtr<ID3D11Texture2D> band;
    D3D11_BOX band_box{};
    std::atomic<uint64_t> frames{0}, decoded{0};
    const auto started = std::chrono::steady_clock::now();
    bool snapshot_taken = options.snapshot.empty();
    auto size = item.Size();
    constexpr int kBuffers = 3;
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, kBuffers, size);

    const auto make_staging = [&](UINT width, UINT height, ComPtr<ID3D11Texture2D>& texture) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        texture.Reset();
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture));
    };

    pool.FrameArrived([&](Direct3D11CaptureFramePool const& sender, auto&&) {
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;
        std::lock_guard lock(mutex);
        if (!out) return;
        const auto processing_started = std::chrono::steady_clock::now();
        const int64_t t = frame.SystemRelativeTime().count();
        const auto content = frame.ContentSize();
        if (content.Width != size.Width || content.Height != size.Height) {
            size = content;
            band.Reset();
            sender.Recreate(winrt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, kBuffers, size);
            return;
        }
        ComPtr<ID3D11Texture2D> texture;
        frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()
            ->GetInterface(IID_PPV_ARGS(&texture));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        // Only the counter band is read back each frame, so the probe never
        // holds a pool buffer long enough to make WGC skip compositions.
        if (!band) {
            const auto clamp = [](double value, UINT limit) {
                return static_cast<UINT>((std::min)((std::max)(value, 0.0), static_cast<double>(limit)));
            };
            band_box = {clamp(fit_x - 2, desc.Width), clamp(fit_y + fit_h * 0.75 - 2, desc.Height), 0,
                        clamp(fit_x + fit_w + 2, desc.Width), clamp(fit_y + fit_h + 2, desc.Height), 1};
            if (band_box.right <= band_box.left || band_box.bottom <= band_box.top ||
                !make_staging(band_box.right - band_box.left, band_box.bottom - band_box.top, band))
                return;
        }
        context->CopySubresourceRegion(band.Get(), 0, 0, 0, 0, texture.Get(), 0, &band_box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(band.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
        const auto counter = parties::client::decode_test_pattern(
            static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
            band_box.right - band_box.left, band_box.bottom - band_box.top,
            fit_x - band_box.left, fit_y - band_box.top, fit_w, fit_h);
        context->Unmap(band.Get(), 0);

        if (!snapshot_taken && counter &&
            std::chrono::steady_clock::now() - started > std::chrono::duration<double>(options.seconds / 2)) {
            ComPtr<ID3D11Texture2D> full;
            if (make_staging(desc.Width, desc.Height, full)) {
                context->CopyResource(full.Get(), texture.Get());
                if (SUCCEEDED(context->Map(full.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    snapshot_taken = save_bmp(options.snapshot, static_cast<const uint8_t*>(mapped.pData),
                                              mapped.RowPitch, desc.Width, desc.Height);
                    context->Unmap(full.Get(), 0);
                }
            }
        }

        const auto processing_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - processing_started).count();
        if (counter) {
            std::fprintf(out, "{\"ev\":\"glass\",\"t\":%lld,\"counter\":%u,\"proc_us\":%lld}\n",
                         static_cast<long long>(t), *counter, static_cast<long long>(processing_us));
            ++decoded;
        } else {
            std::fprintf(out, "{\"ev\":\"glass\",\"t\":%lld,\"counter\":null,\"proc_us\":%lld}\n",
                         static_cast<long long>(t), static_cast<long long>(processing_us));
        }
        ++frames;
    });

    auto session = pool.CreateCaptureSession(item);
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
        session.IsBorderRequired(false);
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
        session.IsCursorCaptureEnabled(false);
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval"))
        session.MinUpdateInterval(std::chrono::milliseconds(1));
    session.StartCapture();

    while (std::chrono::steady_clock::now() - started < std::chrono::duration<double>(options.seconds) &&
           IsWindow(options.hwnd))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    session.Close();
    pool.Close();
    {
        std::lock_guard lock(mutex);
        std::fclose(out);
        out = nullptr;
    }
    std::printf("{\"frames\":%llu,\"decoded\":%llu,\"refresh_hz\":%.3f,\"snapshot\":%s}\n",
                static_cast<unsigned long long>(frames.load()), static_cast<unsigned long long>(decoded.load()),
                refresh_hz, snapshot_taken && !options.snapshot.empty() ? "true" : "false");
    return frames.load() > 0 ? 0 : 1;
}

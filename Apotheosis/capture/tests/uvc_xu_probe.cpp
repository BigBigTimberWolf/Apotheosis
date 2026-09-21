
// UVC 采集卡「压缩质量」探针 —— 查这张卡到底有没有任何可以改变 MJPEG 码流大小的控件。
//
// 动机: 这张卡吐出来的 MJPEG 约 950KB / 1080p ≈ 3.65 bit/像素 (接近视觉无损),
//   而 JPEG 熵解码的开销和码流字节数成正比, 950KB 就要 6~7ms/帧, 240fps 需要
//   ~1.5 个核。如果能把码流压到 300~450KB, 解码掉到 2~3ms, 这台 4 核机就能跑 240。
//   UVC 标准里 MJPEG 没有质量/码率控制 —— 所以只能找厂商私有控件。
//
// 本工具查四件事:
//   [1] KS 拓扑节点列表 —— 有没有 KSNODETYPE_DEV_SPECIFIC (= 厂商扩展单元 XU)。
//       这是【不依赖 GUID】的决定性证据: 没有 XU 节点 = 厂商私有控件不存在。
//   [2] PROPSETID_VIDCAP_VIDEOCOMPRESSION (标准文档化的压缩属性集) 的 GETINFO / QUALITY
//   [3] IAMVideoProcAmp 标准旋钮 (亮度/对比度/饱和度/锐度/伽马/白平衡...)
//   [4] IAMCameraControl 标准旋钮
//
// 用法: uvc_xu_probe [设备名子串]
//   例: uvc_xu_probe KUHAIMI

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dshow.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <vidcap.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

// ks.h 里的 IKsControl 声明被 MIDL 宏 (_IKsControl_ / DECLARE_INTERFACE_) 包着,
// 在这个编译单元里不会展开。这里按 ks.h 里【同一个 IID】自己声明一份
// (见 ks.h L4010 的 STATIC_IID_IKsControl)。
struct __declspec(uuid("28F54685-06FD-11D2-B27A-00A0C9223196")) IKsControlLocal;

struct IKsControlLocal : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE KsProperty(KSPROPERTY* Property, ULONG PropertyLength,
                                                 void* PropertyData, ULONG DataLength,
                                                 ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(KSMETHOD* Method, ULONG MethodLength,
                                               void* MethodData, ULONG DataLength,
                                               ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(KSEVENT* Event, ULONG EventLength,
                                              void* EventData, ULONG DataLength,
                                              ULONG* BytesReturned) = 0;
};

// ── Microsoft UVC 扩展单元属性集 ────────────────────────────────────────────
// 注意: 当前 SDK 的 ksmedia.h 里【没有】这个属性集的定义, 这个值取自 UVC /
// DirectShow 扩展单元文档与样例。因此它是"尽力而为"的一环 —— 它失败【不能】
// 单独证明"没有 XU"; 判断"有没有 XU"要看 [1] 的拓扑节点列表。
const GUID kXuPropSet =
{ 0x2FA368C0, 0x3B8C, 0x11D3, { 0xBD, 0x4B, 0x00, 0x00, 0xF8, 0x5B, 0xBE, 0x5A } };

std::string GuidToStr(const GUID& g)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

std::string WideToUtf8(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

const char* NodeTypeName(const GUID& g)
{
    if (IsEqualGUID(g, KSNODETYPE_DEV_SPECIFIC))          return "★ 厂商扩展单元 XU (DEV_SPECIFIC)";
    if (IsEqualGUID(g, KSNODETYPE_VIDEO_STREAMING))       return "视频流 (VIDEO_STREAMING)";
    if (IsEqualGUID(g, KSNODETYPE_VIDEO_CAMERA_TERMINAL)) return "摄像头终端 (CAMERA_TERMINAL)";
    if (IsEqualGUID(g, KSNODETYPE_VIDEO_INPUT_TERMINAL))  return "输入终端 (INPUT_TERMINAL)";
    if (IsEqualGUID(g, KSNODETYPE_VIDEO_OUTPUT_TERMINAL)) return "输出终端 (OUTPUT_TERMINAL)";
    if (IsEqualGUID(g, KSNODETYPE_VIDEO_PROCESSING))      return "视频处理单元 PU (VIDEO_PROCESSING)";
    if (IsEqualGUID(g, KSNODETYPE_VIDEO_SELECTOR))        return "选择器 (VIDEO_SELECTOR)";
    return "(其它)";
}

HRESULT KsGet(IKsControlLocal* ctl, const GUID& set, ULONG id, ULONG flags,
              void* buf, ULONG bufLen, ULONG* bytesReturned)
{
    KSPROPERTY prop{};
    prop.Set = set;
    prop.Id = id;
    prop.Flags = flags;
    return ctl->KsProperty(&prop, sizeof(prop), buf, bufLen, bytesReturned);
}

void DumpProcAmp(IAMVideoProcAmp* amp)
{
    struct Item { long id; const char* name; };
    const Item items[] = {
        { VideoProcAmp_Brightness, "Brightness 亮度" },
        { VideoProcAmp_Contrast,   "Contrast 对比度" },
        { VideoProcAmp_Hue,        "Hue 色调" },
        { VideoProcAmp_Saturation, "Saturation 饱和度" },
        { VideoProcAmp_Sharpness,  "Sharpness 锐度" },
        { VideoProcAmp_Gamma,      "Gamma 伽马" },
        { VideoProcAmp_ColorEnable,"ColorEnable 彩色开关" },
        { VideoProcAmp_WhiteBalance,"WhiteBalance 白平衡" },
        { VideoProcAmp_BacklightCompensation, "Backlight 背光补偿" },
        { VideoProcAmp_Gain,       "Gain 增益" },
    };
    if (!amp) { printf("      (不支持 IAMVideoProcAmp)\n"); return; }
    bool any = false;
    for (const Item& it : items)
    {
        long mn = 0, mx = 0, st = 0, df = 0, cap = 0;
        if (SUCCEEDED(amp->GetRange(it.id, &mn, &mx, &st, &df, &cap)))
        {
            // strmif.h 里只有 VideoProcAmp_Flags_Auto(0x1) / _Manual(0x2),
            // 含义是"支持自动 / 支持手动", 并没有 Get/Set 那种能力标志。
            const bool canAuto   = (cap & VideoProcAmp_Flags_Auto) != 0;
            const bool canManual = (cap & VideoProcAmp_Flags_Manual) != 0;
            long cur = 0;
            long curCap = 0;
            amp->Get(it.id, &cur, &curCap);
            printf("      %-26s 范围[%ld..%ld] 步长%ld 默认%ld 当前%ld  支持自动=%d 支持手动=%d\n",
                   it.name, mn, mx, st, df, cur, canAuto ? 1 : 0, canManual ? 1 : 0);
            any = true;
        }
    }
    if (!any) printf("      (所有标准旋钮都不支持)\n");
}

void DumpCameraControl(IAMCameraControl* cam)
{
    struct Item { long id; const char* name; };
    const Item items[] = {
        { CameraControl_Pan,      "Pan 平移" },
        { CameraControl_Tilt,     "Tilt 俯仰" },
        { CameraControl_Roll,     "Roll 滚转" },
        { CameraControl_Zoom,     "Zoom 变焦" },
        { CameraControl_Exposure, "Exposure 曝光" },
        { CameraControl_Iris,     "Iris 光圈" },
        { CameraControl_Focus,    "Focus 对焦" },
    };
    if (!cam) { printf("      (不支持 IAMCameraControl)\n"); return; }
    bool any = false;
    for (const Item& it : items)
    {
        long mn = 0, mx = 0, st = 0, df = 0, cap = 0;
        if (SUCCEEDED(cam->GetRange(it.id, &mn, &mx, &st, &df, &cap)))
        {
            long cur = 0;
            long curCap = 0;
            cam->Get(it.id, &cur, &curCap);
            printf("      %-26s 范围[%ld..%ld] 步长%ld 默认%ld 当前%ld\n",
                   it.name, mn, mx, st, df, cur);
            any = true;
        }
    }
    if (!any) printf("      (所有标准旋钮都不支持)\n");
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string want = (argc > 1) ? argv[1] : "KUHAIMI";

    printf("=== UVC 压缩质量探针 ===\n");
    printf("目标设备(子串匹配): \"%s\"\n\n", want.c_str());

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        printf("[FAIL] CoInitializeEx 失败\n");
        return 2;
    }

    ICreateDevEnum* devEnum = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&devEnum))) || !devEnum)
    {
        printf("[FAIL] 创建 ICreateDevEnum 失败\n");
        CoUninitialize();
        return 3;
    }

    IEnumMoniker* enumMoniker = nullptr;
    if (FAILED(devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0))
        || !enumMoniker)
    {
        printf("(系统里没有视频输入设备)\n");
        devEnum->Release();
        CoUninitialize();
        return 4;
    }

    IBaseFilter* target = nullptr;
    std::string targetName;
    IMoniker* moniker = nullptr;
    while (enumMoniker->Next(1, &moniker, nullptr) == S_OK)
    {
        IPropertyBag* bag = nullptr;
        std::string name = "(无名)";
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) && bag)
        {
            VARIANT v; VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR)
                name = WideToUtf8(v.bstrVal);
            VariantClear(&v);
            bag->Release();
        }
        printf("  找到设备: %s\n", name.c_str());

        if (!target && name.find(want) != std::string::npos)
        {
            if (SUCCEEDED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&target))) && target)
                targetName = name;
            else
                target = nullptr;
        }
        moniker->Release();
    }
    enumMoniker->Release();
    devEnum->Release();

    if (!target)
    {
        printf("\n[FAIL] 没找到名字含 \"%s\" 的设备\n", want.c_str());
        CoUninitialize();
        return 5;
    }
    printf("\n选中: %s\n", targetName.c_str());

    // ── [1] KS 拓扑节点 ─────────────────────────────────────────────────────
    printf("\n[1] KS 拓扑节点 (判断有没有厂商扩展单元 XU)\n");
    int xuNodes = 0;
    {
        IKsTopologyInfo* topo = nullptr;
        if (FAILED(target->QueryInterface(IID_PPV_ARGS(&topo))) || !topo)
        {
            printf("      [FAIL] 拿不到 IKsTopologyInfo (设备未绑定成功? 被别的程序占着?)\n");
        }
        else
        {
            DWORD n = 0;
            topo->get_NumNodes(&n);
            printf("      节点数 = %lu\n", static_cast<unsigned long>(n));
            for (DWORD i = 0; i < n; ++i)
            {
                GUID t{};
                if (FAILED(topo->get_NodeType(i, &t))) continue;
                if (IsEqualGUID(t, KSNODETYPE_DEV_SPECIFIC)) ++xuNodes;

                WCHAR nm[256] = {0};
                DWORD need = 0;
                const std::string name = SUCCEEDED(topo->get_NodeName(i, nm, sizeof(nm), &need))
                    ? WideToUtf8(nm) : std::string();
                printf("      node[%lu] %-36s %s  %s\n",
                       static_cast<unsigned long>(i), NodeTypeName(t),
                       GuidToStr(t).c_str(), name.c_str());
            }
            topo->Release();
        }
    }
    if (xuNodes > 0)
        printf("      >>> 有 %d 个 XU 节点, 值得继续挖厂商私有控件\n", xuNodes);
    else
        printf("      >>> 没有 XU 节点: 这张卡不提供任何厂商私有控件\n");

    // ── [1b] XU 描述符: 这个扩展单元里到底有几个控件 ────────────────────────
    printf("\n[1b] XU 描述符 (KSPROPERTY_EXTENSION_UNIT_INFO)\n");
    {
        IKsControlLocal* ctl = nullptr;
        if (FAILED(target->QueryInterface(IID_PPV_ARGS(&ctl))) || !ctl)
        {
            printf("      [FAIL] 拿不到 IKsControl\n");
        }
        else
        {
            // 只对 DEV_SPECIFIC 节点试。两种 flag 组合都试: 有的驱动要 TOPOLOGY。
            const ULONG flagSets[2] = {
                KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_TOPOLOGY,
                KSPROPERTY_TYPE_GET,
            };
            for (DWORD nodeId = 0; nodeId < 8; ++nodeId)
            {
                // 先确认这个 nodeId 确实是 XU
                IKsTopologyInfo* topo = nullptr;
                GUID t{};
                if (FAILED(target->QueryInterface(IID_PPV_ARGS(&topo))) || !topo) break;
                const bool isXu = SUCCEEDED(topo->get_NodeType(nodeId, &t))
                               && IsEqualGUID(t, KSNODETYPE_DEV_SPECIFIC);
                topo->Release();
                if (!isXu) continue;

                bool done = false;
                for (ULONG fi = 0; fi < 2 && !done; ++fi)
                {
                    KSP_NODE req{};
                    req.Property.Set   = kXuPropSet;
                    req.Property.Id    = 0;              // KSPROPERTY_EXTENSION_UNIT_INFO
                    req.Property.Flags = flagSets[fi];
                    req.NodeId         = nodeId;

                    BYTE buf[512] = {0};
                    ULONG got = 0;
                    const HRESULT hr = ctl->KsProperty(&req.Property, sizeof(req),
                                                       buf, sizeof(buf), &got);
                    if (FAILED(hr))
                    {
                        printf("      node[%lu] flags=0x%08lX -> hr=0x%08lX (%lu 字节)\n",
                               static_cast<unsigned long>(nodeId),
                               static_cast<unsigned long>(flagSets[fi]),
                               static_cast<unsigned long>(hr),
                               static_cast<unsigned long>(got));
                        continue;
                    }

                    done = true;
                    printf("      node[%lu] flags=0x%08lX -> OK, 返回 %lu 字节\n",
                           static_cast<unsigned long>(nodeId),
                           static_cast<unsigned long>(flagSets[fi]),
                           static_cast<unsigned long>(got));

                    // UVC XU 描述符布局:
                    //   GUID(16) + bNumControls(1) + bNrInPins(1) + baSourceID[n]
                    //   + bControlSize(1) + bmControls[bControlSize]
                    if (got < 17) { printf("      (太短, 解析不了)\n"); break; }
                    GUID xuGuid{};
                    std::memcpy(&xuGuid, buf, 16);
                    const unsigned numControls = buf[16];
                    printf("      XU GUID        = %s\n", GuidToStr(xuGuid).c_str());
                    printf("      控件数量       = %u\n", numControls);
                    if (got >= 18)
                    {
                        const unsigned nrInPins = buf[17];
                        printf("      输入 pin 数    = %u\n", nrInPins);
                        const size_t off = 18 + nrInPins;
                        if (got > off)
                        {
                            const unsigned ctrlSize = buf[off];
                            printf("      bmControls 长  = %u\n", ctrlSize);
                            printf("      bmControls     =");
                            for (unsigned b = 0; b < ctrlSize && (off + 1 + b) < got; ++b)
                                printf(" %02X", buf[off + 1 + b]);
                            printf("\n");
                        }
                    }
                    printf("      >>> 控件编号就是 UVC XU 的 selector (1..%u), 下面逐个读\n",
                           numControls);
                }
                if (!done)
                    printf("      >>> 这个候选属性集 GUID 没被接受 (也可能是 GUID 不对)\n");
            }
            ctl->Release();
        }
    }

    // ── [2] 标准压缩属性集 ──────────────────────────────────────────────────
    printf("\n[2] PROPSETID_VIDCAP_VIDEOCOMPRESSION (标准文档化的压缩属性集)\n");
    {
        IKsControlLocal* ctl = nullptr;
        if (FAILED(target->QueryInterface(IID_PPV_ARGS(&ctl))) || !ctl)
        {
            printf("      [FAIL] 拿不到 IKsControl\n");
        }
        else
        {
            for (ULONG stream = 0; stream < 2; ++stream)
            {
                KSPROPERTY_VIDEOCOMPRESSION_GETINFO_S info{};
                info.StreamIndex = stream;
                ULONG got = 0;
                const HRESULT hr = KsGet(ctl, PROPSETID_VIDCAP_VIDEOCOMPRESSION,
                                         KSPROPERTY_VIDEOCOMPRESSION_GETINFO,
                                         KSPROPERTY_TYPE_GET, &info, sizeof(info), &got);
                if (SUCCEEDED(hr))
                {
                    printf("      stream%lu: GETINFO ok  默认质量=%ld 质量档数=%ld 能力=0x%lX (%s%s)\n",
                           static_cast<unsigned long>(stream),
                           info.DefaultQuality, info.NumberOfQualitySettings,
                           static_cast<unsigned long>(info.Capabilities),
                           (info.Capabilities & KS_CompressionCaps_CanQuality) ? "CanQuality " : "",
                           (info.Capabilities & KS_CompressionCaps_CanKeyFrame) ? "CanKeyFrame" : "");

                    KSPROPERTY_VIDEOCOMPRESSION_S q{};
                    q.StreamIndex = stream;
                    got = 0;
                    if (SUCCEEDED(KsGet(ctl, PROPSETID_VIDCAP_VIDEOCOMPRESSION,
                                        KSPROPERTY_VIDEOCOMPRESSION_QUALITY,
                                        KSPROPERTY_TYPE_GET, &q, sizeof(q), &got)))
                        printf("                当前 QUALITY = %ld\n", q.Value);
                }
                else
                {
                    printf("      stream%lu: 不支持 (hr=0x%08lX)\n",
                           static_cast<unsigned long>(stream),
                           static_cast<unsigned long>(hr));
                }
            }
            ctl->Release();
        }
    }

    // ── [3][4] 标准旋钮 ─────────────────────────────────────────────────────
    printf("\n[3] IAMVideoProcAmp 标准旋钮\n");
    {
        IAMVideoProcAmp* amp = nullptr;
        target->QueryInterface(IID_PPV_ARGS(&amp));
        DumpProcAmp(amp);
        if (amp) amp->Release();
    }
    printf("\n[4] IAMCameraControl 标准旋钮\n");
    {
        IAMCameraControl* cam = nullptr;
        target->QueryInterface(IID_PPV_ARGS(&cam));
        DumpCameraControl(cam);
        if (cam) cam->Release();
    }

    target->Release();
    CoUninitialize();

    printf("\n=== 结论怎么读 ===\n");
    printf("  * [1] 没有 XU 节点 -> 卡不提供厂商私有控件; UVC 又没有标准的 MJPEG 质量控制,\n");
    printf("    那就【没有任何办法】让它把码流压小。\n");
    printf("  * [2] 若 GETINFO 成功且带 CanQuality -> 存在标准质量旋钮, 可以直接调低。\n");
    printf("  * [3][4] 这些是标准图像旋钮, 与压缩率无关。锐度/饱和度理论上能间接影响码流,\n");
    printf("    但幅度远不足以把 950KB 压到 400KB。\n");
    return 0;
}

// Direct Kernel Streaming probe for the currently installed UVC driver.
// This program neither installs a driver nor uses Media Foundation/DirectShow.
// Usage: ks_capture_probe --stream MJPG 1920 1080 240 5 --async
//        ks_capture_probe --stream YUY2 640 480 60 3
//        ks_capture_probe --xu-live
//        ks_capture_probe --stream MJPG 1280 720 60 3 --async build/diag/frame.jpg
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ksuser.lib")

namespace {

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string errorText(DWORD code) {
    char text[256]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, text, sizeof(text), nullptr);
    return std::to_string(code) + " " + text;
}

bool property(HANDLE handle, void* request, DWORD requestBytes,
              void* result, DWORD resultBytes, DWORD& returned) {
    returned = 0;
    return DeviceIoControl(handle, IOCTL_KS_PROPERTY, request, requestBytes,
                           result, resultBytes, &returned, nullptr) != FALSE;
}

bool pinProperty(HANDLE filter, ULONG pin, ULONG id, void* result,
                 DWORD resultBytes, DWORD& returned) {
    KSP_PIN request{};
    request.Property.Set = KSPROPSETID_Pin;
    request.Property.Id = id;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.PinId = pin;
    return property(filter, &request, sizeof(request), result, resultBytes, returned);
}

std::vector<BYTE> pinBlob(HANDLE filter, ULONG pin, ULONG id) {
    // The first request is expected to return ERROR_MORE_DATA/INSUFFICIENT_BUFFER.
    for (DWORD capacity = 4096; capacity <= 1024 * 1024; capacity *= 2) {
        std::vector<BYTE> buffer(capacity);
        DWORD returned = 0;
        if (pinProperty(filter, pin, id, buffer.data(), capacity, returned)) {
            buffer.resize(returned);
            return buffer;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER)
            break;
    }
    return {};
}

void dumpBytes(const char* label, ULONG selector, const BYTE* data, DWORD size) {
    std::printf("    %s selector %lu (%lu bytes)\n", label, selector, size);
    for (DWORD offset = 0; offset < size; offset += 16) {
        std::printf("      %04lX:", offset);
        for (DWORD i = offset; i < std::min<DWORD>(offset + 16, size); ++i)
            std::printf(" %02X", data[i]);
        std::puts("");
    }
}

void probeExtensionUnit(HANDLE filter, bool dump, bool live) {
    constexpr GUID vendorXu = {0x7CA138CF, 0x71F2, 0x4EC5,
        {0x8D, 0x4C, 0xF0, 0x87, 0x74, 0x1C, 0xAB, 0xAE}};
    KSPROPERTY topology{KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_NODES,
                        KSPROPERTY_TYPE_GET};
    std::vector<BYTE> nodes(4096);
    DWORD returned = 0;
    if (!property(filter, &topology, sizeof(topology), nodes.data(),
                  DWORD(nodes.size()), returned) || returned < sizeof(KSMULTIPLE_ITEM)) {
        std::printf("  topology query failed: %s\n", errorText(GetLastError()).c_str());
        return;
    }
    const auto* list = reinterpret_cast<const KSMULTIPLE_ITEM*>(nodes.data());
    if (list->Count > (returned - sizeof(KSMULTIPLE_ITEM)) / sizeof(GUID)) return;
    const auto* types = reinterpret_cast<const GUID*>(nodes.data() + sizeof(KSMULTIPLE_ITEM));
    for (ULONG node = 0; node < list->Count; ++node) {
        if (!IsEqualGUID(types[node], KSNODETYPE_DEV_SPECIFIC)) continue;
        std::printf("  vendor extension KS node %lu\n", node);
        KSP_NODE request{};
        request.Property.Set = vendorXu;
        request.Property.Id = KSPROPERTY_EXTENSION_UNIT_INFO;
        request.Property.Flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_TOPOLOGY;
        request.NodeId = node;
        BYTE info[512]{};
        if (property(filter, &request, sizeof(request), info, sizeof(info), returned)) {
            std::printf("  XU info: %lu bytes", returned);
            for (DWORD i = 0; i < std::min<DWORD>(returned, 32); ++i)
                std::printf(" %02X", info[i]);
            std::puts("");
        } else {
            std::printf("  XU info failed: %s\n", errorText(GetLastError()).c_str());
        }
        for (ULONG selector = 1; selector <= 24; ++selector) {
            // Selector 1 appears to expose a changing firmware data block.
            // Keep live observations focused on small, stable controls.
            if (live && selector == 1) continue;
            request.Property.Id = selector;
            request.Property.Flags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_TOPOLOGY;
            KSPROPERTY_DESCRIPTION description{};
            if (!property(filter, &request, sizeof(request), &description,
                          sizeof(description), returned)) {
                std::printf("  selector %2lu: basicsupport failed: %s\n",
                            selector, errorText(GetLastError()).c_str());
                continue;
            }
            std::printf("  selector %2lu: access=0x%08lX desc=%lu members=%lu",
                        selector, description.AccessFlags,
                        description.DescriptionSize, description.MembersListCount);
            ULONG valueBytes = 256;
            if (description.DescriptionSize >= sizeof(KSPROPERTY_DESCRIPTION)
                    + sizeof(KSPROPERTY_MEMBERSHEADER)
                && description.DescriptionSize <= 65536) {
                std::vector<BYTE> full(description.DescriptionSize);
                DWORD fullReturned = 0;
                if (property(filter, &request, sizeof(request), full.data(),
                             DWORD(full.size()), fullReturned)
                    && fullReturned >= sizeof(KSPROPERTY_DESCRIPTION)
                        + sizeof(KSPROPERTY_MEMBERSHEADER)) {
                    const auto* members = reinterpret_cast<const KSPROPERTY_MEMBERSHEADER*>(
                        full.data() + sizeof(KSPROPERTY_DESCRIPTION));
                    std::printf(" memberSize=%lu memberCount=%lu",
                                members->MembersSize, members->MembersCount);
                    if (members->MembersSize > 0 && members->MembersSize <= 65536)
                        valueBytes = members->MembersSize;
                    if (dump && (selector == 1 || selector == 3 || selector == 7 || selector == 24))
                        dumpBytes("BASICSUPPORT", selector, full.data(), fullReturned);
                }
            }
            if (!(description.AccessFlags & KSPROPERTY_TYPE_GET)) {
                std::puts("");
                continue;
            }
            request.Property.Flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_TOPOLOGY;
            std::vector<BYTE> value(valueBytes);
            if (!property(filter, &request, sizeof(request), value.data(),
                          DWORD(value.size()), returned)) {
                std::printf(" GET failed: %s\n", errorText(GetLastError()).c_str());
                continue;
            }
            std::printf(" GET[%lu]", returned);
            for (DWORD i = 0; i < std::min<DWORD>(returned, 32); ++i)
                std::printf(" %02X", value[i]);
            if (returned > 32) std::printf(" ...");
            std::puts("");
            if (dump && (selector == 1 || selector == 3 || selector == 7 || selector == 24))
                dumpBytes("GET", selector, value.data(), returned);
        }
    }
}

std::string fourcc(const GUID& guid) {
    const uint32_t v = guid.Data1;
    char out[5] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24), 0};
    for (int i = 0; i < 4; ++i)
        if (out[i] < 32 || out[i] > 126) out[i] = '?';
    return out;
}

struct Range {
    ULONG pin;
    std::vector<BYTE> bytes;
    int width;
    int height;
    int fps;
    std::string format;
};

bool isVideo(const KSDATARANGE& range) {
    return IsEqualGUID(range.MajorFormat, KSDATAFORMAT_TYPE_VIDEO)
        && (IsEqualGUID(range.Specifier, KSDATAFORMAT_SPECIFIER_VIDEOINFO)
            || IsEqualGUID(range.Specifier, KSDATAFORMAT_SPECIFIER_VIDEOINFO2));
}

std::vector<Range> enumerateRanges(HANDLE filter, ULONG pin) {
    std::vector<Range> result;
    const auto blob = pinBlob(filter, pin, KSPROPERTY_PIN_DATARANGES);
    if (blob.size() < sizeof(KSMULTIPLE_ITEM)) return result;
    const auto* list = reinterpret_cast<const KSMULTIPLE_ITEM*>(blob.data());
    const size_t limit = std::min<size_t>(blob.size(), list->Size);
    size_t offset = sizeof(KSMULTIPLE_ITEM);
    for (ULONG i = 0; i < list->Count && offset + sizeof(KSDATARANGE) <= limit; ++i) {
        const auto* range = reinterpret_cast<const KSDATARANGE*>(blob.data() + offset);
        const size_t size = range->FormatSize;
        if (size < sizeof(KSDATARANGE) || size > limit - offset) break;
        if (isVideo(*range)) {
            int width = 0, height = 0, fps = 0;
            if (IsEqualGUID(range->Specifier, KSDATAFORMAT_SPECIFIER_VIDEOINFO)
                && size >= sizeof(KS_DATARANGE_VIDEO)) {
                const auto* video = reinterpret_cast<const KS_DATARANGE_VIDEO*>(range);
                width = video->VideoInfoHeader.bmiHeader.biWidth;
                height = std::abs(video->VideoInfoHeader.bmiHeader.biHeight);
                if (video->VideoInfoHeader.AvgTimePerFrame > 0)
                    fps = int(10000000 / video->VideoInfoHeader.AvgTimePerFrame);
            } else if (IsEqualGUID(range->Specifier, KSDATAFORMAT_SPECIFIER_VIDEOINFO2)
                       && size >= sizeof(KS_DATARANGE_VIDEO2)) {
                const auto* video = reinterpret_cast<const KS_DATARANGE_VIDEO2*>(range);
                width = video->VideoInfoHeader.bmiHeader.biWidth;
                height = std::abs(video->VideoInfoHeader.bmiHeader.biHeight);
                if (video->VideoInfoHeader.AvgTimePerFrame > 0)
                    fps = int(10000000 / video->VideoInfoHeader.AvgTimePerFrame);
            }
            if (width > 0 && height > 0) {
                result.push_back({pin, std::vector<BYTE>(blob.begin() + offset,
                                 blob.begin() + offset + size), width, height, fps,
                                 fourcc(range->SubFormat)});
            }
        }
        offset += (size + 7) & ~size_t(7);
    }
    return result;
}

std::vector<BYTE> intersect(HANDLE filter, const Range& range) {
    const size_t prefix = sizeof(KSP_PIN) + sizeof(KSMULTIPLE_ITEM);
    std::vector<BYTE> request(prefix + ((range.bytes.size() + 7) & ~size_t(7)));
    auto* pin = reinterpret_cast<KSP_PIN*>(request.data());
    pin->Property.Set = KSPROPSETID_Pin;
    pin->Property.Id = KSPROPERTY_PIN_DATAINTERSECTION;
    pin->Property.Flags = KSPROPERTY_TYPE_GET;
    pin->PinId = range.pin;
    auto* multiple = reinterpret_cast<KSMULTIPLE_ITEM*>(request.data() + sizeof(KSP_PIN));
    multiple->Count = 1;
    multiple->Size = DWORD(request.size() - sizeof(KSP_PIN));
    std::memcpy(request.data() + prefix, range.bytes.data(), range.bytes.size());
    std::vector<BYTE> output(4096);
    DWORD returned = 0;
    if (!property(filter, request.data(), DWORD(request.size()),
                  output.data(), DWORD(output.size()), returned)) {
        std::printf("  intersection failed: %s\n", errorText(GetLastError()).c_str());
        return {};
    }
    if (returned < sizeof(KSDATAFORMAT)) return {};
    const auto* format = reinterpret_cast<const KSDATAFORMAT*>(output.data());
    if (format->FormatSize < sizeof(KSDATAFORMAT) || format->FormatSize > returned)
        return {};
    output.resize(format->FormatSize);
    return output;
}

bool setState(HANDLE pin, KSSTATE state) {
    KSPROPERTY request{KSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE,
                       KSPROPERTY_TYPE_SET};
    DWORD returned = 0;
    return property(pin, &request, sizeof(request), &state, sizeof(state), returned);
}

bool streamAsync(HANDLE pin, const Range& range, int seconds, bool copyBench,
                 const std::string& snapshotPath) {
    struct Slot {
        std::vector<BYTE> image;
        KSSTREAM_HEADER header{};
        OVERLAPPED overlapped{};
        HANDLE event = nullptr;
        bool pending = false;
        bool ready = false;
        DWORD returned = 0;
    };
    constexpr int slotCount = 4;
    const size_t capacity = std::max<size_t>(2 * 1024 * 1024,
        size_t(range.width) * size_t(range.height) * 4);
    std::vector<Slot> slots(slotCount);
    bool okay = true;
    for (auto& slot : slots) {
        slot.image.resize(capacity);
        slot.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!slot.event) okay = false;
    }
    auto submit = [&](Slot& slot) {
        slot.header = {};
        slot.header.Size = sizeof(slot.header);
        slot.header.FrameExtent = DWORD(slot.image.size());
        slot.header.Data = slot.image.data();
        slot.overlapped = {};
        slot.overlapped.hEvent = slot.event;
        ResetEvent(slot.event);
        slot.ready = false;
        slot.pending = false;
        slot.returned = 0;
        if (DeviceIoControl(pin, IOCTL_KS_READ_STREAM,
                            &slot.header, sizeof(slot.header),
                            &slot.header, sizeof(slot.header),
                            &slot.returned, &slot.overlapped)) {
            slot.ready = true;
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            slot.pending = true;
            return true;
        }
        std::printf("  async read submit failed: %s\n", errorText(error).c_str());
        return false;
    };
    if (okay)
        for (auto& slot : slots)
            if (!submit(slot)) { okay = false; break; }

    int received = 0;
    int empty = 0;
    int validJpeg = 0;
    int timed = 0;
    double ageSumMs = 0.0;
    double copySumMs = 0.0;
    int copied = 0;
    bool snapshotSaved = false;
    unsigned int copyChecksum = 0;
    std::vector<BYTE> copyBuffer;
    if (copyBench) copyBuffer.resize(capacity);
    LARGE_INTEGER qpcFrequency{};
    QueryPerformanceFrequency(&qpcFrequency);
    const auto start = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (okay && std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
        Slot* completed = nullptr;
        for (auto& slot : slots)
            if (slot.ready) { completed = &slot; break; }
        if (!completed) {
            HANDLE events[slotCount]{};
            int indices[slotCount]{};
            DWORD count = 0;
            for (int i = 0; i < slotCount; ++i)
                if (slots[i].pending) {
                    events[count] = slots[i].event;
                    indices[count] = i;
                    ++count;
                }
            if (!count) break;
            const DWORD wait = WaitForMultipleObjects(count, events, FALSE, 1000);
            if (wait == WAIT_TIMEOUT) continue;
            if (wait < WAIT_OBJECT_0 || wait >= WAIT_OBJECT_0 + count) {
                std::puts("  wait failed");
                break;
            }
            completed = &slots[indices[wait - WAIT_OBJECT_0]];
            if (!GetOverlappedResult(pin, &completed->overlapped,
                                     &completed->returned, FALSE)) {
                std::printf("  async completion failed: %s\n",
                            errorText(GetLastError()).c_str());
                break;
            }
            completed->pending = false;
            completed->ready = true;
        }
        if (completed->header.DataUsed > 0) {
            if (std::chrono::steady_clock::now() >= start) {
                ++received;
                if (range.format == "MJPG" && completed->header.DataUsed >= 2
                    && completed->image[0] == 0xFF && completed->image[1] == 0xD8)
                    ++validJpeg;
                if (!snapshotSaved && !snapshotPath.empty() && range.format == "MJPG"
                    && completed->header.DataUsed >= 4
                    && completed->header.DataUsed <= completed->image.size()
                    && completed->image[0] == 0xFF && completed->image[1] == 0xD8) {
                    FILE* output = nullptr;
                    if (fopen_s(&output, snapshotPath.c_str(), "wb") == 0 && output) {
                        const size_t written = std::fwrite(completed->image.data(), 1,
                            completed->header.DataUsed, output);
                        std::fclose(output);
                        snapshotSaved = written == completed->header.DataUsed;
                        if (snapshotSaved)
                            std::printf("  snapshot: %s\n", snapshotPath.c_str());
                    }
                }
                if (copyBench && completed->header.DataUsed <= copyBuffer.size()) {
                    LARGE_INTEGER before{}, after{};
                    QueryPerformanceCounter(&before);
                    std::memcpy(copyBuffer.data(), completed->image.data(),
                                completed->header.DataUsed);
                    QueryPerformanceCounter(&after);
                    copyChecksum += copyBuffer[completed->header.DataUsed / 2];
                    copySumMs += double(after.QuadPart - before.QuadPart) * 1000.0
                        / double(qpcFrequency.QuadPart);
                    ++copied;
                }
                if ((completed->header.OptionsFlags & KSSTREAM_HEADER_OPTIONSF_TIMEVALID) != 0
                    && qpcFrequency.QuadPart > 0) {
                    LARGE_INTEGER qpc{};
                    QueryPerformanceCounter(&qpc);
                    const double now100ns = double(qpc.QuadPart) * 10000000.0
                        / double(qpcFrequency.QuadPart);
                    const double ageMs = (now100ns
                        - double(completed->header.PresentationTime.Time)) / 10000.0;
                    if (ageMs >= 0.0 && ageMs < 1000.0) {
                        ageSumMs += ageMs;
                        ++timed;
                    }
                }
                if (received <= 3)
                    std::printf("  async frame %d: %lu bytes\n",
                                received, completed->header.DataUsed);
            }
        } else if (empty++ < 3) {
            std::printf("  async empty read: returned=%lu\n", completed->returned);
        }
        if (!submit(*completed)) break;
    }
    CancelIoEx(pin, nullptr);
    for (auto& slot : slots) {
        if (slot.pending)
            GetOverlappedResult(pin, &slot.overlapped, &slot.returned, TRUE);
        if (slot.event) CloseHandle(slot.event);
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("  async result: %d frames in %.3f s = %.1f fps\n",
                received, elapsed, received / std::max(0.001, elapsed));
    if (range.format == "MJPG")
        std::printf("  JPEG SOI valid: %d/%d\n", validJpeg, received);
    if (copied)
        std::printf("  copied JPEG bytes: %.4f ms/frame (%d frames, checksum %u)\n",
                    copySumMs / copied, copied, copyChecksum);
    if (timed)
        std::printf("  async KS timestamp to user read: %.3f ms average (%d frames)\n",
                    ageSumMs / timed, timed);
    return received > 0;
}

bool stream(HANDLE filter, const Range& range, int seconds,
            bool asyncReads, bool copyBench, const std::string& snapshotPath) {
    auto format = intersect(filter, range);
    if (format.empty()) return false;
    std::vector<BYTE> connect(sizeof(KSPIN_CONNECT) + format.size());
    auto* pinConnect = reinterpret_cast<KSPIN_CONNECT*>(connect.data());
    pinConnect->Interface.Set = KSINTERFACESETID_Standard;
    pinConnect->Interface.Id = KSINTERFACE_STANDARD_STREAMING;
    pinConnect->Medium.Set = KSMEDIUMSETID_Standard;
    pinConnect->Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
    pinConnect->PinId = range.pin;
    pinConnect->Priority.PriorityClass = KSPRIORITY_NORMAL;
    pinConnect->Priority.PrioritySubClass = 1;
    std::memcpy(connect.data() + sizeof(KSPIN_CONNECT), format.data(), format.size());
    Handle pin;
    const DWORD status = KsCreatePin(filter, pinConnect, GENERIC_READ, &pin.value);
    if (status != ERROR_SUCCESS) {
        std::printf("  KsCreatePin failed: %s\n", errorText(status).c_str());
        return false;
    }
    std::printf("  KS pin opened: %s %dx%d @%d\n", range.format.c_str(),
                range.width, range.height, range.fps);
    for (KSSTATE state : {KSSTATE_ACQUIRE, KSSTATE_PAUSE, KSSTATE_RUN}) {
        if (!setState(pin.value, state)) {
            std::printf("  state %d failed: %s\n", int(state),
                        errorText(GetLastError()).c_str());
            return false;
        }
    }
    if (asyncReads) {
        const bool success = streamAsync(pin.value, range, seconds, copyBench, snapshotPath);
        setState(pin.value, KSSTATE_STOP);
        return success;
    }

    // Bounded raw delivery test. It measures throughput, not input-to-output latency.
    const size_t capacity = std::max<size_t>(2 * 1024 * 1024,
        size_t(range.width) * size_t(range.height) * 4);
    std::vector<BYTE> image(capacity);
    int received = 0;
    int empty = 0;
    int timed = 0;
    double ageSumMs = 0.0;
    LARGE_INTEGER qpcFrequency{};
    QueryPerformanceFrequency(&qpcFrequency);
    const auto warmupStart = std::chrono::steady_clock::now();
    const auto start = warmupStart + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
        KSSTREAM_HEADER header{};
        header.Size = sizeof(header);
        header.FrameExtent = DWORD(image.size());
        header.Data = image.data();
        DWORD returned = 0;
        if (!DeviceIoControl(pin.value, IOCTL_KS_READ_STREAM, &header, sizeof(header),
                             &header, sizeof(header), &returned, nullptr)) {
            std::printf("  read failed: %s\n", errorText(GetLastError()).c_str());
            break;
        }
        if (header.DataUsed > 0) {
            if (std::chrono::steady_clock::now() < start) continue;
            ++received;
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            double ageMs = -1.0;
            if ((header.OptionsFlags & KSSTREAM_HEADER_OPTIONSF_TIMEVALID) != 0
                && qpcFrequency.QuadPart > 0) {
                const double now100ns = double(qpc.QuadPart) * 10000000.0
                    / double(qpcFrequency.QuadPart);
                ageMs = (now100ns - double(header.PresentationTime.Time)) / 10000.0;
                if (ageMs >= 0.0 && ageMs < 1000.0) {
                    ageSumMs += ageMs;
                    ++timed;
                }
            }
            if (received <= 3)
                std::printf("  frame %d: %lu bytes, timestamp %lld, flags 0x%lx, qpc-age %.3f ms\n",
                            received, header.DataUsed, header.PresentationTime.Time,
                            header.OptionsFlags, ageMs);
        }
        else if (empty++ < 3) {
            std::printf("  empty read: returned=%lu extent=%lu flags=0x%lx\n",
                        returned, header.FrameExtent, header.OptionsFlags);
        }
    }
    setState(pin.value, KSSTATE_STOP);
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("  result: %d frames in %.3f s = %.1f fps\n",
                received, elapsed, received / elapsed);
    if (timed)
        std::printf("  KS timestamp to user read: %.3f ms average (%d frames)\n",
                    ageSumMs / timed, timed);
    return received > 0;
}

void probeCategory(const GUID& category, const char* label, bool doStream,
                   const std::string& wantedFormat, int wantedWidth, int wantedHeight,
                   int wantedFps, int seconds, bool asyncReads,
                   bool copyBench, const std::string& snapshotPath, bool doXu,
                   bool dumpXu, bool liveXu, bool& streamed) {
    HDEVINFO devices = SetupDiGetClassDevsW(&category, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{sizeof(interfaceData)};
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &category, index, &interfaceData))
            break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &needed, nullptr);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> storage(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA deviceInfo{sizeof(deviceInfo)};
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail,
                                              needed, nullptr, &deviceInfo)) continue;
        WCHAR instance[512]{};
        if (!SetupDiGetDeviceInstanceIdW(devices, &deviceInfo, instance,
                                         512, nullptr)) continue;
        const std::wstring id(instance);
        if (id.find(L"VID_048D&PID_CA30&MI_00") == std::wstring::npos) continue;
        std::printf("%s interface index %lu (KUHAIMI video)\n", label, index);
        Handle filter;
        filter.value = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (filter.value == INVALID_HANDLE_VALUE) {
            std::printf("  CreateFile failed: %s\n", errorText(GetLastError()).c_str());
            continue;
        }
        if (doXu) {
            probeExtensionUnit(filter.value, dumpXu, liveXu);
            continue;
        }
        KSPROPERTY countRequest{KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES,
                                KSPROPERTY_TYPE_GET};
        ULONG pinCount = 0;
        DWORD returned = 0;
        if (!property(filter.value, &countRequest, sizeof(countRequest),
                      &pinCount, sizeof(pinCount), returned)) {
            std::printf("  pin count failed: %s\n", errorText(GetLastError()).c_str());
            continue;
        }
        std::printf("  %lu pin factories\n", pinCount);
        std::vector<Range> ranges;
        for (ULONG pin = 0; pin < pinCount; ++pin) {
            KSPIN_DATAFLOW flow{};
            KSPIN_COMMUNICATION communication{};
            pinProperty(filter.value, pin, KSPROPERTY_PIN_DATAFLOW,
                        &flow, sizeof(flow), returned);
            pinProperty(filter.value, pin, KSPROPERTY_PIN_COMMUNICATION,
                        &communication, sizeof(communication), returned);
            auto video = enumerateRanges(filter.value, pin);
            std::printf("  pin %lu: flow=%d communication=%d video ranges=%zu\n",
                        pin, int(flow), int(communication), video.size());
            if (!doStream)
                for (const auto& range : video)
                    std::printf("    %s %dx%d @%d\n", range.format.c_str(),
                                range.width, range.height, range.fps);
            if (flow == KSPIN_DATAFLOW_OUT && communication != KSPIN_COMMUNICATION_NONE)
                ranges.insert(ranges.end(), video.begin(), video.end());
        }
        if (doStream && !streamed && !ranges.empty()) {
            std::stable_sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
                const int64_t pixelsA = int64_t(a.width) * a.height;
                const int64_t pixelsB = int64_t(b.width) * b.height;
                return pixelsA < pixelsB;
            });
            for (const auto& range : ranges) {
                if (range.format != wantedFormat || range.width != wantedWidth
                    || range.height != wantedHeight || range.fps != wantedFps)
                    continue;
                if (stream(filter.value, range, seconds, asyncReads, copyBench, snapshotPath)) {
                    streamed = true;
                    break;
                }
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool doStream = argc > 1 && std::strcmp(argv[1], "--stream") == 0;
    const bool dumpXu = argc > 1 && std::strcmp(argv[1], "--xu-dump") == 0;
    const bool liveXu = argc > 1 && std::strcmp(argv[1], "--xu-live") == 0;
    const bool doXu = (argc > 1 && std::strcmp(argv[1], "--xu") == 0) || dumpXu || liveXu;
    const std::string format = argc > 2 ? argv[2] : "YUY2";
    const int width = argc > 3 ? std::atoi(argv[3]) : 640;
    const int height = argc > 4 ? std::atoi(argv[4]) : 480;
    const int fps = argc > 5 ? std::atoi(argv[5]) : 60;
    const int seconds = std::clamp(argc > 6 ? std::atoi(argv[6]) : 3, 1, 30);
    const bool asyncReads = argc > 7 && std::strcmp(argv[7], "--async") == 0;
    const bool copyBench = argc > 8 && std::strcmp(argv[8], "--copy") == 0;
    const std::string snapshotPath = argc > 9 ? argv[9]
        : (argc > 8 && !copyBench ? argv[8] : "");
    bool streamed = false;
    probeCategory(KSCATEGORY_VIDEO, "KSCATEGORY_VIDEO", doStream,
                  format, width, height, fps, seconds, asyncReads, copyBench,
                  snapshotPath, doXu, dumpXu, liveXu, streamed);
    if (!doXu)
        probeCategory(KSCATEGORY_CAPTURE, "KSCATEGORY_CAPTURE", doStream,
                      format, width, height, fps, seconds, asyncReads, copyBench,
                      snapshotPath, doXu, dumpXu, liveXu, streamed);
    if (doStream && !streamed) {
        std::puts("No KS frame received.");
        return 2;
    }
    return 0;
}

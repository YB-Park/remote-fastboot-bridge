#include "devices.hpp"
#include "socket.hpp"

#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <objbase.h>
#include <setupapi.h>
#include <winusb.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <map>

namespace proxy {
namespace {
constexpr GUID android_interface{0xf72fe0d4, 0xcbcb, 0x407d, {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b}};
constexpr GUID usb_device_interface{0xa5dcbf10, 0x6530, 0x11d2, {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};
constexpr DEVPROPKEY location_paths{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 37};

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto size = static_cast<int>(value.size());
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size,
                                           nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, result.data(), bytes,
                            nullptr, nullptr) != bytes) return {};
    return result;
}

bool equal(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[noreturn]] void fail(std::string_view operation, DWORD code = GetLastError()) {
    throw Error(std::string(operation) + " Win32 error " + std::to_string(code));
}

class Handle {
public:
    ~Handle() { if (valid()) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
    HANDLE value = INVALID_HANDLE_VALUE;
};

class DeviceSet {
public:
    explicit DeviceSet(HDEVINFO info) : value(info) {
        if (value == INVALID_HANDLE_VALUE) fail("SetupDiGetClassDevs");
    }
    ~DeviceSet() { SetupDiDestroyDeviceInfoList(value); }
    DeviceSet(const DeviceSet&) = delete;
    DeviceSet& operator=(const DeviceSet&) = delete;
    HDEVINFO value;
};

struct InterfaceHandle {
    ~InterfaceHandle() { if (value) WinUsb_Free(value); }
    WINUSB_INTERFACE_HANDLE value = nullptr;
};

std::vector<std::wstring> strings(const wchar_t* value, std::size_t chars) {
    std::vector<std::wstring> result;
    for (std::size_t offset = 0; offset < chars && value[offset] != L'\0';) {
        const auto* begin = value + offset;
        const auto* last = std::find(begin, value + chars, L'\0');
        result.emplace_back(begin, last);
        offset += static_cast<std::size_t>(last - begin) + 1;
    }
    return result;
}

std::vector<std::wstring> pnp_strings(HDEVINFO set, SP_DEVINFO_DATA& info, DWORD property) {
    std::array<wchar_t, 4096> buffer{};
    DWORD type{}, length{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &info, property, &type,
            reinterpret_cast<BYTE*>(buffer.data()), sizeof(buffer), &length)) return {};
    if (type != REG_SZ && type != REG_MULTI_SZ) return {};
    return strings(buffer.data(), std::min<std::size_t>(length / sizeof(wchar_t), buffer.size()));
}

std::wstring instance_id(DEVINST dev) {
    std::array<wchar_t, MAX_DEVICE_ID_LEN> value{};
    if (CM_Get_Device_IDW(dev, value.data(), static_cast<ULONG>(value.size()), 0) != CR_SUCCESS) return {};
    return value.data();
}

std::wstring device_location(DEVINST dev) {
    std::array<wchar_t, 4096> value{};
    DEVPROPTYPE type{};
    ULONG size = sizeof(value);
    if (CM_Get_DevNode_PropertyW(dev, &location_paths, &type,
            reinterpret_cast<BYTE*>(value.data()), &size, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_STRING_LIST) return {};
    return value.data();
}

std::wstring unique_serial(DEVINST dev, std::wstring_view id) {
    ULONG capabilities{}, type{}, size = sizeof(capabilities);
    if (CM_Get_DevNode_Registry_PropertyW(dev, CM_DRP_CAPABILITIES, &type,
            &capabilities, &size, 0) != CR_SUCCESS || type != REG_DWORD ||
            (capabilities & CM_DEVCAP_UNIQUEID) == 0) return {};
    const auto slash = id.rfind(L'\\');
    if (slash == id.npos) return {};
    return std::wstring(id.substr(slash + 1));
}

std::uint16_t hardware_number(std::wstring_view id, std::wstring_view prefix) {
    const auto pos = id.find(prefix);
    if (pos == id.npos || id.size() - pos < prefix.size() + 4) return 0;
    const auto hex = utf8(id.substr(pos + prefix.size(), 4));
    std::uint16_t number{};
    const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), number, 16);
    return parsed.ec == std::errc{} && parsed.ptr == hex.data() + hex.size() ? number : 0;
}

void add_guid(std::vector<GUID>& classes, const GUID& guid) {
    if (std::ranges::none_of(classes, [&](const GUID& current) { return !!IsEqualGUID(current, guid); })) {
        classes.push_back(guid);
    }
}

void add_oem_guids(HDEVINFO set, SP_DEVINFO_DATA& info, std::vector<GUID>& classes) {
    const auto key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
    if (key == INVALID_HANDLE_VALUE) return;
    for (const auto* name : {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"}) {
        std::array<wchar_t, 4096> buffer{};
        DWORD type{}, size = sizeof(buffer);
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()),
                             &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_MULTI_SZ)) continue;
        for (const auto& value : strings(buffer.data(), std::min<std::size_t>(size / sizeof(wchar_t), buffer.size()))) {
            GUID guid{};
            if (CLSIDFromString(value.c_str(), &guid) == S_OK) add_guid(classes, guid);
        }
    }
    RegCloseKey(key);
}

void find_paths(const GUID& guid, std::map<DEVINST, Device>& nodes) {
    DeviceSet set(SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA info{};
        info.cbSize = sizeof(info);
        if (!SetupDiEnumDeviceInterfaces(set.value, nullptr, &guid, index, &info)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) fail("SetupDiEnumDeviceInterfaces");
            return;
        }
        if ((info.Flags & SPINT_ACTIVE) == 0) continue;
        DWORD size{};
        SetupDiGetDeviceInterfaceDetailW(set.value, &info, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || size > 65536) continue;
        std::vector<BYTE> storage(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA node{};
        node.cbSize = sizeof(node);
        if (!SetupDiGetDeviceInterfaceDetailW(set.value, &info, detail, size, nullptr, &node)) continue;
        const auto it = nodes.find(node.DevInst);
        if (it != nodes.end() && it->second.path.empty()) it->second.path = detail->DevicePath;
    }
}

std::wstring string_descriptor(WINUSB_INTERFACE_HANDLE usb, UCHAR index, USHORT language) {
    std::array<UCHAR, 256> buffer{};
    ULONG size{};
    if (!WinUsb_GetDescriptor(usb, USB_STRING_DESCRIPTOR_TYPE, index, language,
                              buffer.data(), static_cast<ULONG>(buffer.size()), &size) ||
        size < 2 || size > buffer.size() || buffer[0] != size ||
        buffer[1] != USB_STRING_DESCRIPTOR_TYPE || size % 2 != 0) return {};
    std::wstring result;
    for (std::size_t i = 2; i < size; i += 2) {
        result.push_back(static_cast<wchar_t>(buffer[i] | (buffer[i + 1] << 8)));
    }
    return result;
}

std::string usb_serial(WINUSB_INTERFACE_HANDLE usb, UCHAR index) {
    if (index == 0) return {};
    const auto languages = string_descriptor(usb, 0, 0);
    for (const auto lang : languages) {
        auto value = utf8(string_descriptor(usb, index, static_cast<USHORT>(lang)));
        if (!value.empty()) return value;
    }
    return utf8(string_descriptor(usb, index, 0x0409));
}

class WinUsb final : public Usb {
public:
    WinUsb(const Device& device, Millis timeout) : timeout_(timeout) {
        if (device.path.empty()) throw Error("no published WinUSB device interface; check driver");
        if (device.service != "WINUSB") throw Error("device driver service is not WinUSB");
        file_.value = CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (!file_.valid()) fail("exclusive CreateFile (check driver, ACL or another fastboot process)");
        if (!WinUsb_Initialize(file_.value, &first_.value)) fail("WinUsb_Initialize");
        find_interface();
        USB_DEVICE_DESCRIPTOR descriptor{};
        ULONG length{};
        if (!WinUsb_GetDescriptor(first_.value, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                reinterpret_cast<UCHAR*>(&descriptor), sizeof(descriptor), &length)) {
            fail("WinUsb_GetDescriptor");
        }
        if (length != sizeof(descriptor) || (device.vid != 0 && descriptor.idVendor != device.vid) ||
            (device.pid != 0 && descriptor.idProduct != device.pid)) {
            throw Error("USB device descriptor changed while opening");
        }
        serial_ = usb_serial(first_.value, descriptor.iSerialNumber);
        if (!device.serial.empty() && !serial_.empty()) {
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; };
            if (!std::equal(device.serial.begin(), device.serial.end(), serial_.begin(), serial_.end(),
                            [&](char a, char b) { return lower(a) == lower(b); })) {
                throw Error("USB serial differs from the device instance identity");
            }
        }

        const ULONG milliseconds = static_cast<ULONG>(timeout.count());
        const UCHAR off = FALSE;
        const UCHAR on = TRUE;
        policy(in_, PIPE_TRANSFER_TIMEOUT, milliseconds);
        policy(out_, PIPE_TRANSFER_TIMEOUT, milliseconds);
        policy(in_, IGNORE_SHORT_PACKETS, off);
        policy(in_, ALLOW_PARTIAL_READS, on);
        policy(in_, AUTO_FLUSH, off);
        policy(out_, SHORT_PACKET_TERMINATE, off);
        event_.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event_.valid()) fail("CreateEvent");

        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof(filter);
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
        filter.u.DeviceHandle.hTarget = file_.value;
        if (CM_Register_Notification(&filter, this, &notify, &notification_) != CR_SUCCESS) {
            notification_ = nullptr;
        }
    }

    ~WinUsb() override { if (notification_) CM_Unregister_Notification(notification_); }

    std::size_t read(Bytes destination, const Cancel& stop) override {
        return transfer(true, destination.data(), destination.size(), stop);
    }
    std::size_t write(ConstBytes source, const Cancel& stop) override {
        return transfer(false, const_cast<UCHAR*>(source.data()), source.size(), stop);
    }
    std::size_t out_packet_size() const override { return out_packet_; }
    bool removed() const override { return removed_.load(); }
    std::string details() const override {
        std::ostringstream text;
        text << "ff/42/03 serial=" << log_text(serial_) << " bulk IN=0x" << std::hex
             << static_cast<unsigned>(in_) << " OUT=0x" << static_cast<unsigned>(out_)
             << std::dec << " packet=" << out_packet_
             << " hotplug=" << (notification_ ? "PnP+poll" : "poll");
        return text.str();
    }

private:
    static DWORD CALLBACK notify(HCMNOTIFICATION, void* context, CM_NOTIFY_ACTION action,
                                 CM_NOTIFY_EVENT_DATA*, DWORD) {
        if (action == CM_NOTIFY_ACTION_DEVICEREMOVEPENDING ||
            action == CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE) {
            static_cast<WinUsb*>(context)->removed_.store(true);
        }
        return ERROR_SUCCESS;
    }

    static bool fastboot(WINUSB_INTERFACE_HANDLE handle, USB_INTERFACE_DESCRIPTOR& descriptor) {
        return WinUsb_QueryInterfaceSettings(handle, 0, &descriptor) &&
               descriptor.bInterfaceClass == 0xff && descriptor.bInterfaceSubClass == 0x42 &&
               descriptor.bInterfaceProtocol == 0x03;
    }

    void find_interface() {
        USB_INTERFACE_DESCRIPTOR descriptor{};
        if (fastboot(first_.value, descriptor)) interface_ = first_.value;
        for (UCHAR i = 0; interface_ == nullptr && i < 32; ++i) {
            if (!WinUsb_GetAssociatedInterface(first_.value, i, &associated_.value)) break;
            if (fastboot(associated_.value, descriptor)) {
                interface_ = associated_.value;
            } else {
                WinUsb_Free(associated_.value);
                associated_.value = nullptr;
            }
        }
        if (!interface_) throw Error("WinUSB interface descriptor is not fastboot ff/42/03");
        for (UCHAR index = 0; index < descriptor.bNumEndpoints; ++index) {
            WINUSB_PIPE_INFORMATION pipe{};
            if (!WinUsb_QueryPipe(interface_, 0, index, &pipe)) fail("WinUsb_QueryPipe");
            if (pipe.PipeType != UsbdPipeTypeBulk) continue;
            if ((pipe.PipeId & 0x80) != 0) {
                if (in_ != 0) throw Error("multiple USB bulk IN endpoints are ambiguous");
                in_ = pipe.PipeId;
            } else {
                if (out_ != 0) throw Error("multiple USB bulk OUT endpoints are ambiguous");
                out_ = pipe.PipeId;
                out_packet_ = pipe.MaximumPacketSize;
            }
        }
        if (in_ == 0 || out_ == 0 || out_packet_ == 0 || (65536 % out_packet_) != 0) {
            throw Error("missing or unsupported fastboot bulk endpoints");
        }
    }

    template<class T>
    void policy(UCHAR pipe, ULONG kind, T value) {
        if (!WinUsb_SetPipePolicy(interface_, pipe, kind, sizeof(value), &value)) fail("WinUsb_SetPipePolicy");
    }

    std::size_t transfer(bool read, UCHAR* buffer, std::size_t length, const Cancel& stop) {
        if (stop()) throw Error("USB transfer cancelled");
        OVERLAPPED operation{};
        operation.hEvent = event_.value;
        if (!ResetEvent(event_.value)) fail("ResetEvent");
        ULONG count{};
        const auto size = static_cast<ULONG>(length);
        const BOOL started = read ? WinUsb_ReadPipe(interface_, in_, buffer, size, &count, &operation)
                                  : WinUsb_WritePipe(interface_, out_, buffer, size, &count, &operation);
        if (started) return count;
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) fail(read ? "WinUsb_ReadPipe" : "WinUsb_WritePipe", error);

        const auto deadline = Clock::now() + timeout_;
        while (!stop() && Clock::now() < deadline) {
            const auto wait = WaitForSingleObject(event_.value, 25);
            if (wait == WAIT_TIMEOUT) continue;
            if (wait == WAIT_OBJECT_0) {
                if (!WinUsb_GetOverlappedResult(interface_, &operation, &count, FALSE)) {
                    fail(read ? "WinUSB bulk IN" : "WinUSB bulk OUT");
                }
                return count;
            }
            break;
        }
        // The OVERLAPPED object must outlive the cancellation completion.
        CancelIoEx(file_.value, &operation);
        WinUsb_GetOverlappedResult(interface_, &operation, &count, TRUE);
        throw Error("WinUSB bulk transfer timed out or was cancelled");
    }

    Handle file_;
    InterfaceHandle first_;
    InterfaceHandle associated_;
    Handle event_;
    WINUSB_INTERFACE_HANDLE interface_ = nullptr;
    HCMNOTIFICATION notification_ = nullptr;
    std::atomic_bool removed_{false};
    UCHAR in_ = 0;
    UCHAR out_ = 0;
    std::size_t out_packet_ = 0;
    Millis timeout_;
    std::string serial_;
};

class WinBackend final : public Backend {
public:
    std::string name() const override { return "winusb"; }

    std::vector<Device> enumerate() override {
        DeviceSet set(SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES));
        std::map<DEVINST, Device> nodes;
        std::vector<GUID> classes{android_interface, usb_device_interface};
        for (DWORD index = 0;; ++index) {
            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof(info);
            if (!SetupDiEnumDeviceInfo(set.value, index, &info)) {
                if (GetLastError() != ERROR_NO_MORE_ITEMS) fail("SetupDiEnumDeviceInfo");
                break;
            }
            const auto compatible = pnp_strings(set.value, info, SPDRP_COMPATIBLEIDS);
            if (std::ranges::none_of(compatible, [](const std::wstring& id) {
                    return equal(id, L"USB\\Class_FF&SubClass_42&Prot_03");
                })) continue;

            Device device;
            const auto id = instance_id(info.DevInst);
            if (id.empty()) continue;
            device.instance = utf8(id);
            auto physical = info.DevInst;
            auto physical_id = id;
            if (id.find(L"&MI_") != id.npos) {
                DEVINST parent{};
                if (CM_Get_Parent(&parent, info.DevInst, 0) == CR_SUCCESS) {
                    auto parent_id = instance_id(parent);
                    if (parent_id.starts_with(L"USB\\VID_")) {
                        physical = parent;
                        physical_id = std::move(parent_id);
                    }
                }
            }
            device.vid = hardware_number(physical_id, L"VID_");
            device.pid = hardware_number(physical_id, L"PID_");
            device.serial = utf8(unique_serial(physical, physical_id));
            device.location = utf8(device_location(physical));
            const auto service = pnp_strings(set.value, info, SPDRP_SERVICE);
            if (!service.empty()) device.service = equal(service.front(), L"WinUSB") ? "WINUSB" : utf8(service.front());
            if (device.service == "WINUSB") add_oem_guids(set.value, info, classes);
            nodes.emplace(info.DevInst, std::move(device));
        }

        if (!nodes.empty()) {
            for (const auto& guid : classes) find_paths(guid, nodes);
        }
        std::vector<Device> result;
        for (auto& [node, device] : nodes) {
            static_cast<void>(node);
            result.push_back(std::move(device));
        }
        return result;
    }

    std::unique_ptr<Usb> open(const Device& device, Millis timeout) override {
        return std::make_unique<WinUsb>(device, timeout);
    }

    void doctor(std::ostream& out) const override {
        SYSTEM_INFO info{};
        GetNativeSystemInfo(&info);
        out << "Windows architecture=" << (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ?
                                            "x64" : "other") << '\n';
        for (const auto* dll : {L"winusb.dll", L"setupapi.dll", L"cfgmgr32.dll", L"ws2_32.dll"}) {
            out << utf8(dll) << '=' << (GetModuleHandleW(dll) ? "loaded" : "missing") << '\n';
        }
    }
};
}

std::unique_ptr<Backend> make_winusb() { return std::make_unique<WinBackend>(); }
}

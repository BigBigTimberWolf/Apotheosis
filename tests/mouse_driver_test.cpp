#include <cassert>
#include <iostream>
#include <string>

#include "mouse/mouse_driver.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL: " #expr " at " __FILE__ ":" << __LINE__ << "\n"; \
        return 1; \
    } \
} while(0)

int main()
{
    using namespace mouse_driver;

    std::cout << "[1] 后端名称列表必须包含六种方式...\n";
    {
        const auto names = backendNames();
        CHECK(names.size() == 6);
        CHECK(names[0] == kBackendMakcu);
        CHECK(names[1] == kBackendMakcuNew);
        CHECK(names[2] == kBackendKmboxNet);
        CHECK(names[3] == kBackendFerrum);
        CHECK(names[4] == kBackendDhzboxMini);
        CHECK(names[5] == "WINDOWS");
    }

    std::cout << "[2] 能力位中文描述...\n";
    {
        const std::string desc = describeCapabilities(kCapMove | kCapButtonLeft | kCapKeyboard);
        CHECK(desc.find(u8"位移") != std::string::npos);
        CHECK(desc.find(u8"左键") != std::string::npos);
        CHECK(desc.find(u8"键盘") != std::string::npos);
        CHECK(desc.find(u8"滚轮") == std::string::npos);
        CHECK(describeCapabilities(0) == u8"无");
    }

    std::cout << "[3] 状态字符串渲染 (连不上必须带得出证据)...\n";
    {
        const std::string s_fail = describeStatus("KMBOXNET", false, "192.168.2.88:6234 超时");
        CHECK(s_fail.find(u8"不可用") != std::string::npos);
        CHECK(s_fail.find("192.168.2.88") != std::string::npos);
        CHECK(s_fail.find(u8"超时") != std::string::npos);

        const std::string s_ok = describeStatus("MAKCUNEW", true, "COM3@6000000");
        CHECK(s_ok.find(u8"已连接") != std::string::npos);
        CHECK(s_ok.find("COM3") != std::string::npos);
    }

    std::cout << "[4] 工厂对不认识的名字拒绝并给出理由 (不静默回落)...\n";
    {
        const auto res = open("KMBOX_BPLUS", "", 0, "", 0, "", "", "");
        CHECK(res.driver == nullptr);
        CHECK(!res.error.empty());
        CHECK(res.error.find(u8"不认识的名字") != std::string::npos);
        CHECK(res.error.find("KMBOX_BPLUS") != std::string::npos);
        CHECK(res.error.find("MAKCU") != std::string::npos);
        CHECK(res.error.find("MAKCUNEW") != std::string::npos);
        CHECK(res.error.find("KMBOXNET") != std::string::npos);
        CHECK(res.error.find("FERRUM") != std::string::npos);
        CHECK(res.error.find("DHZBOX_MINI") != std::string::npos);
    }

    {
        const auto ferrum = open("FERRUM", "", 0, "", 0, "", "", "");
        CHECK(ferrum.driver == nullptr);
        CHECK(!ferrum.error.empty());
        const auto dhzbox = open("DHZBOX_MINI", "", 0, "", 0, "", "", "");
        CHECK(dhzbox.driver == nullptr);
        CHECK(!dhzbox.error.empty());
    }

    std::cout << "[5] KMBOXNET 未填 IP 时拒绝并给出理由...\n";
    {
        const auto res = open("KMBOXNET", "", 0, "", 0, "", "6234", "12345");
        CHECK(res.driver == nullptr);
        CHECK(!res.error.empty());
        CHECK(res.error.find(u8"未填写盒子 IP") != std::string::npos);
    }

    std::cout << "[6] 假连接对象的包装器契约...\n";
    {
        WrappedMakcuDriver d_makcu(nullptr);
        CHECK(std::string(d_makcu.name()) == "MAKCU");
        CHECK(!d_makcu.isOpen());
        CHECK(!d_makcu.move(10, 20));
        CHECK(!d_makcu.leftDown());
        CHECK(!d_makcu.tapKey(0x1A, 50, 0));
        CHECK(d_makcu.physicalButtonPressed(1) == -1);
        CHECK((d_makcu.capabilities() & kCapKeyboard) == 0);
        CHECK((d_makcu.capabilities() & kCapKeyboardMask) == 0);
        CHECK((d_makcu.capabilities() & kCapMove) != 0);

        // 双硬件构造: (鼠标那台, 键盘那台)。两者都可以为 null。
        WrappedMakcuNewDriver d_new(nullptr, nullptr);
        CHECK(std::string(d_new.name()) == "MAKCUNEW");
        CHECK(!d_new.isOpen());
        CHECK(!d_new.move(10, 20));
        CHECK(!d_new.tapKey(0x1A, 50, 0));
        CHECK(d_new.directSend());

        // 没有键盘硬件时:
        //   - 能力位不得声明键盘(否则上层会一路走到 tapKey 才发现不能用);
        //   - 键盘动作必须失败, 且【绝不回落】到鼠标那台(否则屏蔽会误伤鼠标输入);
        //   - 鼠标能力必须完好无损。
        CHECK((d_new.capabilities() & kCapKeyboard) == 0);
        CHECK((d_new.capabilities() & kCapKeyboardMask) == 0);
        CHECK((d_new.capabilities() & kCapMove) != 0);
        CHECK((d_new.capabilities() & kCapWheel) != 0);
        CHECK((d_new.capabilities() & kCapButtonLeft) != 0);
        CHECK(d_new.keyboardConnection() == nullptr);
        CHECK(!d_new.maskRealKeyboard(50));

        WrappedKmboxNetDriver d_km(nullptr);
        CHECK(std::string(d_km.name()) == "KMBOXNET");
        CHECK(!d_km.isOpen());
        CHECK(!d_km.move(10, 20));
        CHECK((d_km.capabilities() & kCapKeyboard) != 0);
        CHECK((d_km.capabilities() & kCapKeyboardMask) != 0);
        CHECK(!d_km.maskRealKeyboard(2000));
        CHECK(d_km.directSend());
    }

    std::cout << "All mouse_driver unit tests passed.\n";
    return 0;
}

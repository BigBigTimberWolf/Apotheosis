path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

anchor = """            endpoint_descriptors[endpointCounter].wMaxPacketSize = ep_desc->wMaxPacketSize;
            endpoint_descriptors[endpointCounter].bInterval = ep_desc->bInterval;
"""

assert anchor in src, "endpoint descriptor anchor not found"

addition = anchor + """
            // 记录"这个端点属于哪个接口" —— 复合 HID 透传的关键依据。
            //
            // currentInterfaceNumber 由上一个 USB_INTERFACE_DESC 分支设置, 而
            // 端点描述符总是紧跟在它所属的接口描述符之后, 所以此刻它正是
            // 本端点的归属接口。用端点地址低 4 位做下标, 与 _onReceive 里
            // 的 epSlot() 保持一致。
            {
                const uint8_t slot = epSlot(ep_desc->bEndpointAddress);
                if (slot < kEpSlotCount) {
                    ep_owner[slot].iface    = currentInterfaceNumber;
                    ep_owner[slot].isIn     = (ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;
                    ep_owner[slot].xferType = (uint8_t)(ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK);
                    ep_owner[slot].used     = true;
                    ESP_LOGI("EspUsbHost", "ep 0x%02x -> iface %u (IN=%d, type=%u)",
                             ep_desc->bEndpointAddress, currentInterfaceNumber,
                             (int)ep_owner[slot].isIn, ep_owner[slot].xferType);
                }
            }
"""

src = src.replace(anchor, addition, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("ep_owner populated in onConfig endpoint branch")

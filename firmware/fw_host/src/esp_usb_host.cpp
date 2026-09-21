#include "EspUsbHost.h"

// ===== 链路计数器 (诊断用; 由 diag.cpp 的 DIAG 行报出) =====
//   每一级都计数, 这样任何一级为 0 就能直接定位卡点。
uint32_t volatile g_rxReports   = 0;   // 端点收到的报文总数
uint32_t volatile g_rxMouseGate = 0;   // 通过"鼠标端点"门控的报文数
uint32_t volatile g_rxDecoded   = 0;   // 通过 reportId/layout 校验并解码成功的
uint32_t volatile g_rxFwd       = 0;   // 实际转发给左板的位移
uint32_t volatile g_rxNotMouse  = 0;   // 非鼠标端点的报文

// 说明: 这里原先还有一组"键盘影子副本"(s_kbdRawLast / s_kbdRawLastValid /
// s_kbdRawLastMs)和"当前转发的键盘接口号"(s_kbdFwdIface) —— 它们是旧键盘原样
// 转发方案的屏蔽安全依据。键盘转发已整体删除(键盘由独立固件负责), 故一并删除。
#include <sstream>
#include <iomanip>
#include "freertos/semphr.h"

#define USB_FEATURE_SELECTOR_REMOTE_WAKEUP 1

bool EspUsbHost::deviceMouseReady = false;
bool EspUsbHost::deviceConnected = false;
void flashLED();


void usbLibraryTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true)
    {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (err != ESP_OK)
        {
            ESP_LOGE("EspUsbHost", "usb_host_lib_handle_events() err=%x", err);
            continue;
        }

        if (instance->clientHandle == NULL || !instance->isClientRegistering)
        {
            ESP_LOGI("EspUsbHost", "Registering client...");
            const usb_host_client_config_t client_config = {
                .max_num_event_msg = 10,
                .async = {
                    .client_event_callback = instance->_clientEventCallback,
                    .callback_arg = instance,
                }};

            err = usb_host_client_register(&client_config, &instance->clientHandle);
            ESP_LOGI("EspUsbHost", "usb_host_client_register() status: %d", err);
            if (err != ESP_OK)
            {
                ESP_LOGW("EspUsbHost", "Failed to re-register client, retrying...");
                vTaskDelay(100);
            }
            else
            {
                ESP_LOGI("EspUsbHost", "Client registered successfully.");
                instance->isClientRegistering = true;
            }
        }
    }
}

void usbClientTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true)
    {
        if (!instance->isClientRegistering)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        usb_host_client_handle_events(instance->clientHandle, portMAX_DELAY);
    }
}



void EspUsbHost::get_device_status()
{
    const char *TAG = "get_device_status";

    if (!EspUsbHost::deviceConnected)
    {
        return;
    }

    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(8 + 2, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "usb_host_transfer_alloc() err=%X", err);
        return;
    }

    transfer->num_bytes = 8 + 2;
    transfer->data_buffer[0] = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    transfer->data_buffer[1] = USB_B_REQUEST_GET_STATUS;
    transfer->data_buffer[2] = 0x00;
    transfer->data_buffer[3] = 0x00;
    transfer->data_buffer[4] = 0x00;
    transfer->data_buffer[5] = 0x00;
    transfer->data_buffer[6] = 0x02;
    transfer->data_buffer[7] = 0x00;

    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0x00;

    transfer->callback = [](usb_transfer_t *transfer)
    {
        const char *TAG = "get_device_status_callback";
        EspUsbHost *usbHost = static_cast<EspUsbHost *>(transfer->context);

        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED)
        {
            uint16_t status = (transfer->data_buffer[9] << 8) | transfer->data_buffer[8];
            ESP_LOGI(TAG, "Device status: %X", status);

            if (status & (1 << USB_FEATURE_SELECTOR_REMOTE_WAKEUP))
            {
                ESP_LOGI(TAG, "Remote Wakeup is enabled.");
            }
            else
            {
                ESP_LOGI(TAG, "Remote Wakeup is disabled.");
            }
        }
        else
        {
            ESP_LOGE(TAG, "GET_STATUS transfer failed with status=%X", transfer->status);
        }

        usb_host_transfer_free(transfer);
    };

    transfer->context = this;

    err = usb_host_transfer_submit_control(clientHandle, transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_host_transfer_submit_control() err=%X", err);
        usb_host_transfer_free(transfer);
    }
}

void EspUsbHost::suspend_device()
{
    const char *TAG = "suspend_device";

    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(8 + 1, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "usb_host_transfer_alloc() err=%X", err);
        return;
    }

    transfer->num_bytes = 8;
    transfer->data_buffer[0] = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    transfer->data_buffer[1] = USB_B_REQUEST_SET_FEATURE;
    transfer->data_buffer[2] = USB_FEATURE_SELECTOR_REMOTE_WAKEUP;
    transfer->data_buffer[3] = 0x00;
    transfer->data_buffer[4] = 0x00;
    transfer->data_buffer[5] = 0x00;
    transfer->data_buffer[6] = 0x00;
    transfer->data_buffer[7] = 0x00;

    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0x00;
    transfer->callback = _onReceiveControl;
    // 注意: 本传输【不】取描述符, 只是 SET_FEATURE 的标准控制请求。
    // _onReceiveControl 现在按 ControlContext* 解引用 context, 因此这里绝不能
    // 再传裸 this —— 那会被解释成 ctx->host 读到对象首字节(debugModeActive=0),
    // 得到 nullptr 后静默 return, 后续 get_device_status 永远发不出去。
    // 用一个专用槽位承载"非描述符控制传输"的上下文。
    controlCtx[kCtlCtxMisc].host  = this;
    controlCtx[kCtlCtxMisc].iface = 0xFF;   // 0xFF = 非 HID 描述符请求, 回调直接放行
    transfer->context = &controlCtx[kCtlCtxMisc];

    err = usb_host_transfer_submit_control(clientHandle, transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_host_transfer_submit_control() err=%X", err);
        usb_host_transfer_free(transfer);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    deviceSuspended = true;

    ESP_LOGI(TAG, "Device suspended successfully.");

    get_device_status();
}


void EspUsbHost::resume_device()
{
    const char *TAG = "resume_device";

    if (!EspUsbHost::deviceConnected)
    {
        return;
    }

    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(8 + 1, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "usb_host_transfer_alloc() err=%X", err);
        return;
    }

    transfer->num_bytes = 8;
    transfer->data_buffer[0] = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    transfer->data_buffer[1] = USB_B_REQUEST_CLEAR_FEATURE;
    transfer->data_buffer[2] = USB_FEATURE_SELECTOR_REMOTE_WAKEUP;
    transfer->data_buffer[3] = 0x00;
    transfer->data_buffer[4] = 0x00;
    transfer->data_buffer[5] = 0x00;
    transfer->data_buffer[6] = 0x00;
    transfer->data_buffer[7] = 0x00;

    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0x00;
    transfer->callback = _onReceiveControl;
    // 同 suspend_device(): 必须用 ControlContext, 不能传裸 this。
    controlCtx[kCtlCtxMisc].host  = this;
    controlCtx[kCtlCtxMisc].iface = 0xFF;
    transfer->context = &controlCtx[kCtlCtxMisc];

    err = usb_host_transfer_submit_control(clientHandle, transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_host_transfer_submit_control() err=%X", err);
        usb_host_transfer_free(transfer);
        return;
    }

    deviceSuspended = false;

    ESP_LOGI(TAG, "Device resumed successfully.");

    get_device_status();
}

String EspUsbHost::getUsbDescString(const usb_str_desc_t *str_desc)
{
    String str = "";
    if (str_desc == NULL)
    {
        return str;
    }

    for (int i = 0; i < str_desc->bLength / 2; i++)
    {
        if (str_desc->wData[i] > 0xFF)
        {
            continue;
        }
        str += char(str_desc->wData[i]);
    }
    return str;
}


// 原始字节 hex 打印。
//
// 原实现每次调用都构造 std::stringstream + std::string(两次堆分配), 而它在
// 描述符回调与 HID 报文热路径上被无条件调用 —— 即使 ESP_LOGI 的日志级别被裁掉,
// 参数求值也照样发生。现改为栈上定长缓冲 + 直接格式化, 零堆分配、无 STL 依赖。
//
// 超过 kMaxLogBytes 的部分截断打印(描述符最长 512B, 报文很短, 足够诊断)。
void EspUsbHost::logRawBytes(const char *functionName, const uint8_t *data, uint16_t length)
{
    const uint16_t kMaxLogBytes = 128;
    char buf[kMaxLogBytes * 3 + 24];
    int n = 0;

    n += snprintf(buf + n, sizeof(buf) - n, "Raw Bytes(%u):", (unsigned)length);

    const uint16_t shown = (length > kMaxLogBytes) ? kMaxLogBytes : length;
    for (uint16_t i = 0; i < shown; ++i)
    {
        if ((int)sizeof(buf) - n < 4) break;   // 留出 "XX \0"
        n += snprintf(buf + n, sizeof(buf) - n, " %02X", (unsigned)data[i]);
    }
    if (shown < length)
    {
        snprintf(buf + n, sizeof(buf) - n, " ...");
    }

    ESP_LOGI(functionName, "%s", buf);
}

void EspUsbHost::onConfig(const uint8_t bDescriptorType, const uint8_t *p)
{
    static uint8_t currentInterfaceNumber;

    // 描述符原始字节只在调试模式下打印: 它会遍历整个配置描述符,
    // 非调试时纯属浪费(而且每个描述符还会各打一次)。
    if (debugModeActive) {
        logRawBytes("EspUsbHost::onConfig", p, p[0]);
    }

    switch (bDescriptorType)
    {
    case USB_DEVICE_DESC:
    {
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_DEVICE_DESC");

        const usb_device_desc_t *dev_desc = (const usb_device_desc_t *)p;

        // Assign fields to ensure type compatibility
        descriptor_device.bLength = dev_desc->bLength;
        descriptor_device.bDescriptorType = dev_desc->bDescriptorType;
        descriptor_device.bcdUSB = dev_desc->bcdUSB;
        descriptor_device.bDeviceClass = dev_desc->bDeviceClass;
        descriptor_device.bDeviceSubClass = dev_desc->bDeviceSubClass;
        descriptor_device.bDeviceProtocol = dev_desc->bDeviceProtocol;
        descriptor_device.bMaxPacketSize0 = dev_desc->bMaxPacketSize0;
        descriptor_device.idVendor = dev_desc->idVendor;
        descriptor_device.idProduct = dev_desc->idProduct;
        descriptor_device.bcdDevice = dev_desc->bcdDevice;
        descriptor_device.iManufacturer = dev_desc->iManufacturer;
        descriptor_device.iProduct = dev_desc->iProduct;
        descriptor_device.iSerialNumber = dev_desc->iSerialNumber;
        descriptor_device.bNumConfigurations = dev_desc->bNumConfigurations;

        break;
    }

    case USB_STRING_DESC:
    {
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_STRING_DESC");

        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)p;
        usb_string_descriptor_t usbStringDescriptor;
        usbStringDescriptor.bLength = desc->bLength;
        usbStringDescriptor.bDescriptorType = desc->bDescriptorType;
        usbStringDescriptor.data = "";

        for (int i = 0; i < (desc->bLength - 2); i++)
        {
            if (desc->val[i] < 16)
            {
                usbStringDescriptor.data += "0";
            }
            usbStringDescriptor.data += String(desc->val[i], HEX) + " ";
        }

        break;
    }

    case USB_INTERFACE_DESC:
    {
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_INTERFACE_DESC");

        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;

        // 保存接口描述符原始字段。
        // 原来这里只填了 endpoint_data_list[], 而 sendInterfaceDescriptors()
        // 读的是 interface_descriptors[] —— 那个数组在主机侧从未被写过,
        // 于是上报给上位机的接口列表(class/protocol)恒为 0。
        if (interfaceCounter < MAX_INTERFACE_DESCRIPTORS)
        {
            interface_descriptors[interfaceCounter].bLength            = intf->bLength;
            interface_descriptors[interfaceCounter].bDescriptorType    = intf->bDescriptorType;
            interface_descriptors[interfaceCounter].bInterfaceNumber   = intf->bInterfaceNumber;
            interface_descriptors[interfaceCounter].bAlternateSetting  = intf->bAlternateSetting;
            interface_descriptors[interfaceCounter].bNumEndpoints      = intf->bNumEndpoints;
            interface_descriptors[interfaceCounter].bInterfaceClass    = intf->bInterfaceClass;
            interface_descriptors[interfaceCounter].bInterfaceSubClass = intf->bInterfaceSubClass;
            interface_descriptors[interfaceCounter].bInterfaceProtocol = intf->bInterfaceProtocol;
            interface_descriptors[interfaceCounter].iInterface         = intf->iInterface;
        }

        if (interfaceCounter < MAX_INTERFACE_DESCRIPTORS)
        {
            this->claim_err = usb_host_interface_claim(this->clientHandle, this->deviceHandle, intf->bInterfaceNumber, intf->bAlternateSetting);
            if (this->claim_err != ESP_OK)
            {
                ESP_LOGE("EspUsbHost", "usb_host_interface_claim() err=%x", this->claim_err);
            }
            else
            {
                ESP_LOGI("EspUsbHost", "usb_host_interface_claim() successful for interface %d", intf->bInterfaceNumber);
                this->usbInterface[this->usbInterfaceSize] = intf->bInterfaceNumber;
                this->usbInterfaceSize++;

                currentInterfaceNumber = intf->bInterfaceNumber;
                endpoint_data_list[currentInterfaceNumber].bInterfaceNumber = intf->bInterfaceNumber;
                endpoint_data_list[currentInterfaceNumber].bInterfaceClass = intf->bInterfaceClass;
                endpoint_data_list[currentInterfaceNumber].bInterfaceSubClass = intf->bInterfaceSubClass;
                endpoint_data_list[currentInterfaceNumber].bInterfaceProtocol = intf->bInterfaceProtocol;
                endpoint_data_list[currentInterfaceNumber].bCountryCode = 0; // FIX THIS !!
            }

            interfaceCounter++;
        }

        break;
    }

    case USB_ENDPOINT_DESC:
    {
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_ENDPOINT_DESC");

        const usb_ep_desc_t *ep_desc = (const usb_ep_desc_t *)p;

        if (endpointCounter < MAX_ENDPOINT_DESCRIPTORS)
        {
            endpoint_descriptors[endpointCounter].bLength = ep_desc->bLength;
            endpoint_descriptors[endpointCounter].bDescriptorType = ep_desc->bDescriptorType;
            endpoint_descriptors[endpointCounter].bEndpointAddress = ep_desc->bEndpointAddress;
            endpoint_descriptors[endpointCounter].endpointID = USB_EP_DESC_GET_EP_NUM(ep_desc);
            endpoint_descriptors[endpointCounter].direction = USB_EP_DESC_GET_EP_DIR(ep_desc) ? "IN" : "OUT";
            endpoint_descriptors[endpointCounter].bmAttributes = ep_desc->bmAttributes;
            endpoint_descriptors[endpointCounter].attributes =
                (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_CONTROL ? "CTRL" :
                (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_ISOC ? "ISOC" :
                (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK ? "BULK" :
                "Interrupt";

            endpoint_descriptors[endpointCounter].wMaxPacketSize = ep_desc->wMaxPacketSize;
            endpoint_descriptors[endpointCounter].bInterval = ep_desc->bInterval;

            if (this->claim_err != ESP_OK)
            {
                ESP_LOGW("EspUsbHost", "Skipping endpoint due to claim_err.");
                return;
            }

            // 注意: 这里**不再**往 endpoint_data_list[] 写。
            // 原来它按【端点号】写入, 而接口描述符分支按【接口号】写入同一个数组,
            // 两个下标空间重叠 => 端点记录会覆盖接口记录, 而 serialization.cpp 的
            // sendInterfaceDescriptors() 正是按接口号读它的 => 上报给上位机的
            // 接口列表(class/protocol)是错的。端点归属改由下面的 ep_owner[] 承载。
            const uint8_t slot = epSlot(ep_desc->bEndpointAddress);
            this->ep_owner[slot].iface    = currentInterfaceNumber;
            this->ep_owner[slot].isIn     = (ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;
            this->ep_owner[slot].xferType = (uint8_t)(ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK);
            this->ep_owner[slot].used     = true;

            if ((ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_INT)
            {
                ESP_LOGE("EspUsbHost", "Unsupported transfer type: bmAttributes=%x", ep_desc->bmAttributes);
                return;
            }

            if (ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)
            {
                esp_err_t err = usb_host_transfer_alloc(ep_desc->wMaxPacketSize + 1, 0, &this->usbTransfer[this->usbTransferSize]);
                if (err != ESP_OK)
                {
                    this->usbTransfer[this->usbTransferSize] = NULL;
                    ESP_LOGE("EspUsbHost", "usb_host_transfer_alloc() failed with err=%x", err);
                    return;
                }
                else
                {
                    ESP_LOGI("EspUsbHost", "usb_host_transfer_alloc() successful, data_buffer_size=%d", ep_desc->wMaxPacketSize + 1);
                }

                this->usbTransfer[this->usbTransferSize]->device_handle = this->deviceHandle;
                this->usbTransfer[this->usbTransferSize]->bEndpointAddress = ep_desc->bEndpointAddress;
                this->usbTransfer[this->usbTransferSize]->callback = this->_onReceive;
                this->usbTransfer[this->usbTransferSize]->context = this;
                this->usbTransfer[this->usbTransferSize]->num_bytes = ep_desc->wMaxPacketSize;
                // 原此处有 `interval = ep_desc->bInterval;` —— interval 是只写不读的
                // 死成员(主机侧轮询节奏由 USB Host 驱动按端点描述符自行处理),
                // 已随该成员一并删除。
                isReady = true;
                this->usbTransferSize++;

                ESP_LOGI("EspUsbHost", "Submitting transfer for endpoint 0x%x", ep_desc->bEndpointAddress);

                err = usb_host_transfer_submit(this->usbTransfer[this->usbTransferSize - 1]);
                if (err != ESP_OK)
                {
                    ESP_LOGE("EspUsbHost", "usb_host_transfer_submit() failed with err=%x", err);
                }
                else
                {
                    // ★ deviceMouseReady 是所有下行数据(鼠标报文/按键/位移)的总闸。
                    //
                    // 【原来只由 "USB_INIT" 命令置位】一旦命令时序错过, 或之后
                    // 设备重枚举触发 DEV_GONE 把它清零, 这个闸就永久关着 ——
                    // 现象是"两块板都活着、板间链路通, 但一个字节数据都不发"。
                    //
                    // 端点已成功提交 => 本板确实在读取该设备 => 就绪。
                    // 这样它反映【实际状态】而不是命令时机, 断线时 DEV_GONE 仍会清零。
                    deviceMouseReady = true;
                }
            }

            endpointCounter++;
        }

        break;
    }

    case USB_INTERFACE_ASSOC_DESC:
    {
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_INTERFACE_ASSOC_DESC");

        const usb_iad_desc_t *iad_desc = (const usb_iad_desc_t *)p;

        descriptor_interface_association.bLength = iad_desc->bLength;
        descriptor_interface_association.bDescriptorType = iad_desc->bDescriptorType;
        descriptor_interface_association.bFirstInterface = iad_desc->bFirstInterface;
        descriptor_interface_association.bInterfaceCount = iad_desc->bInterfaceCount;
        descriptor_interface_association.bFunctionClass = iad_desc->bFunctionClass;
        descriptor_interface_association.bFunctionSubClass = iad_desc->bFunctionSubClass;
        descriptor_interface_association.bFunctionProtocol = iad_desc->bFunctionProtocol;
        descriptor_interface_association.iFunction = iad_desc->iFunction;

        break;
    }

    case USB_HID_DESC:
    {
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_HID_DESC");

        const tusb_hid_descriptor_hid_t *hid_desc = (const tusb_hid_descriptor_hid_t *)p;

        if (hidDescriptorCounter < MAX_HID_DESCRIPTORS)
        {
            hid_descriptors[hidDescriptorCounter].bLength = hid_desc->bLength;
            hid_descriptors[hidDescriptorCounter].bDescriptorType = hid_desc->bDescriptorType;
            hid_descriptors[hidDescriptorCounter].bcdHID = hid_desc->bcdHID;
            hid_descriptors[hidDescriptorCounter].bCountryCode = hid_desc->bCountryCode;
            hid_descriptors[hidDescriptorCounter].bNumDescriptors = hid_desc->bNumDescriptors;
            hid_descriptors[hidDescriptorCounter].bReportType = hid_desc->bReportType;
            hid_descriptors[hidDescriptorCounter].wReportLength = hid_desc->wReportLength;
            endpoint_data_list[currentInterfaceNumber].bCountryCode = hid_descriptors[hidDescriptorCounter].bCountryCode;

            submitControl(0x81, 0x00, 0x22, currentInterfaceNumber, hid_descriptors[hidDescriptorCounter].wReportLength);

            hidDescriptorCounter++;
        }

        break;
    }

    case USB_CONFIGURATION_DESC:
    {
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_CONFIGURATION_DESC");

        const usb_config_desc_t *config_desc = (const usb_config_desc_t *)p;

        configurationDescriptor.bLength = config_desc->bLength;
        configurationDescriptor.bDescriptorType = config_desc->bDescriptorType;
        configurationDescriptor.wTotalLength = config_desc->wTotalLength;
        configurationDescriptor.bNumInterfaces = config_desc->bNumInterfaces;
        configurationDescriptor.bConfigurationValue = config_desc->bConfigurationValue;
        configurationDescriptor.iConfiguration = config_desc->iConfiguration;
        configurationDescriptor.bmAttributes = config_desc->bmAttributes;
        configurationDescriptor.bMaxPower = config_desc->bMaxPower;

        break;
    }

    default:
    {
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: Unknown (0x%02X)", bDescriptorType);
        break;
    }
    }
}


void EspUsbHost::_clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);
    esp_err_t err;

    // 复用成员版 logRawBytes(栈缓冲、零堆分配), 不再自己造 stringstream。
    auto logRawBytes = [usbHost](const char* descriptorType, const uint8_t* data, uint8_t length) {
        usbHost->logRawBytes(descriptorType, data, length);
    };

    switch (eventMsg->event)
    {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
    {
        ESP_LOGI("EspUsbHost::_clientEventCallback", "Device connected");

        EspUsbHost::deviceConnected = true;
        usbHost->endpointCounter = 0;
        usbHost->interfaceCounter = 0;
        usbHost->hidDescriptorCounter = 0;
        usbHost->unknownDescriptorCounter = 0;
        memset(usbHost->endpoint_data_list, 0, sizeof(usbHost->endpoint_data_list));
        // 端点归属 / 各接口的报告描述符 / 选中的鼠标接口, 全部随设备插入清空。
        // 不清会让上一只鼠标的布局残留下来, 新设备用旧布局解码 => 乱移乱点。
        memset(usbHost->ep_owner, 0, sizeof(usbHost->ep_owner));
        memset(usbHost->descPerIface, 0, sizeof(usbHost->descPerIface));
        memset(usbHost->descPerIfaceValid, 0, sizeof(usbHost->descPerIfaceValid));
        memset(usbHost->rawDescPerIface, 0, sizeof(usbHost->rawDescPerIface));
        usbHost->mouseIface = -1;
        // 设备刚插入: 上一只鼠标的按键位图必须作废, 否则新鼠标的第一帧会被拿去和
        // 旧状态做差分, 抬键判定直接失效(表现为按键卡死/漏按)。
        usbHost->lastReportedButtons = 0;

        // 事件结构体原始字节只在调试模式下打印
        if (usbHost->debugModeActive) {
            logRawBytes("New Device Event Message", (const uint8_t *)eventMsg, sizeof(usb_host_client_event_msg_t));
        }

        err = usb_host_device_open(usbHost->clientHandle, eventMsg->new_dev.address, &usbHost->deviceHandle);
        if (err != ESP_OK)
        {
            ESP_LOGE("EspUsbHost", "Failed to open device with address %d. Error: %d", eventMsg->new_dev.address, err);
            return;
        }

        ESP_LOGI("EspUsbHost", "Device opened successfully");

        usb_device_info_t dev_info;
        err = usb_host_device_info(usbHost->deviceHandle, &dev_info);
        if (err == ESP_OK)
        {
            usbHost->device_info.speed = dev_info.speed;
            usbHost->device_info.dev_addr = dev_info.dev_addr;
            usbHost->device_info.vMaxPacketSize0 = dev_info.bMaxPacketSize0;
            usbHost->device_info.bConfigurationValue = dev_info.bConfigurationValue;
            // strlcpy: USB 字符串描述符 bLength 是 uint8_t(<=255), getUsbDescString 最多拼
            // 127 个字符 + NUL = 128 字节, 而目标是 64 字节数组。原来用 strcpy 是确定性溢出,
            // 且三处连续执行会把写入足迹推到 device_info 之外, 砸到后面的 descriptor_device
            // 与 endpoint_descriptors[](其中 String 是非平凡对象) => 内存破坏。
            strlcpy(usbHost->device_info.str_desc_manufacturer, getUsbDescString(dev_info.str_desc_manufacturer).c_str(), sizeof(usbHost->device_info.str_desc_manufacturer));
            strlcpy(usbHost->device_info.str_desc_product, getUsbDescString(dev_info.str_desc_product).c_str(), sizeof(usbHost->device_info.str_desc_product));
            strlcpy(usbHost->device_info.str_desc_serial_num, getUsbDescString(dev_info.str_desc_serial_num).c_str(), sizeof(usbHost->device_info.str_desc_serial_num));

            ESP_LOGI("EspUsbHost", "Device info retrieved successfully");
        }
        else
        {
            ESP_LOGE("EspUsbHost", "Failed to retrieve device info. Error: %d", err);
        }

        const usb_device_desc_t *dev_desc;
        err = usb_host_get_device_descriptor(usbHost->deviceHandle, &dev_desc);
        if (err == ESP_OK)
        {
            ESP_LOGI("EspUsbHost::_clientEventCallback", "Device descriptor retrieved successfully");

            // Field-by-field assignment of the descriptor
            usbHost->descriptor_device.bLength = dev_desc->bLength;
            usbHost->descriptor_device.bDescriptorType = dev_desc->bDescriptorType;
            usbHost->descriptor_device.bcdUSB = dev_desc->bcdUSB;
            usbHost->descriptor_device.bDeviceClass = dev_desc->bDeviceClass;
            usbHost->descriptor_device.bDeviceSubClass = dev_desc->bDeviceSubClass;
            usbHost->descriptor_device.bDeviceProtocol = dev_desc->bDeviceProtocol;
            usbHost->descriptor_device.bMaxPacketSize0 = dev_desc->bMaxPacketSize0;
            usbHost->descriptor_device.idVendor = dev_desc->idVendor;
            usbHost->descriptor_device.idProduct = dev_desc->idProduct;
            usbHost->descriptor_device.bcdDevice = dev_desc->bcdDevice;
            usbHost->descriptor_device.iManufacturer = dev_desc->iManufacturer;
            usbHost->descriptor_device.iProduct = dev_desc->iProduct;
            usbHost->descriptor_device.iSerialNumber = dev_desc->iSerialNumber;
            usbHost->descriptor_device.bNumConfigurations = dev_desc->bNumConfigurations;
        }
        else
        {
            ESP_LOGE("EspUsbHost", "Failed to retrieve device descriptor. Error: %d", err);
        }

        const usb_config_desc_t *config_desc;
        err = usb_host_get_active_config_descriptor(usbHost->deviceHandle, &config_desc);
        if (err == ESP_OK)
        {
            usbHost->descriptor_configuration.bLength = config_desc->bLength;
            usbHost->descriptor_configuration.bDescriptorType = config_desc->bDescriptorType;
            usbHost->descriptor_configuration.wTotalLength = config_desc->wTotalLength;
            usbHost->descriptor_configuration.bNumInterfaces = config_desc->bNumInterfaces;
            usbHost->descriptor_configuration.bConfigurationValue = config_desc->bConfigurationValue;
            usbHost->descriptor_configuration.iConfiguration = config_desc->iConfiguration;
            usbHost->descriptor_configuration.bmAttributes = config_desc->bmAttributes;
            usbHost->descriptor_configuration.bMaxPower = config_desc->bMaxPower * 2;

            ESP_LOGI("EspUsbHost::_clientEventCallback", "Configuration descriptor retrieved successfully");

            usbHost->_configCallback(config_desc);
        }
        else
        {
            ESP_LOGE("EspUsbHost", "Failed to retrieve configuration descriptor. Error: %d", err);
        }

        break;
    }

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
    {
        ESP_LOGI("EspUsbHost::_clientEventCallback", "Device disconnected");

        usbHost->isReady = false;
        EspUsbHost::deviceConnected = false;
        deviceMouseReady = false;

        // 鼠标在"按着键"的状态下被拔出时, 设备侧会一直保持该键按下(游戏里按键卡死)。
        // 这里主动补发一次全部抬键, 再通知 Device 侧进入清理流程。
        if (usbHost->lastReportedButtons != 0)
        {
            static const struct { uint8_t bit; const char *cmd; } kRelease[] = {
                {MOUSE_BUTTON_LEFT,     "km.left(0)\n"},
                {MOUSE_BUTTON_RIGHT,    "km.right(0)\n"},
                {MOUSE_BUTTON_MIDDLE,   "km.middle(0)\n"},
                {MOUSE_BUTTON_FORWARD,  "km.side1(0)\n"},
                {MOUSE_BUTTON_BACKWARD, "km.side2(0)\n"},
            };
            for (const auto &r : kRelease)
            {
                if (usbHost->lastReportedButtons & r.bit) usbHost->serial1Send(r.cmd);
            }
            usbHost->lastReportedButtons = 0;
        }

        // 通知 Device 侧外设已拔出
        usbHost->serial1Send("USB_GOODBYE\n");

        ESP_LOGI("EspUsbHost", "Notifying cleanup task...");
        xTaskNotifyGive(usbHost->cleanupTaskHandle); // Notify the cleanup task to start the process

        break;
    }

    default:
        break;
    }
}

void EspUsbHost::cleanupTask(void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); 

        ESP_LOGI("EspUsbHost", "Starting cleanup...");

        esp_err_t err;

        for (int i = 0; i < usbHost->usbTransferSize; i++) {
            if (usbHost->usbTransfer[i] != NULL) {
                err = usb_host_transfer_free(usbHost->usbTransfer[i]);
                if (err == ESP_OK) {
                    ESP_LOGI("EspUsbHost", "Freed USB transfer at index %d", i);
                    usbHost->usbTransfer[i] = NULL;
                } else {
                    ESP_LOGE("EspUsbHost", "Failed to free USB transfer at index %d. Error: %d", i, err);
                }
            }
        }
        usbHost->usbTransferSize = 0;

        for (int i = 0; i < usbHost->usbInterfaceSize; i++) {
            err = usb_host_interface_release(usbHost->clientHandle, usbHost->deviceHandle, usbHost->usbInterface[i]);
            if (err == ESP_OK) {
                ESP_LOGI("EspUsbHost", "Released USB interface at index %d", i);
                usbHost->usbInterface[i] = 0;
            } else {
                ESP_LOGE("EspUsbHost", "Failed to release USB interface at index %d. Error: %d", i, err);
            }
        }
        usbHost->usbInterfaceSize = 0;

        err = usb_host_device_close(usbHost->clientHandle, usbHost->deviceHandle);
        if (err == ESP_OK) {
            ESP_LOGI("EspUsbHost", "Device closed successfully");
        } else {
            ESP_LOGE("EspUsbHost", "Failed to close device. Error: %d", err);
        }

        usbHost->deviceHandle = NULL;
        usbHost->isReady = false;

        ESP_LOGI("EspUsbHost", "Cleanup completed.");
    }
}


void EspUsbHost::_configCallback(const usb_config_desc_t *config_desc)
{
    const uint8_t *p = &config_desc->val[0];
    uint8_t bLength;

    ESP_LOGI("EspUsbHost", "Starting configuration descriptor processing");

    const uint8_t setup[8] = {
        0x80, 0x06, 0x00, 0x02, 0x00, 0x00, (uint8_t)config_desc->wTotalLength, 0x00};

    for (int i = 0; i < config_desc->wTotalLength; i += bLength, p += bLength)
    {
        bLength = *p;

        if ((i + bLength) <= config_desc->wTotalLength)
        {
            const uint8_t bDescriptorType = *(p + 1);

            ESP_LOGD("EspUsbHost", "Processing descriptor of type: 0x%02X", bDescriptorType);

            this->onConfig(bDescriptorType, p);
        }
        else
        {
            ESP_LOGW("EspUsbHost", "Descriptor length exceeds total configuration length");
            return;
        }
    }

    ESP_LOGI("EspUsbHost", "Completed configuration descriptor processing");
}


void EspUsbHost::_onReceiveControl(usb_transfer_t *transfer)
{
    const ControlContext *ctx = static_cast<const ControlContext *>(transfer->context);
    EspUsbHost *usbHost = ctx ? ctx->host : nullptr;
    if (!usbHost)
    {
        usb_host_transfer_free(transfer);
        return;
    }
    if (!usbHost)
    {
        return;
    }

    if (usbHost->debugModeActive) {
        usbHost->logRawBytes("EspUsbHost::_onReceiveControl", transfer->data_buffer, transfer->actual_num_bytes);
    }

    uint8_t *p = &transfer->data_buffer[8];  // Skip the first 8 bytes for processing
    int totalBytes = transfer->actual_num_bytes;

    ESP_LOGI("EspUsbHost", "onReceiveControl called with %d bytes", totalBytes);

    if (totalBytes <= 8)
    {
        ESP_LOGW("EspUsbHost", "Control transfer carried no descriptor payload (%d bytes)", totalBytes);
        usb_host_transfer_free(transfer);
        return;
    }

    // 接口号由 submitControl() 透过 transfer->context 带过来(见那里的注释)。
    const uint8_t ifaceNum = ctx->iface;

    // 哨兵: kCtlIfaceNone 表示这条控制传输不是"取 HID 报告描述符", 而是
    // suspend/resume 用的标准 SET_FEATURE/CLEAR_FEATURE 请求。它没有接口语义,
    // 数据区也不是描述符, 直接丢弃即可 —— 但必须在这里显式放行, 否则 ifaceNum
    // (0xFF)会被当成接口号去索引 endpoint_data_list[17] => 数组越界读。
    if (ifaceNum == kCtlIfaceNone)
    {
        usb_host_transfer_free(transfer);
        return;
    }

    // 先看接口自身的 class/protocol —— 这才是"是不是鼠标接口"的权威依据,
    // 比在描述符字节流里扫魔数可靠得多。
    const bool ifaceNumValid = (ifaceNum < kMaxIface);
    const bool ifaceIsHid = ifaceNumValid &&
                            (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID);
    const uint8_t ifaceProto = ifaceNumValid
                                   ? usbHost->endpoint_data_list[ifaceNum].bInterfaceProtocol
                                   : (uint8_t)HID_ITF_PROTOCOL_NONE;

    // 说明: 原先这里有一段字节流魔数扫描(找 05 01 09 02 判定"像鼠标"), 但它
    // 的结果 looksLikeMouse 从未参与控制流 —— 下面 candidate 只看 ifaceIsHid。
    // 也就是说该循环在每个 HID 接口的描述符回调上白跑最多 totalBytes 次比较,
    // 唯一的去处是一条日志。已删除, 分类交给 parseHIDReportDescriptor()。

    // 候选接口: 任何 HID 接口都放行解析。
    //
    // 不按 protocol 预先筛除, 一律交给 parseHIDReportDescriptor() 判断:
    // 它按内容(有没有 X/Y 轴、有没有键码数组)分类, 比 protocol 字段可靠
    // (很多设备把 protocol 写成 0, 也有鼠标声明成键盘协议的)。
    //
    // 对鼠标链路这是必需的: 漏解析一个 HID 接口, 它的布局就永远缺失
    // (descPerIfaceValid 恒为假), 该接口会落进 _onReceive 里的"标准 8 位鼠标"
    // 兜底通道 —— 一个键盘类接口被这样兜底, 就会产生乱移。
    const bool candidate = ifaceIsHid;

    if (!candidate)
    {
        // 注意: 这里的 class 取值必须先判 ifaceNum 合法 —— ifaceNum >= kMaxIface 时
        // endpoint_data_list[ifaceNum] 越界(前面已过滤 0xFF 哨兵, 这里再兜一次)。
        ESP_LOGI("EspUsbHost", "iface %u is not a HID interface (class=0x%02X proto=%u), skipping",
                 (unsigned)ifaceNum,
                 (unsigned)(ifaceNumValid ? endpoint_data_list[ifaceNum].bInterfaceClass : 0),
                 (unsigned)ifaceProto);
        usb_host_transfer_free(transfer);
        return;
    }

    ESP_LOGI("EspUsbHost", "Parsing HID report descriptor for iface %u (proto=%u, len=%d)",
             (unsigned)ifaceNum, (unsigned)ifaceProto, (int)(totalBytes - 8));

    // ---- 保存原始报告描述符字节 ----
    // 阶段 4(报文格式克隆)需要把真设备的描述符原样搬给 fw_device, 让被控机
    // 读到的 GET_DESCRIPTOR(0x22) 与我们插入的真设备逐字节一致。
    // 解析出来的结构体只够自己解码用, 重建回字节流必然有差异, 所以必须留原件。
    {
        const int rawLen = totalBytes - 8;
        if (ifaceNum < kMaxIface && rawLen > 0) {
            const int cap = (int)sizeof(usbHost->rawDescPerIface[ifaceNum].bytes);
            const int n = rawLen < cap ? rawLen : cap;
            memcpy(usbHost->rawDescPerIface[ifaceNum].bytes, &transfer->data_buffer[8], (size_t)n);
            usbHost->rawDescPerIface[ifaceNum].len = (uint16_t)n;
            if (rawLen > cap) {
                ESP_LOGW("EspUsbHost::_onReceiveControl",
                         "iface %u report descriptor %d bytes truncated to %d",
                         (unsigned)ifaceNum, rawLen, cap);
            }
        }
    }

    HIDReportDescriptor descriptor =
        usbHost->parseHIDReportDescriptor(&transfer->data_buffer[8], totalBytes - 8, ifaceNum);

    if (!descriptor.valid)
    {
        // 注意: 这里不再拒绝设备。该接口布局无效只是"这个接口用不了",
        // 其它接口(真正的鼠标)仍然可能有效 —— 旧代码在这里就放弃了。
        ESP_LOGW("EspUsbHost::_onReceiveControl",
                 "iface %u layout unsupported, iface ignored",
                 (unsigned)ifaceNum);
    }

    usb_host_transfer_free(transfer);
}


// 说明: 原先这里有一个空的 onMouse()(函数体只有注释), 却在每帧 HID 报文上
// 被无条件调用 —— 一次虚表跳转 + 8 字节结构体按值传递, 对行为零贡献。
// 已连同其声明与调用点一并删除。


void EspUsbHost::onMouseButtons(hid_mouse_report_t report, uint8_t last_buttons)
{
    // 屏蔽窗口内: 真实按键变化一律不转发。
    //
    // 关键: 这里【不能】更新 maskLastButtons。那份快照记录的是"设备侧此刻
    // 认为按着什么", 而屏蔽期间设备侧收不到任何变化 —— 它的认知冻结在屏蔽
    // 生效那一刻。若在这里跟着真实输入更新, maskStop() 就会去释放"用户已经
    // 松开、但设备侧根本不知道松开了"的键, 而真正被设备侧认为按下的那些键
    // 反而不会被释放 => 被控机上按键永久卡死。
    // lastReportedButtons 同步更新, 它表示"本机已知的最后一帧", 用于屏蔽
    // 退出后与新状态做差分。
    if (maskIsActive()) {
        lastReportedButtons = report.buttons;
        return;
    }

    if (deviceMouseReady)
    {
        if (!(last_buttons & MOUSE_BUTTON_LEFT) && (report.buttons & MOUSE_BUTTON_LEFT))
        {
            serial1Send("km.left(1)\n");
        }
        if ((last_buttons & MOUSE_BUTTON_LEFT) && !(report.buttons & MOUSE_BUTTON_LEFT))
        {
            serial1Send("km.left(0)\n");
        }

        if (!(last_buttons & MOUSE_BUTTON_RIGHT) && (report.buttons & MOUSE_BUTTON_RIGHT))
        {
            serial1Send("km.right(1)\n");
        }
        if ((last_buttons & MOUSE_BUTTON_RIGHT) && !(report.buttons & MOUSE_BUTTON_RIGHT))
        {
            serial1Send("km.right(0)\n");
        }

        if (!(last_buttons & MOUSE_BUTTON_MIDDLE) && (report.buttons & MOUSE_BUTTON_MIDDLE))
        {
            serial1Send("km.middle(1)\n");
        }
        if ((last_buttons & MOUSE_BUTTON_MIDDLE) && !(report.buttons & MOUSE_BUTTON_MIDDLE))
        {
            serial1Send("km.middle(0)\n");
        }

        if (!(last_buttons & MOUSE_BUTTON_FORWARD) && (report.buttons & MOUSE_BUTTON_FORWARD))
        {
            serial1Send("km.side1(1)\n");
        }
        if ((last_buttons & MOUSE_BUTTON_FORWARD) && !(report.buttons & MOUSE_BUTTON_FORWARD))
        {
            serial1Send("km.side1(0)\n");
        }

        if (!(last_buttons & MOUSE_BUTTON_BACKWARD) && (report.buttons & MOUSE_BUTTON_BACKWARD))
        {
            serial1Send("km.side2(1)\n");
        }
        if ((last_buttons & MOUSE_BUTTON_BACKWARD) && !(report.buttons & MOUSE_BUTTON_BACKWARD))
        {
            serial1Send("km.side2(0)\n");
        }
    }
}


void EspUsbHost::onMouseMove(hid_mouse_report_t report)
{
    // 屏蔽窗口内丢弃真实位移。相对位移无需补偿: 丢掉就等于"sensor 没动",
    // 恢复后天然连续, 不会造成跳变。滚轮同理(离散事件)。
    if (maskIsActive()) return;

    if (deviceMouseReady)
    {
        if (report.x != 0 || report.y != 0)
        {
            // 已协商成功 -> 二进制 0x01 MOVE(省掉 18 字节文本 + 设备侧 sscanf);
            // 否则保持 ASCII km.move, 与旧设备完全兼容。
            if (binaryMovesEnabled) {
                serial1SendMoveBinary(report.x, report.y);
            } else {
                serial1Send("km.move(%d,%d)\n", report.x, report.y);
            }
        }
        if (report.wheel != 0)
        {
            // 滚轮暂不切二进制: 0x20 WHEEL 的 payload 只有 1 字节, 省得有限,
            // 而 km.wheel 路径已被设备侧验证过。保持 ASCII 以缩小改动面。
            serial1Send("km.wheel(%d)\n", report.wheel);
        }
    }
}


// ============================================================================
// 瞬时屏蔽真实输入
//
// 语义: 在 maskUntilMs 之前, 真实鼠标的任何变化都不转发给设备侧。
// 恢复时把"屏蔽期间被按住的鼠标键"补一个抬起, 避免被控机侧按键卡死。
//
// 三条安全设计见 EspUsbHost.h 的声明处注释。
// ============================================================================
void EspUsbHost::maskStart(uint32_t durationMs)
{
    if (durationMs == 0) durationMs = kMaskDefaultMs;
    if (durationMs > kMaskMaxMs) durationMs = kMaskMaxMs;   // 上限钳制

    // 已经在屏蔽中就拒绝重新计时。
    //
    // 允许重入会让软体(或失控的调用方)通过高频重发 km.mask(2000) 把设备
    // 无限期锁在屏蔽态 —— 那正是"硬超时"要防的事。这里让超时成为绝对的:
    // 一旦进入, 只能等到期或显式 km.mask(0)/km.maskoff, 不能再被延长。
    //
    // maskUntilMs 横跨两条任务: maskStart 在 RxTaskSerial0(km.mask),
    // maskTick/maskStop 在 RxTaskSerial1(硬超时)。下面的检查与写入若不加锁
    // 就是 check-then-act, 且 maskStop 尾段的 `maskUntilMs = 0` 会覆盖掉
    // 本函数刚写进去的未来时刻 —— 表现为"屏蔽被静默取消", 真实输入照常
    // 注入被控机, 正是屏蔽要阻止的事。volatile 不提供原子性, 故用互斥量。
    if (!s_mask_mtx) return;
    if (xSemaphoreTake(s_mask_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;

    // 临界区内复查: 前一个持有者可能刚改了状态
    if (maskUntilMs != 0) { xSemaphoreGive(s_mask_mtx); return; }

    // 进入屏蔽前必须记下"此刻设备侧认为按着什么"。
    //
    // 这一步是整个屏蔽功能的安全关键: 屏蔽期间真实输入被丢弃, 设备侧不会
    // 收到任何抬键, 于是 maskLastButtons 记录的这份状态就是唯一能用来补发
    // 释放的依据。漏了它 -> maskStop() 不发释放命令 ->
    // 被控机上该鼠标键永久卡住(游戏里表现为人物一直射击)。
    // 原实现只在命令处理函数里设这个标志, 导致任何其它调用路径都漏补发。
    maskHeldStateValid = true;
    maskLastButtons = lastReportedButtons;

    maskUntilMs = millis() + durationMs;
    if (maskUntilMs == 0) maskUntilMs = 1;                  // 0 是"未屏蔽"的哨兵

    xSemaphoreGive(s_mask_mtx);
}

void EspUsbHost::maskStop()
{
    // 与 maskStart 共用 s_mask_mtx: 保证"检查 active -> 清标志 -> 补发抬键"
    // 整体不被 maskStart 插入, 否则 maskStart 刚写下的新屏蔽会被这里的
    // `maskUntilMs = 0` 抹掉(屏蔽静默取消)。
    if (!s_mask_mtx) return;
    if (xSemaphoreTake(s_mask_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;

    if (maskUntilMs == 0) { xSemaphoreGive(s_mask_mtx); return; }

    // ---- 先清标志, 再补发抬键 ----
    //
    // 顺序至关重要(原实现的真实缺陷):
    // maskUntilMs = 0 是"屏蔽已解除"的唯一标志, 它必须在【任何串口写之前】完成。
    // 若像原实现那样放在 5 次 serial1Send 之后, 那几次写在 5Mbps 下会阻塞排队,
    // 期间 usbClientTask 上的 onMouseButtons 仍看到 maskIsActive()==true, 于是走
    // `lastReportedButtons = report.buttons; return;` 把基准刷成"此刻真实按着"。
    // 等补发的 km.left(0) 已发出(设备侧已抬起)、基准却记成"按住", 后续差分
    // `!(last_buttons & LEFT) && (report.buttons & LEFT)` 恒假 -> 设备侧永远收不到
    // 重新按下 -> 屏蔽结束后该键静默失灵, 直到用户松手重按。
    const uint8_t snapButtons = maskLastButtons;
    const bool snapValid = maskHeldStateValid;

    maskUntilMs = 0;                  // 先解除: 此后真实输入立即恢复正常转发
    maskHeldStateValid = false;
    lastReportedButtons = 0;

    // ---- 补发抬键: 屏蔽期间按着的东西必须在恢复前放掉 ----
    //
    // 屏蔽期间真实输入被丢弃, 设备侧不会收到任何抬键, 所以它仍然以为
    // snapButtons 里记的那些鼠标键是按下的。必须显式发释放, 否则被控机上
    // 该键永久卡住(游戏里表现为人物一直射击)。
    //
    // 鼠标位移不需要补偿: 相对位移丢掉就等于"sensor 没动", 恢复后天然连续。
    if (snapValid) {
        if (snapButtons & MOUSE_BUTTON_LEFT)    serial1Send("km.left(0)\n");
        if (snapButtons & MOUSE_BUTTON_RIGHT)   serial1Send("km.right(0)\n");
        if (snapButtons & MOUSE_BUTTON_MIDDLE)  serial1Send("km.middle(0)\n");
        if (snapButtons & MOUSE_BUTTON_FORWARD) serial1Send("km.side1(0)\n");
        if (snapButtons & MOUSE_BUTTON_BACKWARD) serial1Send("km.side2(0)\n");
    }

    xSemaphoreGive(s_mask_mtx);
}

void EspUsbHost::maskTick()
{
    // 只读一次快照, 避免 maskStart/maskStop 在两次读之间改变状态导致
    // "active 为真但随后读到 0" 这类撕裂判断。
    if (!s_mask_mtx || xSemaphoreTake(s_mask_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    const uint32_t until = maskUntilMs;
    xSemaphoreGive(s_mask_mtx);

    if (until != 0 && (int32_t)(millis() - until) >= 0) {
        maskStop();
    }
}

// 把 16 位相对位移收进 hid_mouse_report_t 的 int8 字段。
//
// hid_mouse_report_t 由 TinyUSB 定义, x/y 是 int8_t, 装不下 16 位值。原实现
// 直接 (int8_t) 强转 —— 那是【环绕截断】而非截顶: +256 变成 0(看起来没动),
// +511 变成 -1(【反向】移动一格)。这是最坏的一种失效: 方向错了但系统不报错。
//
// 正确做法是饱和截顶: 超范围时钳到 ±127, 方向保持正确、幅度有上限。设备侧
// handleMove 本来就会把大位移拆成多个 ±127 的步进, 因此这里钳位不会"丢位移",
// 只是把一次大跳拆到相邻几帧里 —— 相对位移的累积量依然接近正确。
// 注: 真正的无损方案是把 x/y 改成 int16 一路传到底, 那要动 TinyUSB 的类型与
// 设备侧帧格式, 超出本次修复范围; 钳位消除了"反向"这个致命错误。
static inline int8_t clampAxis16(int16_t v) {
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}


esp_err_t EspUsbHost::submitControl(const uint8_t bmRequestType,
                                    const uint8_t bDescriptorIndex,
                                    const uint8_t bDescriptorType,
                                    const uint16_t wInterfaceNumber,
                                    const uint16_t wDescriptorLength)
{
    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(wDescriptorLength + 8 + 1, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE("EspUsbHost::submitControl", "usb_host_transfer_alloc() failed with err=0x%x", err);
        return err;
    }

    transfer->num_bytes = wDescriptorLength + 8;
    transfer->data_buffer[0] = bmRequestType;
    transfer->data_buffer[1] = 0x06;
    transfer->data_buffer[2] = bDescriptorIndex;
    transfer->data_buffer[3] = bDescriptorType;
    transfer->data_buffer[4] = wInterfaceNumber & 0xff;
    transfer->data_buffer[5] = wInterfaceNumber >> 8;
    transfer->data_buffer[6] = wDescriptorLength & 0xff;
    transfer->data_buffer[7] = wDescriptorLength >> 8;

    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0x00;
    transfer->callback = _onReceiveControl;

    // 把"该描述符属于哪个接口"带给异步回调。
    // 回调里没法再知道发起时的接口号, 而复合设备会连续发起多个接口的
    // GET_DESCRIPTOR(0x22), 缺了它就只能全局猜 —— 那正是旧代码用静态
    // HIDReportDesc 被覆盖的根因。
    // 注意: usb_transfer_s 里没有 bInterval 字段(它只存在于端点描述符),
    // 所以用 context 指向一个预分配的小结构同时携带对象指针与接口号。
    const uint8_t ctxIface = (wInterfaceNumber < kMaxIface)
                                 ? (uint8_t)wInterfaceNumber
                                 : (uint8_t)(kMaxIface - 1);
    controlCtx[ctxIface].host  = this;
    controlCtx[ctxIface].iface = ctxIface;
    transfer->context = &controlCtx[ctxIface];

    // 只在调试模式下打印原始字节, 避免每次发控制传输都格式化一遍数据。
    if (debugModeActive) {
        logRawBytes("EspUsbHost::submitControl", transfer->data_buffer, transfer->num_bytes);
    }

    ESP_LOGI("EspUsbHost", "Submitting control transfer, bmRequestType=0x%02x, bDescriptorIndex=0x%02x, bDescriptorType=0x%02x, wInterfaceNumber=0x%04x, wDescriptorLength=%d",
             bmRequestType, bDescriptorIndex, bDescriptorType, wInterfaceNumber, wDescriptorLength);

    err = usb_host_transfer_submit_control(clientHandle, transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE("EspUsbHost", "usb_host_transfer_submit_control() failed with err=%x", err);
        usb_host_transfer_free(transfer);
    }
    else
    {
        ESP_LOGI("EspUsbHost", "Control transfer submitted successfully");
    }

    return err;
}

// ============================================================================
// 以下为 HID 报表解析与报表解码. 重建自审计前的实现, 并按审计结论修复:
//   - 解析器越界读 / 轴宽误判 / X-Y usage 互相覆盖 / 无界偏移
//   - _onReceive 用设备可控的偏移直接索引 data_buffer
// ============================================================================

EspUsbHost::HIDReportDescriptor EspUsbHost::parseHIDReportDescriptor(uint8_t *data, int length, uint8_t ifaceNum)
{
    // ============================================================
    // 重写说明(原实现的缺陷):
    //  1) 越界读: `getValue(data + i + 1, size, ...)` 从不校验 i+1+size <= length,
    //     描述符末尾若是被截断的 1/2 字节 item 就会读到缓冲区之外。
    //     -> 现在先把描述符拷进一个多留 4 字节的尾部缓冲再解析。
    //  2) 轴宽用 LOGICAL_MAXIMUM 猜: 1 字节的 LOGICAL_MAXIMUM 只写 logicalMax8,
    //     而判定读的是 16 位 logicalMax(恒为 0) -> 永远走 12-bit 分支,
    //     标准 8-bit 鼠标被按 12-bit 解码, 于是 Y 是垃圾、滚轮读到未写过的填充字节。
    //     -> 现在轴宽直接取 REPORT_SIZE。
    //  3) X/Y 用同一个 usage 字段: `09 30 09 31` 会互相覆盖, 只能识别出 Y,
    //     且两个轴都从同一个 INPUT 项的偏移推导。
    //     -> 现在按位记录"本 INPUT 项里出现过哪些 usage"。
    //  4) USAGE_MINIMUM/USAGE_MAXIMUM 从未赋值, 按钮识别只能靠 usage 恰好等于 1。
    //     -> 现在显式解析 0x18/0x28, 按钮判定接受 usage 或 usageMinimum。
    //  5) 4 字节 item 被静默当成 0 (getValue 没有 size==4 分支)。
    //  6) 全程无日志守卫, ESP_LOGD 的参数表达式在热路径上照样构造 stringstream。
    // ============================================================
    HIDReportDescriptor desc = {0};

    if (!data || length <= 0) return desc;

    // 尾部补齐: 保证解析器最多多读 4 字节也是安全的
    uint8_t buf[512 + 4];
    const int kMaxDesc = 512;
    int n = (length > kMaxDesc) ? kMaxDesc : length;
    memcpy(buf, data, n);
    memset(buf + n, 0, 4);

    if (debugModeActive) {
        logRawBytes("EspUsbHost::parseHIDReportDescriptor", buf, (uint16_t)n);
    }

    struct HIDItem {
        uint8_t size;      // 载荷字节数 0..4
        uint8_t item;      // prefix & 0xFC
        int32_t value;     // 载荷按小端取出, 再按有符号 item 做符号扩展
    };

    auto readItem = [&](int idx, HIDItem &out) -> bool {
        if (idx >= n) return false;
        const uint8_t prefix = buf[idx];
        out.size = (uint8_t)(prefix & 0x03);
        if (out.size == 3) out.size = 4;
        out.item = (uint8_t)(prefix & 0xFC);
        if (idx + 1 + (int)out.size > n) return false;   // <- 关键: 截断的 item 直接停止解析

        int32_t v = 0;
        for (uint8_t k = 0; k < out.size && k < 4; ++k) {
            v |= (int32_t)((uint32_t)buf[idx + 1 + k] << (8 * k));
        }
        const bool isSigned = (out.item == 0x14 || out.item == 0x24);
        if (isSigned && out.size == 1)      v = (int8_t)v;
        else if (isSigned && out.size == 2) v = (int16_t)v;
        out.value = v;
        return true;
    };

    uint32_t bitOffset = 0;        // 报表内当前位偏移
    uint32_t reportSize = 0;       // REPORT_SIZE (位)
    uint32_t reportCount = 0;      // REPORT_COUNT
    uint8_t  usagePage = 0;
    uint8_t  usage = 0;
    uint8_t  usageMinimum = 0;
    uint8_t  usageMaximum = 0;     // USAGE_MAXIMUM(0x28): 判定 "USAGE_MIN=0x30,MAX=0x31" 写法
    uint8_t  seenXY = 0;           // bit0: 本 INPUT 项里出现过 X(0x30), bit1: Y(0x31)
    bool     haveX = false, haveY = false, haveWheel = false, haveButtons = false;

    const uint32_t kMaxBits = 512u * 8u;
    const uint8_t  kMaxByte = (uint8_t)(kMaxDesc - 1);

    int i = 0;
    while (i < n) {
        HIDItem it;
        if (!readItem(i, it)) break;
        i += (int)it.size + 1;

        switch (it.item) {
        case 0x04: usagePage = (uint8_t)it.value; break;
        case 0x08:                                            // USAGE
            usage = (uint8_t)it.value;
            // ★ 关键: 在【解析到 USAGE 的当下】就记下这是 X 还是 Y, 不能等到 INPUT 项。
            // 标准鼠标描述符是 `09 30 09 31 81 06`: X 与 Y 两个 USAGE 共用一个 INPUT。
            // 逐 item 扫描到 INPUT 时 usage 里只剩最后一个值(Y=0x31), X 已被覆盖 ——
            // 于是旧实现只认 Y, xAxisSize 恒为 0, 下游 looksLikeMouse 永不成立,
            // mouseIface 恒为 -1, 所有鼠标报文被静默丢弃(表现为灯在闪但光标不动)。
            if (usagePage == 0x01) {
                if (it.value == 0x30) seenXY |= 0x01;   // X
                if (it.value == 0x31) seenXY |= 0x02;   // Y
            }
            break;
        case 0x18: usageMinimum = (uint8_t)it.value; break;   // USAGE_MINIMUM(原实现完全忽略)
        case 0x28:
            usageMaximum = (uint8_t)it.value;                 // USAGE_MAXIMUM
            // 允许 `19 30 29 31`(USAGE_MIN=0x30, USAGE_MAX=0x31)这种成对写法
            if (usagePage == 0x01 && it.value == 0x30) seenXY |= 0x01;
            if (usagePage == 0x01 && it.value == 0x31) seenXY |= 0x02;
            break;
        case 0x84:                                            // REPORT_ID
            desc.reportId = (uint8_t)it.value;
            desc.hasReportId = true;
            bitOffset += 8;
            break;
        case 0x74: reportSize = (uint32_t)it.value; break;
        case 0x94: reportCount = (uint32_t)it.value; break;

        case 0x80:   // INPUT
        {
            // 按钮: Usage Page 0x09(Button), 且 usage/usageMinimum 落在 1..16
            if (usagePage == 0x09 && !haveButtons &&
                ((usage >= 1 && usage <= 16) || (usageMinimum >= 1 && usageMinimum <= 16))) {
                desc.buttonBitOffset = (uint16_t)bitOffset;
                desc.buttonStartByte = (uint8_t)(bitOffset / 8);
                desc.buttonSize = (uint8_t)(reportSize * reportCount);
                haveButtons = true;
                bitOffset += reportSize * reportCount;
                if (bitOffset > kMaxBits) bitOffset = kMaxBits;
                break;
            }

            // X/Y 轴: 标准写法把 `09 30 09 31` 放在同一个 81 06 之前(共享 REPORT_SIZE/COUNT)
            //
            // ★ 这里必须处理"一个 INPUT 项同时声明 X 与 Y"的标准写法:
            //     09 30   USAGE (X)
            //     09 31   USAGE (Y)
            //     81 02   INPUT       <- 只有一个 INPUT
            //   解析器逐 item 扫描, 走到 INPUT 时 usage 里只剩【最后一个】值(0x31=Y),
            //   若只在 INPUT 处判断, X 就被永久漏掉:
            //     -> desc.xAxisSize 恒为 0
            //     -> 下游 looksLikeMouse 要求 xAxisSize>0 && yAxisSize>0 => 永不成立
            //     -> mouseIface 恒为 -1 => 所有鼠标报文被静默丢弃
            //   现象正是"灯在闪(报文确实到了)但光标不动", 且没有任何报错。
            //
            //   修法: seenXY 在解析 USAGE/USAGE_MIN/MAX 的当下就记录(见上面 case 0x08/0x28),
            //   到这里直接按它判定, 不依赖已被覆盖的 usage。
            if (usagePage == 0x01 && (seenXY & 0x03) == 0x03)
            {
                const uint32_t perAxisBits = reportSize ? reportSize : 8;
                const uint32_t axisCount   = reportCount ? reportCount : 2;

                // X 在前, Y 紧随其后, 两轴宽度相同(共享 REPORT_SIZE)。
                if (!haveX) {
                    desc.xAxisBitOffset = (uint16_t)bitOffset;
                    desc.xAxisStartByte = (uint8_t)(bitOffset / 8);
                    desc.xAxisSize = (uint8_t)perAxisBits;
                    haveX = true;
                }
                if (!haveY) {
                    const uint16_t yBits = (uint16_t)(bitOffset + perAxisBits);
                    desc.yAxisBitOffset = yBits;
                    desc.yAxisStartByte = (uint8_t)(yBits / 8);
                    desc.yAxisSize = (uint8_t)perAxisBits;
                    haveY = true;
                }

                // 一个 INPUT 项覆盖两根轴: 位偏移一次性推进 reportCount 个字段
                bitOffset += perAxisBits * (axisCount < 2 ? 2 : axisCount);
                if (bitOffset > kMaxBits) bitOffset = kMaxBits;

                // 每字节塞几根轴: 1=顺序存放, 2=半字节交织(12 位轴常见)
                if (reportCount >= 2 && perAxisBits > 0 && perAxisBits < 8) {
                    desc.maxAxisBitsPerByte = (uint8_t)(8 / perAxisBits);
                }

                // 消费掉本次轴标记, 避免影响后续 INPUT 项(X/Y 只声明一次)
                seenXY &= (uint8_t)~0x03;
                break;
            }

            // 单轴写法(少见): 只有一个 USAGE, 且上面成对分支未命中
            if (usagePage == 0x01 && (usage == 0x30 || usage == 0x31))
            {
                const uint32_t perAxisBits = reportSize ? reportSize : 8;

                if (usage == 0x30) {
                    if (!haveX) {
                        desc.xAxisBitOffset = (uint16_t)bitOffset;
                        desc.xAxisStartByte = (uint8_t)(bitOffset / 8);
                        desc.xAxisSize = (uint8_t)perAxisBits;
                        haveX = true;
                    }
                } else {
                    if (!haveY) {
                        desc.yAxisBitOffset = (uint16_t)bitOffset;
                        desc.yAxisStartByte = (uint8_t)(bitOffset / 8);
                        desc.yAxisSize = (uint8_t)perAxisBits;
                        haveY = true;
                    }
                }

                // 两个轴共用这个 INPUT 项: 一次性推进两根轴
                uint32_t axisCount = reportCount ? reportCount : 1;
                if (seenXY == 0x03 && reportCount >= 2 && perAxisBits > 0 && perAxisBits < 8) {
                    // 同一字节里能塞下几根轴 -> 1 表示顺序存放, 2 表示半字节交织
                    desc.maxAxisBitsPerByte = (uint8_t)(8 / perAxisBits);
                }
                bitOffset += perAxisBits * axisCount;
                if (bitOffset > kMaxBits) bitOffset = kMaxBits;
                break;
            }

            // 滚轮: Generic Desktop / Wheel(0x38)
            if (usagePage == 0x01 && usage == 0x38) {
                desc.wheelStartByte = (uint8_t)(bitOffset / 8);
                desc.wheelSize = (uint8_t)(reportSize ? reportSize : 8);
                haveWheel = true;
                bitOffset += reportSize * reportCount;
                if (bitOffset > kMaxBits) bitOffset = kMaxBits;
                break;
            }

            // 键盘修饰键: Usage Page 0x07(Keyboard), Usage 0xE0..0xE7
            // 描述符标准写法是 REPORT_SIZE=1 / REPORT_COUNT=8 / USAGE_MIN=0xE0 /
            // USAGE_MAX=0xE7, 8 个 1-bit 字段拼成 1 字节。usageMinimum 也能触发
            // (有的设备只写 MIN/MAX 不写单个 USAGE)。
            if (usagePage == 0x07 && !desc.hasModifierByte && reportSize == 1 &&
                ((usage >= 0xE0 && usage <= 0xE7) ||
                 (usageMinimum >= 0xE0 && usageMinimum <= 0xE7))) {
                desc.modifierByte = (uint8_t)(bitOffset / 8);
                desc.hasModifierByte = true;
                desc.isKeyboard = true;
                bitOffset += reportSize * reportCount;
                if (bitOffset > kMaxBits) bitOffset = kMaxBits;
                break;
            }

            // 键盘主键码数组: Usage Page 0x07, 8-bit, USAGE_MIN=0x00..USAGE_MAX=0x65
            // 标准键盘是 REPORT_COUNT=6。NKRO 键盘可能更多, 这里按实际 count 记。
            if (usagePage == 0x07 && reportSize == 8 && !desc.keyArrayCount &&
                (usageMinimum < 0xE0 || usageMinimum == 0)) {
                desc.keyArrayStartByte = (uint8_t)(bitOffset / 8);
                desc.keyArrayCount = (uint8_t)(reportCount > 32 ? 32 : reportCount);
                desc.isKeyboard = true;
                bitOffset += reportSize * reportCount;
                if (bitOffset > kMaxBits) bitOffset = kMaxBits;
                break;
            }

            // 其它字段(常量填充 / 厂商自定义): 按声明的位数跳过
            bitOffset += reportSize * reportCount;
            if (bitOffset > kMaxBits) bitOffset = kMaxBits;
            break;
        }

        default:
            break;
        }
    }

    // ---- 收尾校验: 任何越界布局一律判为无效, 宁可不上报也不越界读 ----
    desc.valid = (haveX || haveY || haveButtons || desc.isKeyboard);

    if (desc.hasReportId) {
        // 带 REPORT_ID 的报文首字节是 ID, 所有布局整体后移 1 字节
        if (desc.buttonStartByte < kMaxByte) desc.buttonStartByte++;
        if (desc.xAxisStartByte < kMaxByte)  desc.xAxisStartByte++;
        if (desc.yAxisStartByte < kMaxByte)  desc.yAxisStartByte++;
        if (desc.wheelStartByte < kMaxByte)  desc.wheelStartByte++;
        if (desc.keyArrayStartByte < kMaxByte) desc.keyArrayStartByte++;
        if (desc.hasModifierByte && desc.modifierByte < kMaxByte) desc.modifierByte++;
    }

    // 轴宽合法化: 只接受 8/12/16 位
    if (desc.xAxisSize == 0) desc.xAxisSize = 8;
    if (desc.yAxisSize == 0) desc.yAxisSize = 8;
    if (desc.xAxisSize > 16) desc.xAxisSize = 16;
    if (desc.yAxisSize > 16) desc.yAxisSize = 16;

    // 键盘没有 X/Y 轴, 上面那套轴向校验对它不适用 —— 否则键盘接口会被判无效。
    // 只对"含鼠标轴"的布局做轴向检查。
    if (haveX || haveY) {
        if (desc.xAxisSize != 8 && desc.xAxisSize != 12 && desc.xAxisSize != 16) desc.valid = false;
        if (desc.yAxisSize != 8 && desc.yAxisSize != 12 && desc.yAxisSize != 16) desc.valid = false;
        if (desc.xAxisStartByte > kMaxByte) desc.valid = false;
        if (desc.yAxisStartByte > kMaxByte) desc.valid = false;

        const uint8_t xBytes = (uint8_t)((desc.xAxisSize + 7) / 8);
        const uint8_t yBytes = (uint8_t)((desc.yAxisSize + 7) / 8);
        if ((uint32_t)desc.xAxisStartByte + xBytes > (uint32_t)kMaxDesc) desc.valid = false;
        if ((uint32_t)desc.yAxisStartByte + yBytes > (uint32_t)kMaxDesc) desc.valid = false;
    }

    // 起始字节必须在本缓冲范围内
    if (desc.wheelStartByte > kMaxByte) desc.valid = false;
    if (desc.buttonStartByte > kMaxByte) desc.valid = false;

    // 键盘字段越界同样判无效
    if (desc.isKeyboard) {
        if (desc.modifierByte > kMaxByte) desc.valid = false;
        if ((uint32_t)desc.keyArrayStartByte + desc.keyArrayCount > (uint32_t)kMaxDesc) {
            desc.valid = false;
        }
    }

    if (desc.maxAxisBitsPerByte == 0) desc.maxAxisBitsPerByte = 1;

    // 写入【该接口自己的】槽位, 不再覆盖全局单份布局。
    // 复合设备(鼠标+键盘+厂商接口)每个 HID 接口都会走到这里一次,
    // 各存各的, 互不影响。
    if (ifaceNum < kMaxIface) {
        descPerIface[ifaceNum]      = desc;
        descPerIfaceValid[ifaceNum] = true;

        // 选定鼠标接口: 优先 protocol == MOUSE, 其次 NONE(厂商/无协议)。
        // 只选一个, 避免同一份位移被多个接口重复注入。
        const uint8_t proto = endpoint_data_list[ifaceNum].bInterfaceProtocol;
        const bool isMouseProto = (proto == HID_ITF_PROTOCOL_MOUSE);
        const bool isNoneProto  = (proto == HID_ITF_PROTOCOL_NONE);
        const bool isKbdProto   = (proto == HID_ITF_PROTOCOL_KEYBOARD);

        // 该接口的布局是否真的含鼠标轴/按钮。键盘的按钮(Usage Page 0x09 的
        // 修饰键)也会让 haveButtons 为真, 所以不能只看 desc.valid ——
        // 必须要求出现 X/Y 轴, 否则纯键盘接口会被误选成鼠标接口,
        // 于是键盘报文被当鼠标解码, 产生垃圾位移。
        //
        // ★ 兜底(本次新增): 除了"解析出 X/Y"之外, 再给一条【协议声明】通道。
        //   只要 bInterfaceProtocol 明确写的是 MOUSE(2), 且它不是键盘, 就认它是鼠标。
        //   理由: 上面那条 hasMouseAxes 依赖解析器对描述符声明顺序的理解, 一旦遇到
        //   没预料到的写法(USAGE_MIN/MAX 成对、非标准顺序、厂商私有打包), 解析不出
        //   X/Y 就会让 mouseIface 恒为 -1, 于是【整条鼠标链路被静默关掉】—— 没有任何
        //   报错, 只有"灯在闪但光标不动"。设备既然在接口描述符里自报 protocol=MOUSE,
        //   就应当相信它; 布局异常时下面的 layoutOk 仍会逐帧校验并丢弃越界帧。
        const bool hasMouseAxes = (desc.xAxisSize > 0 && desc.yAxisSize > 0 &&
                                   (desc.xAxisStartByte != desc.yAxisStartByte ||
                                    desc.xAxisBitOffset != desc.yAxisBitOffset));
        const bool protoSaysMouse = isMouseProto && !desc.isKeyboard;
        const bool looksLikeMouse =
            desc.valid && !isKbdProto && !desc.isKeyboard && (hasMouseAxes || protoSaysMouse);

        if (looksLikeMouse && (isMouseProto || isNoneProto)) {
            if (mouseIface < 0) {
                mouseIface = (int8_t)ifaceNum;
            } else if (isMouseProto &&
                       endpoint_data_list[mouseIface].bInterfaceProtocol != HID_ITF_PROTOCOL_MOUSE) {
                // 后出现的"真鼠标协议"接口优先级更高, 顶掉先前选中的 NONE 接口
                mouseIface = (int8_t)ifaceNum;
            }
        }

    } else {
        ESP_LOGW("EspUsbHost::parseHIDReportDescriptor",
                 "iface %u out of range (max %u), layout dropped",
                 (unsigned)ifaceNum, (unsigned)kMaxIface);
    }

    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor",
             "iface=%u layout: valid=%d id=%u btn(sz=%u,byte=%u) x(sz=%u,byte=%u,bit=%u) y(sz=%u,byte=%u,bit=%u) wheel(byte=%u,sz=%u) perByte=%u kbd=%d keyArr(byte=%u,n=%u) mod(byte=%u,has=%d) mouseIface=%d",
             (unsigned)ifaceNum,
             desc.valid ? 1 : 0, (unsigned)desc.reportId,
             (unsigned)desc.buttonSize, (unsigned)desc.buttonStartByte,
             (unsigned)desc.xAxisSize, (unsigned)desc.xAxisStartByte, (unsigned)desc.xAxisBitOffset,
             (unsigned)desc.yAxisSize, (unsigned)desc.yAxisStartByte, (unsigned)desc.yAxisBitOffset,
             (unsigned)desc.wheelStartByte, (unsigned)desc.wheelSize,
             (unsigned)desc.maxAxisBitsPerByte,
             desc.isKeyboard ? 1 : 0,
             (unsigned)desc.keyArrayStartByte, (unsigned)desc.keyArrayCount,
             (unsigned)desc.modifierByte, desc.hasModifierByte ? 1 : 0,
             (int)mouseIface);

    return desc;
}

void EspUsbHost::_onReceive(usb_transfer_t *transfer)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(transfer->context);
    if (!usbHost)
    {
        ESP_LOGE("EspUsbHost::_onReceive", "Error: Context pointer is null in _onReceive");
        usb_host_transfer_free(transfer);
        return;
    }

    bool has_data = (transfer->actual_num_bytes > 0);
    const int reportLen = transfer->actual_num_bytes;

    if (has_data)
    {
        usbHost->last_activity_time = millis();
        if (EspUsbHost::deviceConnected && usbHost->deviceSuspended)
        {
            usbHost->resume_device();
        }
        flashLED();
    }

    // 注意: 这里原来是每 250ms 构造一次 std::stringstream 打整包 hex。
    // ESP_LOGD 虽然被编译裁掉, 但 logRawBytes() 是普通函数调用, 参数照样求值,
    // stringstream 的构造成本在热路径上是实打实的。改成只在调试模式下做。
    if (usbHost->debugModeActive) {
        usbHost->logRawBytes("EspUsbHost::_onReceive HID Report", transfer->data_buffer, transfer->actual_num_bytes);
    }

    // ===== 只处理"数据实际来自的那个端点"所属的接口 =====
    //
    // 用数据实际来自的端点【完整地址(含方向位)】去查 ep_owner[], 拿到所属接口号,
    // 再用该接口号取它自己的报告布局。这样:
    //   1) 一份报表只处理一次(旧实现按 16 个槽位遍历, 同一份位移被注入多次);
    //   2) 不会被其它接口(键盘/厂商接口)的布局串味;
    //   3) IN/OUT 端点号相同的复合设备不会互相顶掉(旧 ep_owner_class[16] 丢了方向位)。
    const uint8_t epSlotIdx = EspUsbHost::epSlot(transfer->bEndpointAddress);
    const bool    epKnown   = usbHost->ep_owner[epSlotIdx].used;
    // 端点未经描述符登记时, iface 字段是 0 —— 而 0 可能恰好等于 mouseIface,
    // 于是未登记的端点会被误判成鼠标接口。这里用一个不可能相等的哨兵值兜住,
    // 保证后续所有以 ifaceNum 为下标的访问都短路在 epKnown 之后。
    const uint8_t ifaceNum  = epKnown ? usbHost->ep_owner[epSlotIdx].iface
                                      : (uint8_t)0xFF;

    // 只认被选中的那个鼠标接口; 未选中(还没解析到鼠标描述符)就什么都不做。
    // 另外要求该接口 class == HID, 避免厂商接口的报文被误当鼠标。
    bool epIsSelectedMouse =
        epKnown &&
        (usbHost->mouseIface >= 0) &&
        (ifaceNum == (uint8_t)usbHost->mouseIface) &&
        (ifaceNum < EspUsbHost::kMaxIface) &&
        (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID) &&
        usbHost->descPerIfaceValid[ifaceNum] &&
        usbHost->descPerIface[ifaceNum].valid;

    // ===== 兜底通道: 报告描述符没解析成功时, 退回 09-10 老版的判定方式 =====
    //
    // ★ 这是本次最重要的修复。
    //
    // 新版把"是否处理鼠标报文"完全挂在 descPerIfaceValid[] 上, 而那份布局只有在
    // GET_DESCRIPTOR(0x22) 异步返回、且解析成功之后才会被写上。这条链路很长:
    //   onConfig(HID_DESC) -> submitControl -> 异步回调 _onReceiveControl -> parse
    // 任何一环出问题(设备不支持该请求 / 返回长度异常 / 描述符写法没被解析器覆盖 /
    // 复合接口的编号对不上), descPerIfaceValid 就一直是 false, 于是:
    //     epIsSelectedMouse == false -> 每一帧报文都被丢弃
    //   —— 而且【一个字节都不会报错】, 表现为"灯在闪但光标不动", 极难定位。
    //
    // 09-10 老版没有这个依赖: 它只要求接口 class==HID 且 protocol ∈ {MOUSE, NONE},
    // 就直接按固定偏移解码。那份代码在真机上是能移动的。
    //
    // 因此保留新版"精确布局"作为首选(支持 12/16 位、Report ID、复合设备),
    // 同时补上老版那条"协议声明即可用"的兜底 —— 布局不可用时按最常见的
    // 8 位标准鼠标格式(byte0=buttons, byte1=X, byte2=Y, byte3=wheel)解码。
    // 该格式覆盖绝大多数鼠标; 若设备实际不是这个格式, 下面的边界检查仍会拦住越界帧。
    const bool ifaceIsHid = epKnown &&
                            (ifaceNum < EspUsbHost::kMaxIface) &&
                            (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID);
    const uint8_t ifaceProto = (ifaceNum < EspUsbHost::kMaxIface)
                                   ? usbHost->endpoint_data_list[ifaceNum].bInterfaceProtocol
                                   : (uint8_t)HID_ITF_PROTOCOL_NONE;
    const bool ifaceProtoLooksMouse =
        (ifaceProto == HID_ITF_PROTOCOL_MOUSE) || (ifaceProto == HID_ITF_PROTOCOL_NONE);
    // ★ 边界检查: 端点未登记时 ifaceNum 是哨兵值 0xFF, 直接拿去索引就是越界读。
    //
    //   原来只有 ifaceIsHid 那一行挡了 kMaxIface, 这一行漏了 —— 于是未登记端点
    //   会去读 descPerfIfaceValid[255] / descPerIface[255], 那是相邻全局内存。
    //   危害不只是 UB: 万一读到非零值, layoutUsable 会假性成立, 而它参与
    //   "兜底鼠标判定"(!layoutUsable), 于是鼠标报文可能被整帧丢弃。
    //   这类"边界条件恰好为真就静默改变行为"的读法必须堵死。
    const bool ifaceIdxOk = (ifaceNum < EspUsbHost::kMaxIface);
    const bool layoutUsable = ifaceIdxOk &&
                              usbHost->descPerIfaceValid[ifaceNum] &&
                              usbHost->descPerIface[ifaceNum].valid;

    // 说明: 这里原先还有一整段"键盘接口判定"(ifaceIsKbdIface), 作用是让鼠标兜底
    // 排除键盘接口。键盘转发已整体删除(键盘由独立固件负责), 该判定随之删除。
    //
    // 键盘类接口仍然不会被这条兜底吃掉, 但靠的是另一条路径: 它的报告描述符会被
    // parseHIDReportDescriptor() 解析成功(desc.isKeyboard 参与 desc.valid 判定),
    // 于是该接口的 layoutUsable 为真 -> 兜底条件 !layoutUsable 不成立。
    const bool epIsFallbackMouse = has_data && !layoutUsable && ifaceIsHid &&
                                   ifaceProtoLooksMouse && (reportLen >= 3);

    if (has_data) g_rxReports++;

    // 鼠标报文解码(精确布局优先, 不可用时用标准 8 位兜底布局)。
    if (has_data && (epIsSelectedMouse || epIsFallbackMouse))
    {
        g_rxMouseGate++;
        // 兜底时合成一份"标准 8 位鼠标"布局, 走上面对话框完全相同的解码/发送路径,
        // 避免出现第二套并行的发送代码(那正是产生分叉 bug 的温床)。
        EspUsbHost::HIDReportDescriptor fb = {};
        const EspUsbHost::HIDReportDescriptor *dp = &usbHost->descPerIface[ifaceNum];
        if (!epIsSelectedMouse) {
            fb.valid = true;
            fb.buttonSize = 8;  fb.buttonStartByte  = 0;
            fb.xAxisSize  = 8;  fb.xAxisStartByte   = 1;
            fb.yAxisSize  = 8;  fb.yAxisStartByte   = 2;
            // 滚轮: 标准 4 字节报文才有第 4 字节; 只有 3 字节时按"无滚轮"处理
            if (reportLen >= 4) { fb.wheelSize = 8; fb.wheelStartByte = 3; }
            fb.maxAxisBitsPerByte = 1;
            dp = &fb;
            if (!usbHost->mouseFallbackLogged) {
                usbHost->mouseFallbackLogged = true;
                ESP_LOGW("EspUsbHost",
                         "iface %u: HID report layout unavailable (descValid=%d) -> using standard 8-bit mouse fallback",
                         (unsigned)ifaceNum,
                         usbHost->descPerIfaceValid[ifaceNum] ? 1 : 0);
            }
        }
        const EspUsbHost::HIDReportDescriptor &d = *dp;

    // ===== 报表解码 =====
    //
    // 安全前提(全部新增): 所有字节偏移都来自设备可控的 HID Report Descriptor,
    // 原来直接拿去索引只按 wMaxPacketSize+1 分配的 data_buffer, 没有任何边界检查 ——
    // 一个畸形描述符就能让它读到分配块之外(heap OOB read)。
    // 现在逐项校验: 起始字节 + 需要读取的字节数必须落在本次实际收到的报文长度内,
    // 否则丢弃该帧且不产生任何位移/滚轮输出。
    //
    // 布局选择用描述符里的真实轴宽(REPORT_SIZE), 不再用 LOGICAL_MAXIMUM 猜:
    //   axisSize==8  -> 每轴 1 字节
    //   axisSize==12 且两轴挤在 2 字节里 -> 半字节交织(QMK/部分游戏鼠标)
    //   axisSize==16 -> 每轴 2 字节小端
    {
        // Report ID 过滤: 描述符里有 REPORT_ID 时, 报文首字节必须是该 ID
        bool reportIdOk = !d.hasReportId || (reportLen > 0 && transfer->data_buffer[0] == d.reportId);

        const uint8_t xBytes = (uint8_t)((d.xAxisSize + 7) / 8);
        const uint8_t yBytes = (uint8_t)((d.yAxisSize + 7) / 8);

        bool layoutOk =
            (d.buttonSize == 0 || (int)d.buttonStartByte < reportLen) &&
            ((int)d.xAxisStartByte + (int)xBytes <= reportLen) &&
            ((int)d.yAxisStartByte + (int)yBytes <= reportLen) &&
            (d.wheelSize == 0 || (int)d.wheelStartByte < reportLen);

        if (reportIdOk && layoutOk)
        {
            g_rxDecoded++;
            hid_mouse_report_t report = {};

            if (d.buttonSize) {
                report.buttons = transfer->data_buffer[d.buttonStartByte];
            }

            const uint8_t *db = transfer->data_buffer;

            if (d.xAxisSize == 12 && d.yAxisSize == 12 && xBytes == 2 &&
                d.maxAxisBitsPerByte == 2 && d.yAxisStartByte == d.xAxisStartByte)
            {
                // 半字节交织: 两轴共 3 字节, X = b0 | (b1 & 0x0F) << 8, Y = (b1 >> 4) | b2 << 4
                const uint8_t off = d.xAxisStartByte;
                int16_t xValue = (int16_t)(db[off] | ((db[off + 1] & 0x0F) << 8));
                int16_t yValue = (int16_t)(((db[off + 1] >> 4) & 0x0F) | (db[off + 2] << 4));
                // 12 位有符号就近还原(游戏鼠标的 12 位相对位移通常以 0x800 为 0)
                if (xValue >= 0x800) xValue -= 0x1000;
                if (yValue >= 0x800) yValue -= 0x1000;
                report.x = clampAxis16(xValue);   // 饱和截顶, 不做环绕截断
                report.y = clampAxis16(yValue);
                report.wheel = d.wheelSize ? (int8_t)db[d.wheelStartByte] : 0;
            }
            else if (d.xAxisSize == 16 && d.yAxisSize == 16)
            {
                int16_t xv = (int16_t)((uint16_t)db[d.xAxisStartByte] | ((uint16_t)db[d.xAxisStartByte + 1] << 8));
                int16_t yv = (int16_t)((uint16_t)db[d.yAxisStartByte] | ((uint16_t)db[d.yAxisStartByte + 1] << 8));
                // 16 位值收进 int8 字段: 饱和截顶而非环绕 —— 原 (int8_t) 强转会让
                // +511 变成 -1(反向移动), +256 变成 0(静默丢帧), 见 clampAxis16 说明。
                report.x = clampAxis16(xv);
                report.y = clampAxis16(yv);
                report.wheel = d.wheelSize ? (int8_t)db[d.wheelStartByte] : 0;
            }
            else
            {
                // 8 位(最常见): 每轴 1 字节有符号相对位移
                report.x = (int8_t)db[d.xAxisStartByte];
                report.y = (int8_t)db[d.yAxisStartByte];
                report.wheel = d.wheelSize ? (int8_t)db[d.wheelStartByte] : 0;
            }

            if (report.buttons != usbHost->lastReportedButtons)
            {
                usbHost->onMouseButtons(report, usbHost->lastReportedButtons);
                usbHost->lastReportedButtons = report.buttons;
            }
            if (report.x != 0 || report.y != 0 || report.wheel != 0)
            {
                g_rxFwd++;
                usbHost->onMouseMove(report);
            }
        }
        else
        {
            // 宁可丢这一帧, 也不越界读
            ESP_LOGW("EspUsbHost::_onReceive",
                     "drop report: len=%d idOk=%d layoutOk=%d (btn@%u x@%u/%u y@%u/%u wheel@%u)",
                     reportLen, reportIdOk ? 1 : 0, layoutOk ? 1 : 0,
                     (unsigned)d.buttonStartByte,
                     (unsigned)d.xAxisStartByte, (unsigned)xBytes,
                     (unsigned)d.yAxisStartByte, (unsigned)yBytes,
                     (unsigned)d.wheelStartByte);
        }
    }
    }   // 结束 "if (has_data && (epIsSelectedMouse || epIsFallbackMouse))"

    // Handle transfer status
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGW("EspUsbHost", "Transfer STALL received: Endpoint=0x%x", transfer->bEndpointAddress);
        } else {
            ESP_LOGE("EspUsbHost", "Transfer error: Status=0x%x, Endpoint=0x%x", transfer->status, transfer->bEndpointAddress);
        }
    }

    // ===== 无条件重提交 (关键修复) =====
    //
    // 【为什么去掉 deviceSuspended 门控】
    //   USB Host 读 IN 端点必须"每收完一帧就重新提交", 否则该端点永久静默。
    //   原来这一步被 `if (!deviceSuspended)` 门控: 一旦这个标志被置位,
    //   重提交就永久停止, 而且【没有任何东西会把它清回来】——
    //   实测现象: 设备用一阵子后输入突然完全没反应, 必须重新上电才恢复,
    //   而日志里 conn/ready/xfer 全部正常, 只有 lastAct 冻结不再变化。
    //
    //   deviceSuspended 只是"当前是否挂起"的状态描述, 不该当作"是否继续读取"
    //   的开关。挂起期间提交会被 USB 栈自行处理; 真正需要停止读取时应该
    //   在别处显式停, 而不是靠这里静默失效。
    {
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK)
        {
            ESP_LOGW("EspUsbHost", "Failed to resubmit transfer: err=0x%x, Endpoint=0x%x",
                     err, transfer->bEndpointAddress);
        }
    }

    // ★ 这里【不再】在挂起时 usb_host_transfer_free(transfer)。
    //
    //   原来的 else 分支会在 deviceSuspended 时释放传输, 但【没有任何地方
    //   会重新分配它】—— 于是设备恢复后该端点再也没有传输可提交, 永久静默,
    //   只能靠重新上电恢复。这与上面的门控是同一个故障的两半。
    //
    //   传输应当与该设备的生命周期一致: 挂起期间留着, 恢复后继续用它重提交。
    //   真正的资源回收在设备断开(DEV_GONE)时统一进行。
}

#include "EspUsbHost.h"
#include <sstream>
#include <iomanip>
#include "freertos/semphr.h"

#define USB_FEATURE_SELECTOR_REMOTE_WAKEUP 1

bool EspUsbHost::deviceMouseReady = false;
bool EspUsbHost::deviceConnected = false;
EspUsbHost::HIDReportDescriptor EspUsbHost::HIDReportDesc = {};
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
    transfer->context = this;

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
    transfer->context = this;

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


void EspUsbHost::logRawBytes(const char *functionName, const uint8_t *data, uint16_t length)
{
    std::stringstream rawByteStream;
    
    for (int i = 0; i < length; ++i)
    {
        rawByteStream << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
    }

    ESP_LOGI(functionName, "Raw Bytes: %s", rawByteStream.str().c_str());
}

void EspUsbHost::onConfig(const uint8_t bDescriptorType, const uint8_t *p)
{
    static uint8_t currentInterfaceNumber;

        logRawBytes("EspUsbHost::onConfig", p, p[0]);

    switch (bDescriptorType)
    {
    case USB_DEVICE_DESC:
    {
        // Log raw bytes for USB_DEVICE_DESC
        logRawBytes("USB_DEVICE_DESC", p, p[0]); 
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
        // Log raw bytes for USB_STRING_DESC
        logRawBytes("USB_STRING_DESC", p, p[0]);
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
        // Log raw bytes for USB_INTERFACE_DESC
        logRawBytes("USB_INTERFACE_DESC", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_INTERFACE_DESC");

        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;

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
        // Log raw bytes for USB_ENDPOINT_DESC
        logRawBytes("USB_ENDPOINT_DESC", p, p[0]);
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

            if (this->claim_err != ESP_OK)
            {
                ESP_LOGW("EspUsbHost", "Skipping endpoint due to claim_err.");
                return;
            }

            uint8_t ep_num = USB_EP_DESC_GET_EP_NUM(ep_desc);
            endpoint_data_list[ep_num].bInterfaceNumber = endpoint_data_list[currentInterfaceNumber].bInterfaceNumber;
            endpoint_data_list[ep_num].bInterfaceClass = endpoint_data_list[currentInterfaceNumber].bInterfaceClass;
            endpoint_data_list[ep_num].bInterfaceSubClass = endpoint_data_list[currentInterfaceNumber].bInterfaceSubClass;
            endpoint_data_list[ep_num].bInterfaceProtocol = endpoint_data_list[currentInterfaceNumber].bInterfaceProtocol;
            endpoint_data_list[ep_num].bCountryCode = endpoint_data_list[currentInterfaceNumber].bCountryCode;

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
                interval = ep_desc->bInterval;
                isReady = true;
                this->usbTransferSize++;

                ESP_LOGI("EspUsbHost", "Submitting transfer for endpoint 0x%x", ep_desc->bEndpointAddress);

                err = usb_host_transfer_submit(this->usbTransfer[this->usbTransferSize - 1]);
                if (err != ESP_OK)
                {
                    ESP_LOGE("EspUsbHost", "usb_host_transfer_submit() failed with err=%x", err);
                }
            }

            endpointCounter++;
        }

        break;
    }

    case USB_INTERFACE_ASSOC_DESC:
    {
        // Log raw bytes for USB_INTERFACE_ASSOC_DESC
        logRawBytes("USB_INTERFACE_ASSOC_DESC", p, p[0]);   // did not log 
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
        // Log raw bytes for USB_HID_DESC
        logRawBytes("USB_HID_DESC", p, p[0]);
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
        // Log raw bytes for USB_CONFIGURATION_DESC
        logRawBytes("USB_CONFIGURATION_DESC", p, p[0]);
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
        // Log raw bytes for unknown descriptor type
        logRawBytes("Unknown", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: Unknown (0x%02X)", bDescriptorType);
        break;
    }
    }
}


void EspUsbHost::_clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);
    esp_err_t err;

    // Helper function to log raw bytes using std::stringstream
    auto logRawBytes = [](const char* descriptorType, const uint8_t* data, uint8_t length) {
        std::stringstream rawByteStream;
        for (int i = 0; i < length; ++i) {
            rawByteStream << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
        }
        ESP_LOGI("EspUsbHost::_clientEventCallback", "Raw Bytes (%s): %s", descriptorType, rawByteStream.str().c_str());
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

        ESP_LOGD("EspUsbHost", "New device event detected. Raw event message:");

        logRawBytes("New Device Event Message", (const uint8_t *)eventMsg, sizeof(usb_host_client_event_msg_t));

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
            ESP_LOGD("EspUsbHost", "Retrieved device info. Raw device info:");

            logRawBytes("Device Info", (const uint8_t *)&dev_info, sizeof(usb_device_info_t));

            usbHost->device_info.speed = dev_info.speed;
            usbHost->device_info.dev_addr = dev_info.dev_addr;
            usbHost->device_info.vMaxPacketSize0 = dev_info.bMaxPacketSize0;
            usbHost->device_info.bConfigurationValue = dev_info.bConfigurationValue;
            strcpy(usbHost->device_info.str_desc_manufacturer, getUsbDescString(dev_info.str_desc_manufacturer).c_str());
            strcpy(usbHost->device_info.str_desc_product, getUsbDescString(dev_info.str_desc_product).c_str());
            strcpy(usbHost->device_info.str_desc_serial_num, getUsbDescString(dev_info.str_desc_serial_num).c_str());

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

            // Log raw bytes for the device descriptor
            logRawBytes("Device Descriptor", (const uint8_t *)dev_desc, sizeof(usb_device_desc_t));

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
            // Log raw bytes for the configuration descriptor
            logRawBytes("Configuration Descriptor", (const uint8_t *)config_desc, config_desc->wTotalLength);

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
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(transfer->context);
    if (!usbHost)
    {
        return;
    }

     usbHost->logRawBytes("EspUsbHost::_onReceiveControl", transfer->data_buffer, transfer->actual_num_bytes);

    bool isMouse = false;
    uint8_t *p = &transfer->data_buffer[8];  // Skip the first 8 bytes for processing
    int totalBytes = transfer->actual_num_bytes;

    ESP_LOGI("EspUsbHost", "onReceiveControl called with %d bytes", totalBytes);

    // ===== 报告描述符归属 =====
    //
    // 【老代码的 bug】这里用"描述符里有没有 05 01 09 02 (Mouse)"来判断,
    // 不是鼠标就整包丢弃。对复合设备(键盘+鼠标接收器)是致命的:
    //   1) 键盘接口的报告描述符没有 Mouse usage -> 被丢弃, 于是键盘接口
    //      永远没有自己的描述符, 后面无处可查。
    //   2) 解析结果只写进【一个全局】 HIDReportDesc -> 无论哪个接口的描述符
    //      到达, 都覆盖同一份数据。复合设备下最终存的是"最后一个到达的",
    //      谁也无法保证是鼠标那份。
    //
    // 【本版改法】不再按内容筛选, 而是"按接口存档":
    //   控制传输的 wIndex(接口号)记录在 data_buffer 的前 8 字节 setup 包里,
    //   用它把每份报告描述符存到 iface_report_desc[接口号] 下。
    //   鼠标解码时按"本帧所属接口"取自己那份, 键盘则只看类别标记。
    //
    // 同时保留 isMouse 判定, 只用于决定是否【额外】填充全局 HIDReportDesc
    // (兼容只认全局的那条老鼠标路径, 避免纯鼠标设备回归)。

    // setup 包布局: [0]=bmRequestType [1]=bRequest [2..3]=wValue
    //               [4..5]=wIndex(接口号) [6..7]=wLength
    const uint8_t reqType = transfer->data_buffer[0];
    const uint8_t ctrlIface = transfer->data_buffer[4];

    // 只关心 "GET_DESCRIPTOR / Report" (bmRequestType=0x81, bRequest=0x06,
    // wValue 高字节=0x22)。其余控制传输(字符串/配置等)直接放行, 不参与解析。
    const bool isReportDesc =
        (reqType == 0x81) &&
        (transfer->data_buffer[1] == 0x06) &&
        (transfer->data_buffer[3] == 0x22);

    if (!isReportDesc || totalBytes <= 8) {
        usb_host_transfer_free(transfer);
        return;
    }

    // 接口号有效性: 越界则按 0 处理(单接口设备的情形)
    uint8_t slot = (ctrlIface < EspUsbHost::kMaxReportDescIfaces) ? ctrlIface : 0;

    const uint8_t *desc = &transfer->data_buffer[8];
    const int descLen = totalBytes - 8;

    // 内容判定: 是不是鼠标 (05 01 09 02 = Usage Page Generic Desktop, Usage Mouse)
    // 注意: 顶部的旧声明已废弃, 这里重新判定(不再用于"非鼠标就丢弃")。
    isMouse = false;
    for (int i = 0; i + 3 < descLen; i++)
    {
        if (desc[i] == 0x05 && desc[i + 1] == 0x01 && desc[i + 2] == 0x09 && desc[i + 3] == 0x02)
        {
            isMouse = true;
            break;
        }
    }

    // 内容判定: 是不是键盘 (05 01 09 06 = Usage Page Generic Desktop, Usage Keyboard)
    bool isKbd = false;
    for (int i = 0; i + 3 < descLen; i++)
    {
        if (desc[i] == 0x05 && desc[i + 1] == 0x01 && desc[i + 2] == 0x09 && desc[i + 3] == 0x06)
        {
            isKbd = true;
            break;
        }
    }

    // 存档: 每个接口自己那份描述符的解析结果
    usbHost->iface_isKbd[slot]    = isKbd;
    usbHost->iface_isMouse[slot]  = isMouse;
    usbHost->iface_descValid[slot] = true;
    usbHost->iface_report_desc[slot] =
        usbHost->parseHIDReportDescriptor((uint8_t *)desc, descLen);

    ESP_LOGI("EspUsbHost", "Report desc for iface %u: mouse=%d kbd=%d (len=%d)",
             slot, (int)isMouse, (int)isKbd, descLen);

    // 兼容老路径: 同时写全局 HIDReportDesc。
    //
    // 【严格性调整】原来只在 isMouse==true 时才写。但 isMouse 依赖描述符字节
    // 精确匹配 '05 01 09 02', 一旦某设备用不同编码(分开写 Usage Page/Usage、
    // 或带 Report ID 前缀)就会漏判, 结果全局保持全零 -> 鼠标位移解码全错
    // (实测: 亮灯但光标不动)。
    //
    // 现在改为: 只要该接口的描述符【解析成功】就写全局。
    // 理由: 能走到这里的都是本设备的 HID 接口描述符; 对单接口鼠标设备而言
    //       它就是鼠标描述符。复合设备下鼠标解码仍优先走 iface_report_desc
    //        (按接口取值), 全局只作为兜底, 所以放宽不会带偏鼠标布局。
    HIDReportDesc = usbHost->iface_report_desc[slot];

    // 记录"哪个接口是鼠标", 供诊断与兜底使用
    if (isMouse) {
        ESP_LOGI("EspUsbHost", "iface %u identified as MOUSE", slot);
    }

    usb_host_transfer_free(transfer);
}


void EspUsbHost::onMouse(hid_mouse_report_t report, uint8_t last_buttons)
{
    // 热路径保持轻量，避免高频日志阻塞
}


void EspUsbHost::onMouseButtons(hid_mouse_report_t report, uint8_t last_buttons)
{
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
    if (deviceMouseReady)
    {
        if (report.x != 0 || report.y != 0)
        {
            serial1Send("km.move(%d,%d)\n", report.x, report.y);
        }
        if (report.wheel != 0)
        {
            serial1Send("km.wheel(%d)\n", report.wheel);
        }
    }
}


// 键盘透传: 把真实键盘的键码快照发给设备侧。
//
// 为什么需要去重: 键盘报文是"全量快照", 真实键盘会在状态没变时也周期性
// 重发同样的内容。不去重的话, 打字/按住键时串口上行会被无意义的重复快照
// 占满, 设备侧还要白跑一遍 HID 发送。所以内容不变就整帧丢弃。
//
// 去重缓存用 static: 本函数只在 _onReceive 里被调用(单线程), 且不跨设备
// 复用(HIDReportDesc 在设备插入时会重置, 这里也一并作废)。
void EspUsbHost::onKeyboardSnapshot(uint8_t mod, const uint8_t *keys)
{
    if (!deviceMouseReady) {
        return;     // 设备侧尚未就绪, 先不透传(与鼠标通路保持一致的时机)
    }

    // ---- 去重 ----
    static uint8_t lastMod = 0xFF;      // 初值取不可能的值, 保证第一帧一定发出
    static uint8_t lastKeys[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    bool changed = (mod != lastMod);
    for (int i = 0; i < 6 && !changed; ++i) {
        if (keys[i] != lastKeys[i]) changed = true;
    }
    if (!changed) {
        return;                         // 内容没变: 丢掉这一帧
    }

    lastMod = mod;
    for (int i = 0; i < 6; ++i) {
        lastKeys[i] = keys[i];
    }

    // ---- 透传 ----
    // 走二进制 0x23 KB_REPORT: 打字时每秒可能十几帧, 二进制仅 14 字节且
    // 设备侧零解析(ASCII 形式要 30+ 字节并 sscanf 解析)。
    serial1SendKeyboardBinary(mod, lastKeys, 6);
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

    static unsigned long lastLogTime = 0;
    unsigned long currentTime = millis();
    if (currentTime - lastLogTime >= 250)
    {
        usbHost->logRawBytes("EspUsbHost::_onReceive", transfer->data_buffer, transfer->actual_num_bytes);
        lastLogTime = currentTime;
    }

    uint8_t endpoint_num = transfer->bEndpointAddress & 0x0F;
    bool has_data = (transfer->actual_num_bytes > 0);

    if (has_data)
    {
        usbHost->last_activity_time = millis();
        if (EspUsbHost::deviceConnected && usbHost->deviceSuspended)
        {
            usbHost->resume_device();
        }
        flashLED();
    }

    if (usbHost->debugModeActive) {
        usbHost->logRawBytes("EspUsbHost::_onReceive HID Report", transfer->data_buffer, transfer->actual_num_bytes);
    }

    // ===== 复合 HID 设备透传 =====
    //
    // 【设计原则】能透传的都尽量原样复制透传; 传输层特性(轮询间隔、刷新率、
    // 时序)不透传 —— 那些由设备侧自己决定, 复制它们没有意义。
    //
    // 一个复合 HID 设备(鼠标 + 键盘 + 多媒体键 + 厂商自定义)有【多个接口】,
    // 每个接口有【自己的 IN 端点】和【自己的报告描述符】。正确做法是:
    // 先由"本帧来自哪个端点"定位它属于哪个接口, 再用【那个接口自己的
    // 描述符】解码 —— 而不是像老代码那样拿一份全局描述符去套所有端点。
    //
    // 【老代码的 bug】它循环遍历 16 个槽位, 对每个匹配
    // (class==HID && protocol ∈ {MOUSE,NONE}) 的槽位都解码一遍并发送。
    // 而 onConfig() 把同一接口信息复制到了两处:
    //     endpoint_data_list[currentInterfaceNumber]   (按接口号)
    //     endpoint_data_list[ep_num]                   (按端点号)
    // 于是同一接口至少被匹配 2 次 -> 位移被重复注入 -> 移动量放大数倍,
    // 快速移动时重复帧在 5Mbps 链路上堆叠 -> 卡顿累积。
    //
    // 【本次改法】先由端点地址定位"本帧唯一的接口", 再按该接口的类别
    // 分派: 鼠标 -> 解码位移; 键盘 -> 透传键码快照。两者互不干扰, 因此
    // "鼠标+键盘"复合设备的两条通路都能各自正确工作。

    // 本帧来自哪个接口?
    //
    // ★ 这里必须按【端点】定位, 不能"取第一个 HID 接口"。
    //
    // 【上一版的 bug】原实现是"遍历 16 个槽位, 取第一个 class==HID 且
    // bInterfaceNumber == 下标 的槽位"。这在纯鼠标上碰巧能用, 但对
    // "键盘接收器"这类设备会彻底失效, 原因是:
    //   1) 它忽略了 transfer->bEndpointAddress —— 帧到底从哪个端点来,
    //      代码根本没看。多个 HID 接口时永远只会命中同一个接口。
    //   2) 判据 bInterfaceNumber == 下标 只是"槽位是接口号索引"的弱证据,
    //      接口 0 被鼠标占用时, 键盘接口(接口 1/2)永远轮不到。
    //   结果: 键盘接口的报文被拿去当【鼠标】解码, 键盘静默失效
    //        (而 flashLED() 在更前面, 所以灯照样闪 —— 现象与实测完全一致)。
    //
    // 【本版改法】用端点地址反查接口:
    //   onConfig 已经为每个 IN 端点填好了 ep_owner[epSlot(addr)].iface,
    //   那是"这个端点属于哪个接口"的权威记录。按它定位才正确。
    //
    // epSlot()/ep_owner 的定义见 EspUsbHost.h。
    int8_t ifaceOfFrame = -1;

    const uint8_t epSlotIdx = epSlot(transfer->bEndpointAddress);
    if (epSlotIdx < EspUsbHost::kEpSlotCount && usbHost->ep_owner[epSlotIdx].used) {
        ifaceOfFrame = (int8_t)usbHost->ep_owner[epSlotIdx].iface;
    }

    // 兜底: 老版本里端点如果没被 ep_owner 记录(或表未初始化), 再退回
    // "第一个 HID 接口"的老做法, 保证纯鼠标设备不至于因此失效。
    if (ifaceOfFrame < 0) {
        for (int i = 0; i < 16; i++) {
            const auto &e = usbHost->endpoint_data_list[i];
            if (e.bInterfaceClass != USB_CLASS_HID) continue;
            if (e.bInterfaceNumber != i) continue;
            ifaceOfFrame = (int8_t)i;
            break;
        }
    }

    const bool ifaceValid    = (ifaceOfFrame >= 0);
    const uint8_t ifaceProto = ifaceValid
                                 ? usbHost->endpoint_data_list[ifaceOfFrame].bInterfaceProtocol
                                 : (uint8_t)HID_ITF_PROTOCOL_NONE;

    // ===== 接口类别判定: 以【报告描述符内容】为准, protocol 只作兜底 =====
    //
    // 【为什么不能只看 bInterfaceProtocol —— 实测踩到的坑】
    // 2.4G 接收器这类设备的键盘接口常常报:
    //     Class_03 (HID)  SubClass_00  Prot_00      <- protocol = NONE, 不是 1!
    // (键盘接收器 VID_3554:PID_FA09 就是如此, 其键盘接口为 MI_00/Prot_00)
    // 于是 `proto == HID_ITF_PROTOCOL_KEYBOARD` 判据永远不成立, 键盘报文
    // 被判成鼠标 -> 拿鼠标的布局去解码 -> 键盘完全失效。
    //
    // 正确判据是直接看该接口的报告描述符里出现了哪个 Usage:
    //     Usage Page 0x01 + Usage 0x02 -> Mouse
    //     Usage Page 0x01 + Usage 0x06 -> Keyboard
    // 这在 _onReceiveControl 里已经按接口解析并记录 (iface_isMouse/iface_isKbd)。
    //
    // protocol 仅在描述符尚未到达时兜底, 保证"描述符没解析出来"也不会
    // 让纯鼠标设备失效。
    bool descIsKbd   = false;
    bool descIsMouse = false;
    bool descKnown   = false;
    if (ifaceValid && ifaceOfFrame < EspUsbHost::kMaxReportDescIfaces) {
        if (usbHost->iface_descValid[ifaceOfFrame]) {
            descKnown   = true;
            descIsKbd   = usbHost->iface_isKbd[ifaceOfFrame];
            descIsMouse = usbHost->iface_isMouse[ifaceOfFrame];
        }
    }

    // 键盘接口
    const bool ifaceIsKeyboard = ifaceValid &&
        (descKnown ? descIsKbd : (ifaceProto == HID_ITF_PROTOCOL_KEYBOARD));

    // 指针接口: 排除键盘后, 描述符说是鼠标 / 或 protocol 为 MOUSE|NONE。
    // 注意【必须排除键盘】—— 很多键盘接口 protocol 也是 NONE,
    // 不排除的话键盘报文会被当位移注入。
    const bool ifaceLooksMouse = ifaceValid && !ifaceIsKeyboard &&
        (descKnown ? descIsMouse
                   : (ifaceProto == HID_ITF_PROTOCOL_MOUSE ||
                      ifaceProto == HID_ITF_PROTOCOL_NONE));

    // ---------------------------------------------------------------
    // 1) 指针类接口 -> 解码位移并透传
    // ---------------------------------------------------------------
    if (ifaceLooksMouse)
    {
        // 用【本接口自己的】报告描述符解码 —— 复合设备下每个 HID 接口的布局
        // 都不同, 必须各用各的。取不到时才退回全局那份(纯鼠标设备的兼容路径)。
        // 取本接口的描述符; 取不到就用全局那份。
        EspUsbHost::HIDReportDescriptor md =
            ((ifaceValid && ifaceOfFrame < EspUsbHost::kMaxReportDescIfaces &&
              usbHost->iface_descValid[ifaceOfFrame])
                 ? usbHost->iface_report_desc[ifaceOfFrame]
                 : usbHost->HIDReportDesc);

        // ---- 合理性检查(修回归的关键) ----
        // 若拿到的布局明显不可用(两个轴位宽都是 0), 说明描述符没解析出来,
        // 此时用它解码会得到全零偏移 -> 位移异常/光标不动。
        // 退回标准 3 字节鼠标布局: buttons@0, x@1, y@2, wheel@3 (8 位轴)。
        // 这保证即使描述符请求失败, 鼠标仍然可用(宁可按标准布局动作)。
        if (md.xAxisSize == 0 && md.yAxisSize == 0) {
            static bool warnedOnce = false;
            if (!warnedOnce) {
                ESP_LOGW("EspUsbHost", "mouse layout unusable (xSize=%d ySize=%d) -> fallback to standard 3-byte layout",
                         md.xAxisSize, md.yAxisSize);
                warnedOnce = true;
            }
            md.reportId        = 0;
            md.buttonStartByte = 0;
            md.buttonSize      = 8;
            md.xAxisSize       = 8;
            md.yAxisSize       = 8;
            md.xAxisStartByte  = 1;
            md.yAxisStartByte  = 2;
            md.wheelSize       = 8;
            md.wheelStartByte  = 3;
        }
        // Report ID 匹配过滤: 若报告描述符含 Report ID, 报文首字节必须匹配。
        // 注意 has_data 检查: 零字节帧没有 data_buffer[0] 可读。
        const bool idOk =
            (md.reportId == 0) ||
            (has_data && transfer->data_buffer[0] == md.reportId);

        if (idOk)
        {
            static uint8_t last_buttons = 0;
            hid_mouse_report_t report = {};
            report.buttons = transfer->data_buffer[md.buttonStartByte];

            if (md.xAxisSize == 12 && md.yAxisSize == 12)
            {
                uint8_t xyOffset = md.xAxisStartByte;
                int16_t xValue = (transfer->data_buffer[xyOffset]) |
                                 ((transfer->data_buffer[xyOffset + 1] & 0x0F) << 8);
                int16_t yValue = ((transfer->data_buffer[xyOffset + 1] >> 4) & 0x0F) |
                                 (transfer->data_buffer[xyOffset + 2] << 4);

                report.x = xValue;
                report.y = yValue;
                uint8_t wheelOffset = md.wheelStartByte;
                report.wheel = transfer->data_buffer[wheelOffset];
            }
            else if (md.xAxisSize == 16 && md.yAxisSize == 16)
            {
                uint8_t xOffset = md.xAxisStartByte;
                uint8_t yOffset = md.yAxisStartByte;
                uint8_t wheelOffset = md.wheelStartByte;

                int16_t xValue = (int16_t)((uint16_t)transfer->data_buffer[xOffset] | ((uint16_t)transfer->data_buffer[xOffset + 1] << 8));
                int16_t yValue = (int16_t)((uint16_t)transfer->data_buffer[yOffset] | ((uint16_t)transfer->data_buffer[yOffset + 1] << 8));

                report.x = xValue;
                report.y = yValue;
                report.wheel = (int8_t)transfer->data_buffer[wheelOffset];
            }
            else
            {
                uint8_t xOffset = md.xAxisStartByte;
                uint8_t yOffset = md.yAxisStartByte;
                uint8_t wheelOffset = md.wheelStartByte;

                report.x = (int8_t)transfer->data_buffer[xOffset];
                report.y = (int8_t)transfer->data_buffer[yOffset];
                report.wheel = (int8_t)transfer->data_buffer[wheelOffset];
            }

            usbHost->onMouse(report, last_buttons);
            if (report.buttons != last_buttons)
            {
                usbHost->onMouseButtons(report, last_buttons);
                last_buttons = report.buttons;
            }
            if (report.x != 0 || report.y != 0 || report.wheel != 0)
            {
                usbHost->onMouseMove(report);
            }
        }
    }

    // ---------------------------------------------------------------
    // 2) 键盘接口 -> 透传键码快照
    // ---------------------------------------------------------------
    // 与鼠标同理: 只有本接口确实是键盘时才走这条路。这样"鼠标+键盘"
    // 复合设备的两个接口都能各自正确透传。
    //
    // 键盘报告标准布局(8 字节):
    //     [0] modifier 位图
    //     [1] 保留
    //     [2..7] 6 个键码
    // 若描述符声明了 Report ID, 则首字节是 ID, 上述布局整体后移一位。
    if (ifaceIsKeyboard && has_data)
    {
        // 用【本接口自己】的报告描述符取 Report ID, 而不是全局那份
        // (全局那份可能被别的接口覆盖, 见 iface_report_desc 的说明)。
        const uint8_t rptId = (ifaceValid && ifaceOfFrame < EspUsbHost::kMaxReportDescIfaces &&
                               usbHost->iface_descValid[ifaceOfFrame])
                                ? usbHost->iface_report_desc[ifaceOfFrame].reportId
                                : 0;

        const bool idPresent = (rptId != 0) &&
                               (transfer->actual_num_bytes > 0) &&
                               (transfer->data_buffer[0] == rptId);
        const int base = idPresent ? 1 : 0;

        // 标准键盘报文 = modifier(1) + 保留(1) + 6 键码 = 8 字节。
        // 但有些接收器只报 6 字节(modifier + 保留 + 4 键码), 因此不再硬性
        // 要求 8 字节 —— 只要够读 modifier 和至少一个键码槽就按实际长度发。
        const int avail = transfer->actual_num_bytes - base;
        if (avail >= 2)
        {
            uint8_t mod = transfer->data_buffer[base];

            static uint8_t zero6[6] = {0,0,0,0,0,0};
            uint8_t buf6[6] = {0,0,0,0,0,0};
            // 从 base+2 起最多拷 6 个键码, 不足补 0
            const int keyBytes = (avail > 2) ? (avail - 2) : 0;
            const int n = (keyBytes < 6) ? keyBytes : 6;
            for (int i = 0; i < n; ++i) buf6[i] = transfer->data_buffer[base + 2 + i];
            (void)zero6;

            usbHost->onKeyboardSnapshot(mod, buf6);
        }
    }

    // Handle transfer status
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGW("EspUsbHost", "Transfer STALL received: Endpoint=0x%x", transfer->bEndpointAddress);
        } else {
            ESP_LOGE("EspUsbHost", "Transfer error: Status=0x%x, Endpoint=0x%x", transfer->status, transfer->bEndpointAddress);
        }
    }

    // Resubmit the transfer if the device is not suspended
    if (!usbHost->deviceSuspended)
    {
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK)
        {
            ESP_LOGE("EspUsbHost", "Failed to resubmit transfer: err=0x%x, Endpoint=0x%x", err, transfer->bEndpointAddress);
        }
    }
    else
    {
        usb_host_transfer_free(transfer);
    }
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
    transfer->context = this;

    // Log raw bytes using the helper function
    logRawBytes("EspUsbHost::submitControl", transfer->data_buffer, transfer->num_bytes);

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

EspUsbHost::HIDReportDescriptor EspUsbHost::parseHIDReportDescriptor(uint8_t *data, int length)
{
    // Log the raw bytes using the helper function
    logRawBytes("EspUsbHost::parseHIDReportDescriptor", data, length);

    int i = 0;
    ParsedValues parsedValues = {0};

    auto getValue = [](uint8_t *data, int size, bool isSigned) -> int16_t
    {
        int16_t value = 0;
        if (isSigned)
        {
            if (size == 1)
            {
                value = (int8_t)data[0];
            }
            else if (size == 2)
            {
                value = (int16_t)(data[0] | (data[1] << 8));
            }
        }
        else
        {
            if (size == 1)
            {
                value = (uint8_t)data[0];
            }
            else if (size == 2)
            {
                value = (uint16_t)(data[0] | (data[1] << 8));
            }
        }
        ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "getValue: data[0]=0x%02X, size=%d, isSigned=%d, value=%d", data[0], size, isSigned, value);
        return value;
    };

    HIDReportDescriptor localHIDReportDesc = {0};

    ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Starting parsing HID report descriptor of length %d", length);

    while (i < length)
    {
        uint8_t prefix = data[i];
        parsedValues.size = (prefix & 0x03);
        parsedValues.size = (parsedValues.size == 3) ? 4 : parsedValues.size;
        uint8_t item = prefix & 0xFC;
        bool isSigned = (item == 0x14 || item == 0x24);
        int16_t value = getValue(data + i + 1, parsedValues.size, isSigned);

        ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "At index %d: prefix=0x%02X, size=%d, item=0x%02X, isSigned=%d", i, prefix, parsedValues.size, item, isSigned);
        ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Value extracted: %d", value);

        switch (item)
        {
        case 0x04: // USAGE_PAGE
            parsedValues.usagePage = (uint8_t)value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "USAGE_PAGE: %d", parsedValues.usagePage);
            break;
        case 0x08: // USAGE
            parsedValues.usage = (uint8_t)value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "USAGE: %d", parsedValues.usage);
            break;
        case 0x84: // REPORT_ID
            parsedValues.reportId = value;
            localHIDReportDesc.reportId = parsedValues.reportId;
            parsedValues.hasReportId = true;
            parsedValues.currentBitOffset += 8;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "REPORT_ID: %d, currentBitOffset: %d", parsedValues.reportId, parsedValues.currentBitOffset);
            break;
        case 0x74: // REPORT_SIZE
            parsedValues.reportSize = value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "REPORT_SIZE: %d", parsedValues.reportSize);
            break;
        case 0x94: // REPORT_COUNT
            parsedValues.reportCount = value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "REPORT_COUNT: %d", parsedValues.reportCount);
            break;
        case 0x14: // LOGICAL_MINIMUM
            if (parsedValues.size == 1)
            {
                parsedValues.logicalMin8 = (int8_t)value;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "LOGICAL_MINIMUM (8-bit): %d", parsedValues.logicalMin8);
            }
            else
            {
                parsedValues.logicalMin16 = value;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "LOGICAL_MINIMUM (16-bit): %d", parsedValues.logicalMin16);
            }
            break;
        case 0x24: // LOGICAL_MAXIMUM
            if (parsedValues.size == 1)
            {
                parsedValues.logicalMax8 = (int8_t)value;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "LOGICAL_MAXIMUM (8-bit): %d", parsedValues.logicalMax8);
            }
            else
            {
                parsedValues.logicalMax = value;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "LOGICAL_MAXIMUM (16-bit): %d", parsedValues.logicalMax);
            }
            break;
        case 0xA0: // COLLECTION
            parsedValues.level++;
            parsedValues.collection = value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "COLLECTION: level=%d, collection=%d", parsedValues.level, parsedValues.collection);
            break;
        case 0xC0: // END_COLLECTION
            parsedValues.level--;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "END_COLLECTION: level=%d", parsedValues.level);
            break;
        case 0x80: // INPUT
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "INPUT detected");
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Current usagePage: %d, usage: %d", parsedValues.usagePage, parsedValues.usage);
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Logical min: %d, Logical max: %d", parsedValues.logicalMin16, parsedValues.logicalMax);

            // Handle X and Y axis (Usage Page 0x01 and Usage 0x30 or 0x31)
            if (parsedValues.usagePage == 0x01 && (parsedValues.usage == 0x30 || parsedValues.usage == 0x31))
            {
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Processing X/Y axis");
                if (parsedValues.logicalMax <= 2047)
                {
                    // Handle 12-bit range for X and Y axis
                    localHIDReportDesc.xAxisSize = 12;
                    localHIDReportDesc.xAxisStartByte = parsedValues.currentBitOffset / 8;
                    parsedValues.currentBitOffset += 12;
                    localHIDReportDesc.yAxisSize = 12;
                    localHIDReportDesc.yAxisStartByte = parsedValues.currentBitOffset / 8;
                    parsedValues.currentBitOffset += 12;
                    ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "X and Y axis (12-bit): xAxisSize=%d, yAxisSize=%d, xAxisStartByte=%d, yAxisStartByte=%d", localHIDReportDesc.xAxisSize, localHIDReportDesc.yAxisSize, localHIDReportDesc.xAxisStartByte, localHIDReportDesc.yAxisStartByte);
                }
                else
                {
                    // Handle 8-bit or 16-bit ranges
                    uint8_t axisSize = (parsedValues.logicalMax <= 127) ? 8 : 16;
                    localHIDReportDesc.xAxisSize = axisSize;
                    localHIDReportDesc.xAxisStartByte = parsedValues.currentBitOffset / 8;
                    parsedValues.currentBitOffset += axisSize;
                    localHIDReportDesc.yAxisSize = axisSize;
                    localHIDReportDesc.yAxisStartByte = parsedValues.currentBitOffset / 8;
                    parsedValues.currentBitOffset += axisSize;
                    ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "X and Y axis (%d-bit): xAxisSize=%d, yAxisSize=%d, xAxisStartByte=%d, yAxisStartByte=%d", axisSize, localHIDReportDesc.xAxisSize, localHIDReportDesc.yAxisSize, localHIDReportDesc.xAxisStartByte, localHIDReportDesc.yAxisStartByte);
                }
            }
            // Handle wheel movement (Usage Page 0x01 and Usage 0x38)
            else if (parsedValues.usagePage == 0x01 && parsedValues.usage == 0x38)
            {
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Processing wheel movement");
                localHIDReportDesc.wheelSize = (parsedValues.logicalMax <= 127 && parsedValues.logicalMax >= -128) ? 8 : 16;
                localHIDReportDesc.wheelStartByte = parsedValues.currentBitOffset / 8;
                parsedValues.currentBitOffset += localHIDReportDesc.wheelSize;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Wheel movement: wheelSize=%d, wheelStartByte=%d", localHIDReportDesc.wheelSize, localHIDReportDesc.wheelStartByte);
            }
            // Handle buttons (Usage Page 0x09, Usage Minimum 0x01, Usage Maximum 0x10)
            else if (parsedValues.usagePage == 0x09 && parsedValues.usage >= 0x01 && parsedValues.usage <= 0x10)
            {
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Processing buttons");
                localHIDReportDesc.buttonSize = parsedValues.reportCount * parsedValues.reportSize;
                localHIDReportDesc.buttonStartByte = parsedValues.currentBitOffset / 8;
                parsedValues.currentBitOffset += localHIDReportDesc.buttonSize;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Buttons: buttonSize=%d, buttonStartByte=%d, currentBitOffset=%d", localHIDReportDesc.buttonSize, localHIDReportDesc.buttonStartByte, parsedValues.currentBitOffset);
            }
            else
            {
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Unhandled INPUT usage");
                parsedValues.currentBitOffset += parsedValues.reportSize * parsedValues.reportCount;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Updated currentBitOffset: %d", parsedValues.currentBitOffset);
            }
            break;
        default:
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Unhandled item: 0x%02X", item);
            break;
        }

        i += parsedValues.size + 1;
        ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Moving to next item, index now: %d", i);
    }

    // ===== 不再无条件覆盖全局 HIDReportDesc =====
    //
    // 【老代码的 bug】这里原本是 `HIDReportDesc = localHIDReportDesc;`。
    // 复合设备(键盘+鼠标接收器)会依次送来【每个接口】的报告描述符, 于是
    // 全局那份被反复覆盖, 最终留下的是"最后一个到达的接口"的布局。
    // 若最后到达的是键盘接口(布局与鼠标完全不同), 鼠标解码就会拿键盘的
    // 偏移量去读位移 -> 位移错乱或恒为 0。
    //
    // 现在解析结果由调用方(_onReceiveControl)按接口存进 iface_report_desc[],
    // 全局 HIDReportDesc 只作为"最近一次解析结果"的兼容副本保留 ——
    // 鼠标接口的描述符到达时会被赋值为鼠标那份, 供只认全局的老路径使用。
    //
    // 因此这里【直接返回局部结果】, 由调用方决定是否写全局。
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "Final parsed values:");
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "reportId: %d", localHIDReportDesc.reportId);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "buttonSize: %d", localHIDReportDesc.buttonSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "xAxisSize: %d", localHIDReportDesc.xAxisSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "yAxisSize: %d", localHIDReportDesc.yAxisSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "wheelSize: %d", localHIDReportDesc.wheelSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "buttonStartByte: %d", localHIDReportDesc.buttonStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "xAxisStartByte: %d", localHIDReportDesc.xAxisStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "yAxisStartByte: %d", localHIDReportDesc.yAxisStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "wheelStartByte: %d", localHIDReportDesc.wheelStartByte);

    return localHIDReportDesc;
}

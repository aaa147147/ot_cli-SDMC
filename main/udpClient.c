#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
//#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/uart_types.h"
#include "nvs_flash.h"
#include "openthread/cli.h"
#include "openthread/instance.h"
#include "openthread/logging.h"
#include "openthread/tasklet.h"

//==========================================================
#include <esp_openthread.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>

#include <openthread/udp.h>   //使用这个里面的方法进行UDP通讯
#include "lwip/sockets.h"
#include <esp_openthread.h>
#include <openthread/udp.h>
#include <string.h>
#include "esp_crc.h"
#include "esp_log.h"
#include "openthread/srp_client.h"
//==========================================================
//==========================================================

char myDatasetTlv[] = "0e08000000000001000035060004001fffe00c0402a0f7f8000300000b0208022e012eb6210af50708fd7e2653e276e9b70510022e012eb6210af57e2653e276e9b71b030d5468726561642d3078303232650102022e04100266657f94d5199ac8266e40dd1be5f4"; 

#define UDP_PORT 33521
otUdpSocket mysocket;
otSockAddr sockaddr;
int udp_Receive_State = 0;  //UDP接收状态：0-未接收到数据，1-数据接收完成，2-数据处理完成
otMessageInfo myMessageInfo;
char udp_databuffer[128]; // Adjust buffer size as needed

otSrpClientService mySrpService;
char srpHostName[17] = { 0 };
const char srpServiceName[] = "_ESP32H2._udp";
char srpInstanceName[23] = { 0 };

char udpSendState = 0; //udp发送状态-> 0:等待发送；1:重试状态；

static esp_err_t load_dataset_from_flash(otOperationalDatasetTlvs* DatasetTlv) {
    esp_err_t ret = ESP_OK;
    nvs_handle_t nvs_handle;
    size_t temp;
    // 打开 NVS 分区
    ret = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 从 NVS 读取字节数组
    ret = nvs_get_blob(nvs_handle, "dataset_mTlvs", DatasetTlv->mTlvs, &temp);
    ret = nvs_get_blob(nvs_handle, "dataset_mLength", &(DatasetTlv->mLength), &temp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error getting blob: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    ESP_LOGI(TAG,"active_dataset_mLength=%d",DatasetTlv->mLength);
    // 关闭 NVS 句柄
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Active dataset TLV loaded from flash successfully");
    return ESP_OK;
}

static esp_err_t load_blob_from_flash(otOperationalDatasetTlvs* DatasetTlv,const char*lengthBlob,const char*dataBlob) {
    esp_err_t ret = ESP_OK;
    nvs_handle_t nvs_handle;
    size_t temp;

    // 打开 NVS 分区
    ret = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // 从 NVS 读取字节数组
    ret = nvs_get_blob(nvs_handle, dataBlob, DatasetTlv->mTlvs, &temp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error getting blob for %s: %s", dataBlob, esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_get_blob(nvs_handle, lengthBlob, &(DatasetTlv->mLength), &temp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error getting blob for %s: %s", lengthBlob, esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ESP_LOGI(TAG, "active_dataset_mLength=%d", DatasetTlv->mLength);
    // 关闭 NVS 句柄
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Dataset TLV loaded from flash successfully (Namespace: %s)", "storage");
    return ESP_OK;
}

static void set_Extaddr(void *aContext){
    otError error;
    otExtAddress eui64;
    otLinkGetFactoryAssignedIeeeEui64(esp_openthread_get_instance(), &eui64);
    error = otLinkSetExtendedAddress(esp_openthread_get_instance(), &eui64);
    if (error != OT_ERROR_NONE) {
        ESP_LOGE("set_Extaddr", "Failed to otLinkSetExtendedAddress: %s", otThreadErrorToString(error));
        return;
    }
    ESP_LOGI("set_Extaddr","set_Extaddr finish");
}

/**
 * 处理角色变化事件的回调函数
 * 
 * 当OpenThread实例的角色发生变化时，此函数会被调用它根据角色变化
 * 执行相应的操作，例如记录日志等
 * 
 * @param aFlags 角色变化的标志位，用于检查哪些角色发生了变化
 * @param aContext 上下文环境的指针，通常用于传递附加信息，在此函数中未使用
 */
static void handle_role_changed(otChangedFlags aFlags, void *aContext) {
    // 获取OpenThread实例
    otInstance *instance = esp_openthread_get_instance();

    // 检查是否是Thread角色发生了变化
    if (aFlags & OT_CHANGED_THREAD_ROLE) {
        // 获取当前的设备角色
        otDeviceRole role = otThreadGetDeviceRole(instance);
        // 根据不同的角色执行相应的操作
        switch (role) {
            case OT_DEVICE_ROLE_DISABLED:
                set_Extaddr(NULL);
                ESP_LOGI("OT_ROLE", "Role: Disabled");
                break;
            case OT_DEVICE_ROLE_DETACHED:
                ESP_LOGI("OT_ROLE", "Role: Detached");
                break;
            case OT_DEVICE_ROLE_CHILD:
                ESP_LOGI("OT_ROLE", "Role: Child");
                break;
            case OT_DEVICE_ROLE_ROUTER:
                ESP_LOGI("OT_ROLE", "Role: Router");
                break;
            case OT_DEVICE_ROLE_LEADER:
                ESP_LOGI("OT_ROLE", "Role: Leader");
                break;
            default:
                set_Extaddr(NULL);
                ESP_LOGI("OT_ROLE", "Role: Unknown");
                break;
        }
        // 根据角色变化执行相应的操作
    }
}

int hexCharToByte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
int hexStringToBytes(const char *str, uint8_t *bytes, size_t maxLen) {
    size_t len = strlen(str);
    if (len % 2 != 0) return -1;  // 字符串长度必须是偶数

    size_t numBytes = len / 2;
    if (numBytes > maxLen) return -1;  // 超过最大长度

    for (size_t i = 0; i < numBytes; i++) {
        int hi = hexCharToByte(str[i * 2]);
        int lo = hexCharToByte(str[i * 2 + 1]);
        if (hi == -1 || lo == -1) return -1;  // 非法字符

        bytes[i] = (hi << 4) | lo;
    }
    return numBytes;
}


static void srpClientinit(void *aContext) {
    otError error;    
    // 初始化 SRP 客户端
    const otExtAddress *extAddress = otLinkGetExtendedAddress(esp_openthread_get_instance());
    // 将扩展MAC地址转换成字符串
    snprintf(srpHostName, sizeof(srpHostName), "%02X%02X%02X%02X%02X%02X%02X%02X",
             extAddress->m8[0], extAddress->m8[1], extAddress->m8[2], extAddress->m8[3],
             extAddress->m8[4], extAddress->m8[5], extAddress->m8[6], extAddress->m8[7]);    
    ESP_LOGI("srpClient","srpHostName=%s",srpHostName);    
    
    snprintf(srpInstanceName, sizeof(srpInstanceName), "SDMC_%02X%02X%02X%02X%02X%02X%02X%02X",
             extAddress->m8[0], extAddress->m8[1], extAddress->m8[2], extAddress->m8[3],
             extAddress->m8[4], extAddress->m8[5], extAddress->m8[6], extAddress->m8[7]);
    ESP_LOGI("srpClient","srpInstanceName=%s",srpInstanceName);    

    error = otSrpClientSetHostName(esp_openthread_get_instance(), srpHostName); // srp client host name my-host
    if (error != OT_ERROR_NONE) {
        ESP_LOGE("srpClient", "Failed to otSrpClientSetHostName: %s", otThreadErrorToString(error));
        return;
    }
    error = otSrpClientEnableAutoHostAddress(esp_openthread_get_instance()); // srp client host address auto
    if (error != OT_ERROR_NONE) {
        ESP_LOGE("srpClient", "Failed to otSrpClientEnableAutoHostAddress: %s", otThreadErrorToString(error));
        return;
    }
    
    memset(&mySrpService,0,sizeof(otSrpClientService));
    mySrpService.mName = srpServiceName;          // The service labels (e.g., "_mt._udp", not the full domain name).
    mySrpService.mInstanceName = srpInstanceName; // The service instance name label (not the full name).
    mySrpService.mSubTypeLabels = NULL;           // Array of sub-type labels (must end with `NULL` or can be `NULL`).
    mySrpService.mPort = UDP_PORT;                   // The service port number.
    mySrpService.mPriority = 0;                   // The service priority.
    mySrpService.mWeight = 0;                     // The service weight.
    mySrpService.mLease = 7200;                    // Desired lease interval in sec - zero to use default.
    mySrpService.mKeyLease = 7200;                 // Desired key lease interval in sec - zero to use default.
    mySrpService.mNumTxtEntries = 0;

    error = otSrpClientAddService(esp_openthread_get_instance(), &mySrpService); // srp client service add sdmc _mt._udp 12345
    if (error != OT_ERROR_NONE) {
        ESP_LOGE("srpClient", "Failed to otSrpClientAddService: %s", otThreadErrorToString(error));
        return;
    }
    otSrpClientEnableAutoStartMode(esp_openthread_get_instance(), NULL, NULL);  // srp client autostart enable
    ESP_LOGI("srpClient","srpClientinit finish");
}



/**
 * @brief UDP接收回调函数，用于处理接收到的UDP消息
 * 
 * 当UDP消息到达时，此函数会被调用。它根据当前的接收状态决定是否读取和处理消息。
 * 如果接收状态不为1-数据接收完成，则从消息中读取数据，将其转换为字符串，并记录日志信息。
 * 
 * @param aContext 保留参数，未使用
 * @param aMessage 接收到的消息对象
 * @param aMessageInfo 消息的附加信息，包括发送方的地址和端口
 */
static void udp_receive_callback(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo) 
{
    uint16_t length = 0;
    // 检查UDP接收状态，如果不为1，则准备接收和处理数据
    if(udp_Receive_State != 1)
    {
        // 从消息中读取数据到缓冲区
        length = otMessageRead(aMessage, 0, udp_databuffer, sizeof(udp_databuffer) - 1);
        // 在字符串末尾添加空字符终止
        udp_databuffer[length] = '\0'; 

        // 创建一个足够大的字符串数组来存储IP地址
        char ip_string[INET6_ADDRSTRLEN];
        // 将发送方的IPv6地址转换为可读的字符串形式
        inet_ntop(AF_INET6, &aMessageInfo->mPeerAddr, ip_string, INET6_ADDRSTRLEN);
        // 记录接收到的数据信息，包括数据内容、发送方IP地址和端口
        ESP_LOGI("udp_client", "Received data: %s [from %s:%d]", udp_databuffer, ip_string, aMessageInfo->mPeerPort);
        //保存收到数据的IP地址和端口号用于后续回复消息
        memcpy(&myMessageInfo, aMessageInfo, sizeof(*aMessageInfo));
        
        // 更新UDP接收状态为1，表示已接收到数据
        udp_Receive_State = 1;
    }
}

/**
 * @brief 初始化UDP客户端
 * 
 * 该函数负责初始化UDP客户端，包括打开UDP套接字和绑定端口，为接收UDP消息做准备
 * 它使用OpenThread API来操作UDP层
 * 
 * @param aContext 上下文环境，未使用
 */
static void udp_client_init(void *aContext)
{
    // 获取OpenThread实例
    otInstance *instance = esp_openthread_get_instance();
    // 用于存储OpenThread API调用的结果
    otError error;
    
    // 初始化UDP套接字结构体
    memset(&mysocket, 0, sizeof(mysocket));
    
    // 打开UDP套接字
    error = otUdpOpen(instance, &mysocket, udp_receive_callback, NULL);
    if (error != OT_ERROR_NONE) {
        // 如果打开UDP套接字失败，记录错误并返回
        ESP_LOGE("udp_client", "Failed to open UDP socket: %s", otThreadErrorToString(error));
        return;
    }
    
    // 初始化UDP套接字地址结构体
    memset(&sockaddr, 0, sizeof(sockaddr));
    // 设置监听的UDP端口
    sockaddr.mPort = UDP_PORT;
    
    // 绑定UDP套接字到指定端口
    error = otUdpBind(instance, &mysocket, &sockaddr, OT_NETIF_THREAD_HOST);
    if (error != OT_ERROR_NONE) {
        // 如果绑定UDP套接字失败，记录错误并关闭UDP套接字
        ESP_LOGE("udp_client", "Failed to bind UDP socket: %s", otThreadErrorToString(error));
        otUdpClose(instance, &mysocket);
        return;
    }
    
    // 初始化UDP接收状态
    udp_Receive_State = 0;
    
    // 记录UDP监听开始的信息
    ESP_LOGI("udp_client", "Listening on UDP port %d", UDP_PORT);
}

static void ledFlash(void *aContext){      //灯闪烁
    otInstance *instance = esp_openthread_get_instance();
    otDeviceRole role = otThreadGetDeviceRole(instance);
    esp_openthread_state_indicator_set(0, 100, 0, 100);  
    vTaskDelay(pdMS_TO_TICKS(100));

    if (role == OT_DEVICE_ROLE_DISABLED) {
        esp_openthread_state_indicator_clear();
    } else if (role == OT_DEVICE_ROLE_DETACHED) {
        esp_openthread_state_indicator_set(0, CONFIG_DETACHED_INDICATOR_RED, CONFIG_DETACHED_INDICATOR_GREEN, CONFIG_DETACHED_INDICATOR_BLUE);
    } else if (role == OT_DEVICE_ROLE_LEADER) {
        esp_openthread_state_indicator_set(0, CONFIG_LEADER_INDICATOR_RED, CONFIG_LEADER_INDICATOR_GREEN, CONFIG_LEADER_INDICATOR_BLUE);
    } else if (role == OT_DEVICE_ROLE_ROUTER) {
        esp_openthread_state_indicator_set(0, CONFIG_ROUTER_INDICATOR_RED, CONFIG_ROUTER_INDICATOR_GREEN, CONFIG_ROUTER_INDICATOR_BLUE);
    } else if (role == OT_DEVICE_ROLE_CHILD) {
        esp_openthread_state_indicator_set(0, CONFIG_CHILD_INDICATOR_RED, CONFIG_CHILD_INDICATOR_GREEN, CONFIG_CHILD_INDICATOR_BLUE);
    }
} 
/**
 * @brief 生成指定长度的随机数字符串
 * 
 * @param buffer 存储随机数字符串的缓冲区
 * @param length 随机数字符串的长度
 */
void generate_random_number(char *buffer, size_t length) {
    // 使用当前时间作为随机数种子
    srand(time(NULL));

    // 填充缓冲区，直到达到指定长度
    for (size_t i = 0; i < length; ++i) {
        // 生成0-9之间的随机数字，并转换为字符
        buffer[i] = '0' + rand() % 10;
    }

    // 在字符串末尾添加空字符终止
    buffer[length] = '\0';
}

static uint32_t calculate_crc(const uint8_t *data, size_t length)
{
    return esp_crc32_le(0, data, length);
}

static void send_udp_message(const char *message, const otMessageInfo *messageInfo)
{
    otMessage *udpMessage = otUdpNewMessage(esp_openthread_get_instance(), NULL);
    if (udpMessage == NULL) {
        ESP_LOGE("udp_client", "Failed to create message");
        return;
    }

    otError appendError = otMessageAppend(udpMessage, message, strlen(message));
    if (appendError != OT_ERROR_NONE) {
        ESP_LOGE("udp_client", "Failed to append message: %s", otThreadErrorToString(appendError));
        otMessageFree(udpMessage);
        return;
    }

    ESP_LOGI("udp_client", "otMessageGetLength:%d", otMessageGetLength(udpMessage));

    char ip_string[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &messageInfo->mPeerAddr, ip_string, INET6_ADDRSTRLEN);
    ESP_LOGI("udp_client", "Destination Address: %s-%d", ip_string, messageInfo->mPeerPort);

    otError sendError = otUdpSend(esp_openthread_get_instance(), &mysocket, udpMessage, messageInfo);
    if (sendError != OT_ERROR_NONE) {
        ESP_LOGE("udp_client", "Failed to send UDP message: %s", otThreadErrorToString(sendError));
    } else {
        ESP_LOGI("udp_client", "UDP message sent successfully");
    }
    ledFlash(NULL);
}

static void handle_received_data(void)
{
    // 计算接收数据的CRC校验值
    uint32_t calculated_crc = calculate_crc((uint8_t*)udp_databuffer, sizeof(udp_databuffer));
    ESP_LOGI("udp_client", "udp_databuffer CRC: %u,[%s]", (unsigned int)calculated_crc, udp_databuffer);

    switch (calculated_crc)
    {
        case 4216599461:  //{"Name":"Data_Transmission_Testing","Value":"Data_Transmission_Testing","Message":"Data_Transmission_Testing"}
            const char *response = "{\"Name\":\"Data_Transmission_Testing\",\"Value\":\"Data_Transmission_Testing\",\"Message\":\"Copy\"}\0";
            send_udp_message(response, &myMessageInfo);
            break;
        case 2959270098: //{"Name":"Data_Transmission_Testing","Value":"Data_Transmission_Testing","Message":"Copy"}
            if (udpSendState == 1){
                ESP_LOGI("udp_client", "Message:Copy");
                udpSendState = 0; //udp发送状态-> 0:等待发送；1:重试状态；
            }
            break;
        default:
            break;
    }

    // 清空接收数据缓冲区
    memset(udp_databuffer, 0, sizeof(udp_databuffer));
    // 重置接收状态
    udp_Receive_State = 2;
}
    

static esp_err_t periodic_tasks(const char *enableTX)
{
    esp_err_t ret = ESP_OK;
    // 获取srp server的IP地址
    otInstance *instance = esp_openthread_get_instance();
    const otSockAddr *serverSockAddr = otSrpClientGetServerAddress(instance);
    const otSrpClientHostInfo *srpClientHostInfo = otSrpClientGetHostInfo(instance);

    if (serverSockAddr->mPort != 0) {
        // 将IP地址转换为可读字符串
        char ip_string[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &serverSockAddr->mAddress, ip_string, INET6_ADDRSTRLEN);
        // SRP server信息
        ESP_LOGI("udp_client", "srpServerIP: %s:%d", ip_string, (*serverSockAddr).mPort);

        // 检查SRP客户端是否已经注册
        if (srpClientHostInfo->mState == OT_SRP_CLIENT_ITEM_STATE_REGISTERED) {
            // 动态构建JSON响应字符串，包含srpHostName
            char response[256];
            snprintf(response, sizeof(response), "{\"Name\":\"Data_Transmission_Testing\",\"Value\":\"%s\",\"Message\":\"Data_Transmission_Testing\"}", srpHostName);

            // 创建otMessageInfo结构体
            otMessageInfo messageInfo;
            memset(&messageInfo, 0, sizeof(messageInfo));
            messageInfo.mPeerAddr = serverSockAddr->mAddress;
            messageInfo.mPeerPort = UDP_PORT;

            // 发送消息到SRP服务器
            send_udp_message(response, &messageInfo);

        } else {
            ESP_LOGW("udp_client", "Srp Client Host is not REGISTERED");
            ret = ESP_FAIL;
        }
    }else {
        ESP_LOGW("udp_client", "Can not found Srp Server");
        ret = ESP_FAIL;
    }
    return ret;
}

static void udp_client_mainloop(void *enableTX)
{
    uint32_t temp_count = 0;
    static int retry_count = 0; //重试次数
    const int max_retries = 5; // 最大重试次数

    ESP_LOGI("udp_client", "client_mainloop started with enableTX=%x", *(char*)enableTX );
    // 初始化UDP客户端
    udp_client_init(NULL);

    while (1) {
        // 短暂延迟以避免忙等待
        vTaskDelay(pdMS_TO_TICKS(10));
        temp_count++;

        // 检查是否有接收的数据
        if (udp_Receive_State == 1) {
            handle_received_data();
        }
        if (0x01 == *(char*)enableTX){
            switch (udpSendState)
            {
                case 0:
                    if (temp_count >= 100 * 60) {  // 每分钟获取一次SRP服务端的IP地址,并向SRP服务端发送一次数据
                        temp_count = 0;
                        if (ESP_OK == periodic_tasks(enableTX)){
                            udpSendState = 1; //udp发送状态-> 0:等待发送；1:重试状态；
                        }
                    }
                    break;
                case 1:  //重试状态
                    if (temp_count >= 100 * 2) {  //超时接收2s，如果没有收到数据，重复发送数据
                        temp_count = 0;
                        if (ESP_OK == periodic_tasks(enableTX)){
                            udpSendState = 1; //udp发送状态-> 0:等待发送；1:重试状态；
                            retry_count = retry_count + 1;
                        }
                    }
                    break;
                default:
                    break;
            }
            if(retry_count >= max_retries){
                udpSendState = 0; //udp发送状态-> 0:等待发送；1:重试状态；
            }
        }

    }
}
//==========================================================






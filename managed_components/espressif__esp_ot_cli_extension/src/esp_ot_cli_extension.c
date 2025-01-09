/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_ot_cli_extension.h"
#include "esp_openthread.h"
#include "esp_ot_curl.h"
#include "esp_ot_dns64.h"
#include "esp_ot_heap_diag.h"
#include "esp_ot_ip.h"
#include "esp_ot_iperf.h"
#include "esp_ot_loglevel.h"
#include "esp_ot_nvs_diag.h"
#include "esp_ot_ota_commands.h"
#include "esp_ot_rcp_commands.h"
#include "esp_ot_tcp_socket.h"
#include "esp_ot_udp_socket.h"
#include "esp_ot_wifi_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "openthread/cli.h"


#include "nvs_flash.h"
#include <esp_openthread.h>
#include <openthread/dataset.h>
#include "esp_log.h"


esp_err_t myNvs_save_data(otOperationalDatasetTlvs* tlvs,const char*lengthBlob,const char*dataBlob)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = ESP_OK;
    
    // 打开 NVS 分区
    ret = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE("sdmcapp", "Error opening NVS handle: %s", esp_err_to_name(ret));
        return OT_ERROR_FAILED;
    }
    // 将字节数组保存到 NVS
    ret = nvs_set_blob(nvs_handle, lengthBlob, &(tlvs->mLength), 1);
    ret = nvs_set_blob(nvs_handle, dataBlob, tlvs->mTlvs, OT_OPERATIONAL_DATASET_MAX_LENGTH);
    if (ret != ESP_OK) {
        ESP_LOGE("sdmcapp", "Error setting blob: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return OT_ERROR_FAILED;
    }
    // 提交更改
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE("sdmcapp", "Error committing changes: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return OT_ERROR_FAILED;
    }
    // 关闭 NVS 句柄
    nvs_close(nvs_handle);
    return ESP_OK;
}

otError sdmcapp(void *aContext, uint8_t aArgsLength, char *aArgs[])
{
    esp_err_t ret = ESP_OK;
    otOperationalDataset dataset;
    otOperationalDatasetTlvs tlvs;
    
    otInstance *instance = esp_openthread_get_instance();

    //清空数据
    tlvs.mLength = 0;
    memset(tlvs.mTlvs,0,OT_OPERATIONAL_DATASET_MAX_LENGTH);  //保存全0数据


    if (aArgsLength == 0){
        otCliOutputFormat("---sdmcapp parameter---\n");
        otCliOutputFormat("savedataset                :     save dataset active to flash\n");
        otCliOutputFormat("cleardataset               :     clear dataset active of flash\n");
        otCliOutputFormat("enableTX                   :     enable transmission test\n");
        otCliOutputFormat("disableTX                  :     disable transmission test\n");
        otCliOutputFormat("\nExample of modify dataset in flash:\n");
        otCliOutputFormat("dataset set active XXXXX\n");
        otCliOutputFormat("sdmcapp savedataset\n");
        otCliOutputFormat("sdmcapp enableTX\n");        
        otCliOutputFormat("factoryreset\n");

    }else if (strcmp(aArgs[0], "savedataset") == 0){
        // 获取当前活动的 dataset
        otDatasetGetActive(instance, &dataset);
        otDatasetGetActiveTlvs(instance, &tlvs);
        
        ret = myNvs_save_data(&tlvs,"dataset_mLength","dataset_mTlvs");
        if (ESP_OK != ret)
        {
            return ret;
        }
        
        ESP_LOGI("sdmcapp", "Active dataset TLV saved to flash successfully");
    }else if (strcmp(aArgs[0], "cleardataset") == 0){

        //保存清空的数据
        ret = myNvs_save_data(&tlvs,"dataset_mLength","dataset_mTlvs");
        if (ESP_OK != ret)
        {
            return ret;
        }
        
        ESP_LOGI("sdmcapp", "Active dataset TLV clear successfully");
    }else if (strcmp(aArgs[0], "enableTX") == 0){

        tlvs.mLength = 1;
        tlvs.mTlvs[0] = 0x01;
        ret = myNvs_save_data(&tlvs,"enTX_mLength","enTX_mTlvs");
        if (ESP_OK != ret)
        {
            return ret;
        }
        
        ESP_LOGI("sdmcapp", "Active transmission has been enabled");

    }else if (strcmp(aArgs[0], "disableTX") == 0){

        tlvs.mLength = 1;
        tlvs.mTlvs[0] = 0x00;
        ret = myNvs_save_data(&tlvs,"enTX_mLength","enTX_mTlvs");
        if (ESP_OK != ret)
        {
            return ret;
        }
        
        ESP_LOGI("sdmcapp", "Active transmission has been disabled");

    }

    else {
        otCliOutputFormat("invalid commands\n");
    }
    return OT_ERROR_NONE;
}


static const otCliCommand kCommands[] = {
    {"sdmcapp", sdmcapp},
    {"curl", esp_openthread_process_curl},
#if CONFIG_OPENTHREAD_DNS64_CLIENT
    {"dns64server", esp_openthread_process_dns64_server},
#endif // CONFIG_OPENTHREAD_DNS64_CLIENT
    {"heapdiag", esp_ot_process_heap_diag},
    {"ip", esp_ot_process_ip},
    {"iperf", esp_ot_process_iperf},
    {"loglevel", esp_ot_process_logset},
    {"mcast", esp_ot_process_mcast_group},
#if CONFIG_OPENTHREAD_NVS_DIAG
    {"nvsdiag", esp_ot_process_nvs_diag},
#endif // CONFIG_OPENTHREAD_NVS_DIAG
#if CONFIG_OPENTHREAD_CLI_OTA
    {"ota", esp_openthread_process_ota_command},
#endif // CONFIG_OPENTHREAD_CLI_OTA
#if CONFIG_OPENTHREAD_RCP_COMMAND
    {"otrcp", esp_openthread_process_rcp_command},
#endif // CONFIG_OPENTHREAD_RCP_COMMAND
    {"tcpsockclient", esp_ot_process_tcp_client},
    {"tcpsockserver", esp_ot_process_tcp_server},
    {"udpsockclient", esp_ot_process_udp_client},
    {"udpsockserver", esp_ot_process_udp_server},
#if CONFIG_OPENTHREAD_CLI_WIFI
    {"wifi", esp_ot_process_wifi_cmd},
#endif // CONFIG_OPENTHREAD_CLI_WIFI
};

void esp_cli_custom_command_init()
{
    esp_ot_heap_diag_init();
    otInstance *instance = esp_openthread_get_instance();
    otCliSetUserCommands(kCommands, (sizeof(kCommands) / sizeof(kCommands[0])), instance);
}

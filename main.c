// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdio.h>
#include <stdlib.h>

#include "iothub_module_client_ll.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "iothubtransportmqtt.h"
#include "iothub.h"
#include "time.h"

typedef struct FILTERED_MESSAGE_INSTANCE_TAG
{
    IOTHUB_MESSAGE_HANDLE messageHandle;
    size_t messageTrackingId;  // For tracking the messages within the user callback.
} 
FILTERED_MESSAGE_INSTANCE;

size_t messagesReceivedByInput1Queue = 0;

// SendConfirmationCallbackFromFilter is invoked when the message that was forwarded on from 'InputQueue1FilterCallback'
// pipeline function is confirmed.
static void SendConfirmationCallbackFromFilter(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    // The context corresponds to which message# we were at when we sent.
    FILTERED_MESSAGE_INSTANCE* filteredMessageInstance = (FILTERED_MESSAGE_INSTANCE*)userContextCallback;
    printf("Confirmation[%zu] received for message with result = %d\r\n", filteredMessageInstance->messageTrackingId, result);
    IoTHubMessage_Destroy(filteredMessageInstance->messageHandle);
    free(filteredMessageInstance);
}

// Allocates a context for callback and clones the message
// NOTE: The message MUST be cloned at this stage.  InputQueue1FilterCallback's caller always frees the message
// so we need to pass down a new copy.
static FILTERED_MESSAGE_INSTANCE* CreateFilteredMessageInstance(IOTHUB_MESSAGE_HANDLE message)
{
    FILTERED_MESSAGE_INSTANCE* filteredMessageInstance = (FILTERED_MESSAGE_INSTANCE*)malloc(sizeof(FILTERED_MESSAGE_INSTANCE));
    if (NULL == filteredMessageInstance)
    {
        printf("Failed allocating 'FILTERED_MESSAGE_INSTANCE' for pipelined message\r\n");
    }
    else
    {
        memset(filteredMessageInstance, 0, sizeof(*filteredMessageInstance));

        if ((filteredMessageInstance->messageHandle = IoTHubMessage_Clone(message)) == NULL)
        {
            free(filteredMessageInstance);
            filteredMessageInstance = NULL;
        }
        else
        {
            filteredMessageInstance->messageTrackingId = messagesReceivedByInput1Queue;
        }
    }

    return filteredMessageInstance;
}

// InputQueue1FilterCallback implements a filtering mechanism.
static IOTHUBMESSAGE_DISPOSITION_RESULT InputQueue1FilterCallback(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{
    IOTHUBMESSAGE_DISPOSITION_RESULT result;
    IOTHUB_CLIENT_RESULT clientResult;
    IOTHUB_MODULE_CLIENT_LL_HANDLE iotHubModuleClientHandle = (IOTHUB_MODULE_CLIENT_LL_HANDLE)userContextCallback;

    unsigned const char* messageBody;
    size_t contentSize;

    if (IoTHubMessage_GetByteArray(message, &messageBody, &contentSize) != IOTHUB_MESSAGE_OK)
    {
        messageBody = "<null>";
    }

    printf("Received Message [%zu]\r\n Data: [%s]\r\n", 
            messagesReceivedByInput1Queue, messageBody);

    // This message should be sent to next stop in the pipeline, namely "output1".  What happens at "outpu1" is determined
    // by the configuration of the Edge routing table setup.
    FILTERED_MESSAGE_INSTANCE *filteredMessageInstance = CreateFilteredMessageInstance(message);
    if (NULL == filteredMessageInstance)
    {
        result = IOTHUBMESSAGE_ABANDONED;
    }
    else
    {
        printf("Sending message (%zu) to the next stage in pipeline\n", messagesReceivedByInput1Queue);

        clientResult = IoTHubModuleClient_LL_SendEventToOutputAsync(iotHubModuleClientHandle, filteredMessageInstance->messageHandle, "output1", SendConfirmationCallbackFromFilter, (void *)filteredMessageInstance);
        if (clientResult != IOTHUB_CLIENT_OK)
        {
            IoTHubMessage_Destroy(filteredMessageInstance->messageHandle);
            free(filteredMessageInstance);
            printf("IoTHubModuleClient_LL_SendEventToOutputAsync failed on sending msg#=%zu, err=%d\n", messagesReceivedByInput1Queue, clientResult);
            result = IOTHUBMESSAGE_ABANDONED;
        }
        else
        {
            result = IOTHUBMESSAGE_ACCEPTED;
        }
    }

    messagesReceivedByInput1Queue++;
    return result;
}

static IOTHUB_MODULE_CLIENT_LL_HANDLE InitializeConnectionForFilter()
{
    IOTHUB_MODULE_CLIENT_LL_HANDLE iotHubModuleClientHandle;

    if (IoTHub_Init() != 0)
    {
        printf("Failed to initialize the platform.\r\n");
        iotHubModuleClientHandle = NULL;
    }
    else if ((iotHubModuleClientHandle = IoTHubModuleClient_LL_CreateFromEnvironment(MQTT_Protocol)) == NULL)
    {
        printf("ERROR: IoTHubModuleClient_LL_CreateFromEnvironment failed\r\n");
    }
    else
    {
        // Uncomment the following lines to enable verbose logging.
        // bool traceOn = true;
        // IoTHubModuleClient_LL_SetOption(iotHubModuleClientHandle, OPTION_LOG_TRACE, &trace);
    }

    return iotHubModuleClientHandle;
}

static void DeInitializeConnectionForFilter(IOTHUB_MODULE_CLIENT_LL_HANDLE iotHubModuleClientHandle)
{
    if (iotHubModuleClientHandle != NULL)
    {
        IoTHubModuleClient_LL_Destroy(iotHubModuleClientHandle);
    }
    IoTHub_Deinit();
}

static int SetupCallbacksForModule(IOTHUB_MODULE_CLIENT_LL_HANDLE iotHubModuleClientHandle)
{
    int ret;

    if (IoTHubModuleClient_LL_SetInputMessageCallback(iotHubModuleClientHandle, "input1", InputQueue1FilterCallback, (void*)iotHubModuleClientHandle) != IOTHUB_CLIENT_OK)
    {
        printf("ERROR: IoTHubModuleClient_LL_SetInputMessageCallback(\"input1\")..........FAILED!\r\n");
        ret = __FAILURE__;
    }
    else
    {
        ret = 0;
    }

    return ret;
}

void iothub_module()
{
    IOTHUB_MODULE_CLIENT_LL_HANDLE iotHubModuleClientHandle;

    srand((unsigned int)time(NULL));

    if ((iotHubModuleClientHandle = InitializeConnectionForFilter()) == NULL)
    {
        ;
    }
    else if (SetupCallbacksForModule(iotHubModuleClientHandle) != 0)
    {
        ;
    }
    else
    {
        // The receiver just loops constantly waiting for messages.
        printf("Waiting for incoming messages.\r\n");
        while (true)
        {
            IoTHubModuleClient_LL_DoWork(iotHubModuleClientHandle);
            ThreadAPI_Sleep(100);
        }
    }

    DeInitializeConnectionForFilter(iotHubModuleClientHandle);
}

int main(void)
{
    iothub_module();
    return 0;
}

#include "scomTask.h"
#if SCOM_TASK_ENABLED

TaskHandle_t scomTask;
TaskHandle_t mainTask;
bool scomChanged = false;
bool printEndPacket = false;
bool mainTaskDone = false;
int displayBufferSize = 0;

bool mainLoopWaiting = false;

#define START_END_PACKET_LENGTH 16
const uint8_t startPacket[START_END_PACKET_LENGTH] = {116, 104, 105, 115, 105, 115, 97, 115, 116, 97, 114, 116, 112, 97, 99, 107};
const uint8_t endPacket[START_END_PACKET_LENGTH] = {116, 104, 105, 115, 105, 115, 97, 101, 110, 100, 100, 100, 112, 97, 99, 107};

/*
To make this work, to compile go to your screen class definition, for me it's
.pio/libdeps/max/GxEPD2/src/GxEPD2_BW.h (just right click at the display variable value definition )
and move the _buffer from private to public
*/
void printPacket(const uint8_t *packet)
{
    for (int i = 0; i < START_END_PACKET_LENGTH; i++)
    {
        Serial.write(packet[i]);
    }
}

void loopScomTask(void *parameter)
{
    debugLog("dis->width(): " + String(dis->width()));
    debugLog("dis->height():" + String(dis->height()));
    displayBufferSize = (int(dis->width()) / 8) * int(dis->height()); // GxEPD2_Type::WIDTH / 8) * page_height
    debugLog("Buffer size is:" + String(displayBufferSize));
    Serial.setTimeout(300);

    delayTask(4500);
    while (true)
    {
        // debugLog("iterating...");
        serialWrite.lock();
        // long-back-button:
        // back-button:
        if (Serial.available() > 1)
        {
            String cmd = Serial.readStringUntil(':');
            serialWrite.unlock();
            if (cmd.lastIndexOf("button") != -1)
            {
                if (cmd.lastIndexOf("back") != -1)
                {
                    if (cmd.lastIndexOf("long") != -1)
                    {
                        setButton(LongBack);
                    }
                    else
                    {
                        setButton(Back);
                    }
                }
                else if (cmd.lastIndexOf("menu") != -1)
                {
                    if (cmd.lastIndexOf("long") != -1)
                    {
                        setButton(LongMenu);
                    }
                    else
                    {
                        setButton(Menu);
                    }
                }
                else if (cmd.lastIndexOf("up") != -1)
                {
                    if (cmd.lastIndexOf("long") != -1)
                    {
                        setButton(LongUp);
                    }
                    else
                    {
                        setButton(Up);
                    }
                }
                else if (cmd.lastIndexOf("down") != -1)
                {
                    if (cmd.lastIndexOf("long") != -1)
                    {
                        setButton(LongDown);
                    }
                    else
                    {
                        setButton(Down);
                    }
                }
            }
            else if (cmd.lastIndexOf("screen") != -1)
            {
                scomChanged = true;
            }
            else if (cmd.lastIndexOf("reset") != -1)
            {
                debugLog("Resetting the watchy because of scom request!");
                ESP.restart();
            }
            serialWrite.lock();
        }

        bool scomDid = false;
        if (scomChanged == true)
        {
            scomChanged = false;
            scomDid = true;
            uint8_t coppiedBuffer[displayBufferSize] = {0};

            /*
            serialWrite.unlock();
            while(mainLoopWaiting == false) {
                delayTask(10);
            }
            serialWrite.lock();
            //vTaskSuspend(mainTask); //
            //memcpy(coppiedBuffer, dis->_buffer, displayBufferSize);
            for (int i = 0; i < displayBufferSize; i++)
            {
                // (GxEPD2_Type::WIDTH / 8) * page_height
                coppiedBuffer[i] = dis->_buffer[i];
            }
            */

            printPacket(startPacket);
            for (int i = 0; i < displayBufferSize; i++)
            {
                Serial.write(dis->_buffer[i]);
            }
        }
        // Print EOF
        if (printEndPacket == true || scomDid == true)
        {
            printEndPacket = false;
            printPacket(endPacket);
            Serial.write('\n');
            flushLogs();
        }

        serialWrite.unlock();
        if (scomDid == true)
        {
            delayTask(600);
        }
        else
        {
            delayTask(25);
        }
    }
}

void initScomTask()
{
    debugLog("- done");
    xTaskCreate(
        loopScomTask,
        "scomTask",
        TASK_STACK_SCOM, // because of huge list we copy
        NULL,
        SCOM_PRIORITY,
        &scomTask);
}

void getMainTask()
{
    if (mainTaskDone == false)
    {
        mainTaskDone = true;
        mainTask = xTaskGetCurrentTaskHandle();
        debugLog("Set main task done");
    }
}

#endif
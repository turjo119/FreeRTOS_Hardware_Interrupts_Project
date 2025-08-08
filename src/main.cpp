/**
ESP32 "Final Project" with Hardware Timers
Sample the ADC value every 100ms and store the it in a buffer.
Once the buffer has 10 readings, Task A will read the values and 
calculate the average. Task B will echo whatever is wrttien in the
serial terminal and the OLED Display unless the user types "avg" 
where it will print out the average value. 

Date: Auguest 6th, 2025
Author: Rifat Turjo  
*/

// When writing code in PlatformIO
#include <Arduino.h>

// For using OLED Display
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED settings
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Use only core 1
#if CONFIG_FREERTOS_UNICORE
    static const BaseType_t app_cpu = 0;
#else
    static const BaseType_t app_cpu = 1;
#endif

//ADC Sampling
//#define ADC_PIN 34
static const int ADC_PIN = A0;

//Setup Double Buffer each to hold 10 ADC samples
int samples[2][10];

//Buffer Tracking
volatile int writeIndex = 0; // Keep track of which buffer ISR writes to
volatile int samplePos = 0; //index within current buffer

//Global average variable
float globalAverage = 0.0;
SemaphoreHandle_t avgMutex;

//Pointer for ISR to Task A
TaskHandle_t taskAHandle  = NULL;

// Timer to handle adc sampling
hw_timer_t *timer = NULL;

// ISR Timer Function
void IRAM_ATTR onTimer(){
    int adcValue = analogRead(ADC_PIN); // Read from the ADC pin
    
    //Start wrting ADC value into buffer
    samples[writeIndex][samplePos] = adcValue;
    samplePos++; // Move to next chunk of memory

    if (samplePos >= 10){ // When a buffer is filled with 10 ADC samples
        int bufferJustFilled = writeIndex; // Indicate which buffer is full

        //Flip buffer
        writeIndex = 1 - writeIndex;
        samplePos = 0;

        //Notify Task A which buffer is ready
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        
        xTaskNotifyFromISR( taskAHandle, 
                            bufferJustFilled, 
                            eSetValueWithOverwrite, 
                            &xHigherPriorityTaskWoken);
        
        //Lets a higher-priority task (like Task A) run immediately after the ISR.
        // In ESP-IDF, this yield function does not accept a parameter
        //portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR();
                }
    }
}

//Define Task A: waits for ISR and then computes average of 10 ADC samples
void TaskA(void* parameter){
    uint32_t bufferIndex;

    while(true){
        //Wait for notification from ISR
        if(xTaskNotifyWait(0, 0, &bufferIndex, portMAX_DELAY)== pdTRUE){
            //Compute average of the 10 samples
            int sum = 0;
            for(int i = 0; i < 10; i++) {
                sum += samples[bufferIndex][i];
            }

            float average = sum / 10.0;

            //Safely update the global varaible
            //Check if Mutex is available and then access global variable
            if (xSemaphoreTake(avgMutex,portMAX_DELAY) == pdTRUE){
                globalAverage = average;
                //Return Mutex
                xSemaphoreGive(avgMutex);
            }
        }
    }
}
// Helper function to update OLED
void updateOLED(String displayString) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.println(displayString);
    display.display();
}

//Define Task B: echos whatever is written into serial.
//If user types "avg" prints out the average of 10 ADC samples
void TaskB(void* parameter){

    String inputBuffer = ""; //collects characters until a newline

    while(true){
        
        while(Serial.available()) {
            
            //Read from Serial Monitor
            char c = Serial.read();
            Serial.print(c); //echo back to terminal

            if(c =='\n') {
                inputBuffer.trim(); //Remove whitespace
                
                // If user types "avg"
                if(inputBuffer == "avg") {
                    
                    float avgCopy;// for copying the global average
                    
                    //Check if Mutex is available and then access global variable
                    if(xSemaphoreTake(avgMutex, pdMS_TO_TICKS(50))== pdTRUE) {
                        Serial.print("Average: ");
                        //Copy Global average
                        avgCopy = globalAverage;
                        Serial.println(avgCopy);
                        //Return Mutex
                        xSemaphoreGive(avgMutex);
                        
                        //Display average on OLED
                        String avgStr = "Average: " + String(avgCopy);
                        updateOLED(avgStr);
                    }

                    else {
                        Serial.println("ERROR! >_<...Couldn't access average");
                    }
                }

                else {
                    // Only echo user input if NOT "avg"
                    Serial.println(inputBuffer);
                    updateOLED(inputBuffer);
                }

                inputBuffer = ""; //Reset buffer for next command
            }
            
            else {
                inputBuffer +=c; // Copy character into buffer
            }

        }
        //Let me check the serial port, then chill for a moment before checking again
        vTaskDelay(pdMS_TO_TICKS(10)); //Avoid busy-loop. 
    }
}

void setup(){
    Serial.begin(115200);
    delay(1000); //Give time for serial monitor to connect
    
    // Create mutex for protecting globalAverage
    avgMutex = xSemaphoreCreateMutex();
    if (avgMutex == NULL) {
        Serial.println("Failed to create mutex!");
        while(true);
    }

    //Create Task A (ADC processing)
    xTaskCreatePinnedToCore(
        TaskA,  //Task function
        "TaskA", // Name
        2048, //Stack size
        NULL, //Parameter
        1, //Priority
        &taskAHandle, // Task Handle
        0 // Set to Core 0
    );

    // Create Task B (Serial interaction)
    xTaskCreatePinnedToCore(
        TaskB, //Task function
        "TaskB", //Name
        2048, // Stack Size
        NULL, //Parameter
        1, //Priority
        NULL, //Task Handle
        0 // Set to Core 0 (might need to change this)
    );

    // Configure hardware timer (Timer 0, 1us tick)
    timer = timerBegin(0, 80, true); // 80MHz / 80 = 1 MHz => 1 tick = 1 us  
    timerAttachInterrupt(timer, &onTimer, true);
    timerAlarmWrite(timer, 100000, true); // 100,000 us = 100 ms
    timerAlarmEnable(timer); // Start the timer

    //Setup the OLED 
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // 0x3C is most common I²C address
    Serial.println(F("SSD1306 allocation failed"));
    for (;;); // Don't proceed, loop forever
    }

    display.clearDisplay();
    display.setTextSize(1);      // Normal 1:1 pixel scale
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("OLED Ready");
    display.display();
    delay(1000);
    display.clearDisplay();


    vTaskDelete(NULL); //Delete the setup task itself
}

void loop (){
     // loop() can remain empty — FreeRTOS handles scheduling
}


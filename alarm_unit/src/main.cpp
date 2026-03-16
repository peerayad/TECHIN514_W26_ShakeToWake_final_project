/**
 * ============================================================
 *  ESP-NOW RECEIVER + ALARM CLOCK
 *  XIAO ESP32C3 — OLED + X27 Motor + LED + Buzzer + 2 Buttons + B10K Pot
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SwitecX25.h>

// ─── Pin Definitions ────────────────────────────────────────
#define BTN_ENTER   D9
#define BTN_CANCEL  D8
#define LED_PIN     D6
#define BUZZER_PIN  D10
#define POT_PIN     D0
#define M1  D1
#define M2  D2
#define M3  D3
#define M4  D7

// ─── OLED ───────────────────────────────────────────────────
#define SCREEN_W  128
#define SCREEN_H   64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ─── Data struct ────────────────────────────────────────────
typedef struct struct_message { float x, y, z; } struct_message;
struct_message incomingData;

// ─── ESP-NOW State ───────────────────────────────────────────
float gX=0, gY=0, gZ=0;
bool  espnowActive=false, espnowConnected=false;
unsigned long lastDataMs=0;
#define ESPNOW_TIMEOUT_MS 2000

// ─── State Machine ───────────────────────────────────────────
enum AppState { STATE_SHOW, STATE_MENU, STATE_SET_H, STATE_SET_M, STATE_SET_AMPM, STATE_ALARMING };
AppState appState = STATE_SHOW;

// ─── Clock & Alarm ───────────────────────────────────────────
int clkH=0, clkM=0, clkS=50, almH=0, almM=1;
bool almEnabled=true, editAM=true, editingClock=true;
int  editH=12, editM=0, menuSel=0;
unsigned long lastSecMillis=0, lastDisplayMillis=0, alarmStartMillis=0;
#define ALARM_TIMEOUT_MS 60000
float prevGX=0, prevGY=0, prevGZ=0;

// ─── Adaptive DSP ────────────────────────────────────────────
#define SMA_WINDOW       10
#define CALIB_SAMPLES    10
#define CALIB_SIGMA_MULT 2.5f  // balanced between sensitive and stable
float smaBuffer[SMA_WINDOW]={0};
int   smaIdx=0;
float calibMean=1.5f, calibThreshold=1.5f;
bool  calibDone=false;
int   calibCount=0;

float smaUpdate(float v){
    smaBuffer[smaIdx%SMA_WINDOW]=v; smaIdx++;
    float s=0; for(int i=0;i<SMA_WINDOW;i++) s+=smaBuffer[i];
    return s/SMA_WINDOW;
}

int  to12h(int h){ return (h%12==0)?12:h%12; }
bool isAM(int h) { return h<12; }
int  to24h(int h,bool am){ if(am) return (h==12)?0:h; else return (h==12)?12:h+12; }

// ─── X27 Motor ───────────────────────────────────────────────
#define MOTOR_MAX              600
#define MOTOR_STEP              60
#define MOTOR_STEP_COOLDOWN_MS 500
SwitecX25 motor(MOTOR_MAX, M1, M2, M3, M4);
int motorProgress=0;
unsigned long lastMotorStepMs=0;

// ── Blocking return to physical 0 ────────────────────────────
void motorReset() {
    motorProgress = 0;

    // Show on OLED while returning
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(4, 18);
    display.print("Returning");
    display.setCursor(4, 38);
    display.print("to zero..");
    display.display();

    Serial.printf("[MOTOR] Returning from step %d to 0\n", motor.currentStep);

    // Force target = 0 and step until physically there
    // Use targetStep check too in case currentStep already reads 0
    motor.setPosition(0);
    unsigned long t = millis();
    while ((motor.currentStep != 0 || motor.targetStep != 0) && millis()-t < 10000UL) {
        motor.update();
    }

    Serial.println("[MOTOR] At zero");

    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(28, 24);
    display.print("Done!");
    display.display();
    delay(500);
}

// ─── ESP-NOW ─────────────────────────────────────────────────
void OnDataRecv(const uint8_t*, const uint8_t*, int);

void espnowStart(){
    if(espnowActive) return;
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    if(esp_now_init()!=ESP_OK){ Serial.println("[ESP-NOW] FAILED"); return; }
    esp_now_register_recv_cb(OnDataRecv);
    espnowActive=true; espnowConnected=false; lastDataMs=0; gX=gY=gZ=0;
    Serial.println("[ESP-NOW] Started");
}
void espnowStop(){
    if(!espnowActive) return;
    esp_now_deinit(); WiFi.mode(WIFI_OFF);
    espnowActive=false; espnowConnected=false;
    Serial.println("[ESP-NOW] Stopped");
}

// ─── LED ─────────────────────────────────────────────────────
void updateStatusLed(){
    if(appState==STATE_ALARMING) return;
    digitalWrite(LED_PIN, LOW);
}

// ─── Buzzer – Für Elise ──────────────────────────────────────
#define BUZZER_CH 0
void buzzerTone(int f){
    if(f<=0){ ledcDetachPin(BUZZER_PIN); digitalWrite(BUZZER_PIN,LOW); return; }
    ledcSetup(BUZZER_CH,f,8); ledcAttachPin(BUZZER_PIN,BUZZER_CH); ledcWrite(BUZZER_CH,128);
}
void buzzerOff(){ ledcDetachPin(BUZZER_PIN); digitalWrite(BUZZER_PIN,LOW); }

const int FE_NOTES[]={659,623,659,623,659,494,587,523,440,0,262,330,440,494,0,330,415,494,523,0,330,659,623,659,623,659,494,587,523,440,0};
const int FE_DURS[] ={120,120,120,120,120,120,120,120,240,120,120,120,120,240,120,120,120,120,240,120,120,120,120,120,120,120,120,120,120,120,240,360};
const int FE_LEN=sizeof(FE_NOTES)/sizeof(FE_NOTES[0]);
int feIdx=0; unsigned long lastNoteMs=0;

void buzzerUpdate(){
    if(appState!=STATE_ALARMING) return;
    if(millis()-lastNoteMs<(unsigned long)FE_DURS[feIdx]) return;
    feIdx=(feIdx+1)%FE_LEN; lastNoteMs=millis();
    buzzerTone(FE_NOTES[feIdx]);
}

// ─── ESP-NOW Receive ─────────────────────────────────────────
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len){
    if(len!=sizeof(struct_message)) return;
    memcpy(&incomingData,data,sizeof(incomingData));
    gX=incomingData.x; gY=incomingData.y; gZ=incomingData.z;
    lastDataMs=millis(); espnowConnected=true;
    Serial.printf("[RECV] X=%.2f Y=%.2f Z=%.2f\n",gX,gY,gZ);

    if(appState==STATE_ALARMING){
        float dX=fabsf(gX-prevGX);
        float dY=fabsf(gY-prevGY);
        float dZ=fabsf(gZ-prevGZ);
        float rawMag=dX+dY+dZ;
        float smoothMag=smaUpdate(rawMag);

        static float calibBuf[CALIB_SAMPLES];
        if(!calibDone){
            if(calibCount<CALIB_SAMPLES){
                calibBuf[calibCount++]=smoothMag;
                Serial.printf("[CALIB] %d/%d\n",calibCount,CALIB_SAMPLES);
            } else {
                float sum=0; for(int i=0;i<CALIB_SAMPLES;i++) sum+=calibBuf[i];
                calibMean=sum/CALIB_SAMPLES;
                float var=0;
                for(int i=0;i<CALIB_SAMPLES;i++){ float d=calibBuf[i]-calibMean; var+=d*d; }
                float sigma=sqrtf(var/CALIB_SAMPLES);
                calibThreshold=calibMean+CALIB_SIGMA_MULT*sigma;

                // Safety cap — threshold must be between 0.3g and 2.0g
                // Prevents bad calibration (moving during calib) locking out alarm
                if(calibThreshold < 0.6f) calibThreshold = 0.6f;  // min 0.6g prevents idle false positive
                if(calibThreshold > 2.0f) calibThreshold = 2.0f;

                calibDone=true;
                Serial.printf("[CALIB] Done! mean=%.3f sigma=%.3f thr=%.3f\n",calibMean,sigma,calibThreshold);
                display.clearDisplay();
                display.setTextColor(SSD1306_WHITE);
                display.setTextSize(2); display.setCursor(4,20); display.print("Calibrated!");
                display.setTextSize(1); display.setCursor(4,44); display.printf("thr=%.2fg",calibThreshold);
                display.display(); delay(1000);
            }
            prevGX=gX; prevGY=gY; prevGZ=gZ;
            return;
        }

        bool moving = smoothMag > calibThreshold;
        Serial.printf("[DSP] raw=%.3f smooth=%.3f thr=%.3f moving=%d\n",rawMag,smoothMag,calibThreshold,moving);

        if(moving && millis()-lastMotorStepMs>=MOTOR_STEP_COOLDOWN_MS){
            motorProgress+=MOTOR_STEP;
            if(motorProgress>MOTOR_MAX) motorProgress=MOTOR_MAX;
            motor.setPosition(motorProgress);
            lastMotorStepMs=millis();
            Serial.printf("[ALARM] Progress: %d/%d (%d%%)\n",motorProgress,MOTOR_MAX,map(motorProgress,0,MOTOR_MAX,0,100));
        }
    }
    prevGX=gX; prevGY=gY; prevGZ=gZ;
}

// ─── Clock Tick ──────────────────────────────────────────────
void clockTick(){
    unsigned long now=millis();
    if(now-lastSecMillis<1000) return;
    lastSecMillis+=1000;
    if(now-lastSecMillis>2000) lastSecMillis=now;
    if(++clkS>=60){clkS=0; if(++clkM>=60){clkM=0; if(++clkH>=24) clkH=0;}}
    if(espnowActive) espnowConnected=(millis()-lastDataMs<ESPNOW_TIMEOUT_MS);
    if(almEnabled && appState==STATE_SHOW && clkH==almH && clkM==almM && clkS==0){
        appState=STATE_ALARMING; alarmStartMillis=now;
        motorProgress=0; lastMotorStepMs=0;
        motor.setPosition(0);
        feIdx=0; lastNoteMs=0;
        prevGX=prevGY=prevGZ=0;
        calibDone=false; calibCount=0;
        espnowStart();
        Serial.println("[ALARM] Triggered");
    }
}

// ─── Buttons ─────────────────────────────────────────────────
volatile bool enterPressed=false, cancelPressed=false;
unsigned long lastEnterMs=0, lastCancelMs=0;
#define DEBOUNCE_MS 50
void IRAM_ATTR isrEnter() { if(millis()-lastEnterMs>DEBOUNCE_MS){ enterPressed=true; lastEnterMs=millis(); } }
void IRAM_ATTR isrCancel(){ if(millis()-lastCancelMs>DEBOUNCE_MS){ cancelPressed=true; lastCancelMs=millis(); } }

// ─── Pot ─────────────────────────────────────────────────────
static int potPrev=-1; static unsigned long lastPotMove=0;
#define POT_DEADBAND 80
#define POT_DELAY_MS 150
void readPot(){
    if(appState==STATE_SHOW||appState==STATE_ALARMING) return;
    int r=0; for(int i=0;i<4;i++) r+=analogRead(POT_PIN); r/=4;
    if(potPrev<0){potPrev=r; return;}
    int diff=r-potPrev;
    if(abs(diff)<POT_DEADBAND) return;
    if(millis()-lastPotMove<POT_DELAY_MS) return;
    int dir=(diff>0)?1:-1; potPrev=r; lastPotMove=millis();
    switch(appState){
        case STATE_MENU:     menuSel=constrain(menuSel+dir,0,1); break;
        case STATE_SET_H:    editH=constrain(editH+dir,1,12);    break;
        case STATE_SET_M:    editM=(editM+dir+60)%60;             break;
        case STATE_SET_AMPM: editAM=!editAM;                     break;
        default: break;
    }
}

// ─── State Transitions ───────────────────────────────────────
void handleInput(){
    if(!enterPressed&&!cancelPressed) return;
    switch(appState){
        case STATE_SHOW:
            if(enterPressed){ menuSel=0; appState=STATE_MENU; } break;
        case STATE_MENU:
            if(enterPressed){
                editingClock=(menuSel==0);
                int sH=editingClock?clkH:almH, sM=editingClock?clkM:almM;
                editH=to12h(sH); editM=sM; editAM=isAM(sH); appState=STATE_SET_H;
            }
            if(cancelPressed) appState=STATE_SHOW; break;
        case STATE_SET_H:
            if(enterPressed)  appState=STATE_SET_M;
            if(cancelPressed) appState=STATE_MENU; break;
        case STATE_SET_M:
            if(enterPressed)  appState=STATE_SET_AMPM;
            if(cancelPressed) appState=STATE_SET_H; break;
        case STATE_SET_AMPM:
            if(enterPressed){
                int h24=to24h(editH,editAM);
                if(editingClock){clkH=h24;clkM=editM;clkS=0;lastSecMillis=millis();}
                else {almH=h24;almM=editM;almEnabled=true;}
                appState=STATE_SHOW;
            }
            if(cancelPressed) appState=STATE_SET_M; break;
        case STATE_ALARMING:
            // Stop alarm immediately, then motor returns, then WiFi off
            buzzerOff();
            digitalWrite(LED_PIN,LOW);
            almEnabled=false;
            appState=STATE_SHOW;   // switch state first so OLED shows "Returning"
            motorReset();          // blocking — shows "Returning to zero.."
            espnowStop();
            Serial.println("[ALARM] Dismissed by button");
            break;
    }
    enterPressed=cancelPressed=false;
}

// ─── Alarm Effects ───────────────────────────────────────────
void alarmEffects(){
    if(appState!=STATE_ALARMING) return;
    if(millis()-alarmStartMillis>ALARM_TIMEOUT_MS){
        buzzerOff(); digitalWrite(LED_PIN,LOW); almEnabled=false;
        appState=STATE_SHOW;
        motorReset(); espnowStop();
        Serial.println("[ALARM] Timeout"); return;
    }
    if(motorProgress>=MOTOR_MAX && motor.currentStep==motor.targetStep){
        buzzerOff(); digitalWrite(LED_PIN,LOW); almEnabled=false;
        appState=STATE_SHOW;
        motorReset(); espnowStop();
        Serial.println("[ALARM] Done by motion"); return;
    }
    digitalWrite(LED_PIN,(millis()/300)%2?HIGH:LOW);
    buzzerUpdate();
}

// ─── Display ─────────────────────────────────────────────────
void drawStatusBadge(){
    display.setTextSize(1); display.setCursor(92,0);
    if(espnowConnected){ display.print("[NOW]"); }
    else {
        static uint8_t dots=0; static unsigned long ld=0;
        if(millis()-ld>400){dots=(dots+1)%4;ld=millis();}
        display.print("NOW"); for(int i=0;i<dots;i++) display.print(".");
    }
}
void drawShowTime(){
    display.setTextSize(2); display.setCursor(0,0);
    display.printf("%2d:%02d:%02d",to12h(clkH),clkM,clkS);
    display.setTextSize(1); display.setCursor(112,0); display.print(isAM(clkH)?"AM":"PM");
    display.setCursor(0,18); display.printf("A %2d:%02d%s %s",to12h(almH),almM,isAM(almH)?"AM":"PM",almEnabled?"ON ":"off");
    display.drawLine(0,27,127,27,SSD1306_WHITE);
    display.setCursor(0,54); display.print("[ ENTER = Setup ]");
}
void drawMenu(){
    display.setTextSize(1); display.setCursor(0,0); display.print("-- Setup --");
    display.drawLine(0,9,127,9,SSD1306_WHITE); display.setTextSize(2);
    if(menuSel==0){display.fillRoundRect(0,14,128,22,3,SSD1306_WHITE);display.setTextColor(SSD1306_BLACK);}
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(28,18); display.print("Clock"); display.setTextColor(SSD1306_WHITE);
    if(menuSel==1){display.fillRoundRect(0,38,128,22,3,SSD1306_WHITE);display.setTextColor(SSD1306_BLACK);}
    display.setCursor(28,42); display.print("Alarm"); display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0,56); display.print("Turn=Sel  ENT=OK  CAN=Back");
}
void drawSetH(){
    display.setTextSize(1); display.setCursor(0,0); display.print(editingClock?"Set Clock":"Set Alarm");
    display.drawLine(0,9,127,9,SSD1306_WHITE); display.setCursor(0,12); display.print("Hour  (1-12)");
    display.setTextSize(3); display.setCursor(4,22); display.printf("%2d:%02d",editH,editM);
    display.setTextSize(2); display.setCursor(98,26); display.print(editAM?"AM":"PM");
    display.drawLine(4,50,54,50,SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0,56); display.print("ENT=Next  CAN=Back");
}
void drawSetM(){
    display.setTextSize(1); display.setCursor(0,0); display.print(editingClock?"Set Clock":"Set Alarm");
    display.drawLine(0,9,127,9,SSD1306_WHITE); display.setCursor(0,12); display.print("Minute  (0-59)");
    display.setTextSize(3); display.setCursor(4,22); display.printf("%2d:%02d",editH,editM);
    display.setTextSize(2); display.setCursor(98,26); display.print(editAM?"AM":"PM");
    display.drawLine(58,50,110,50,SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0,56); display.print("ENT=Next  CAN=Back");
}
void drawSetAMPM(){
    display.setTextSize(1); display.setCursor(0,0); display.print(editingClock?"Set Clock":"Set Alarm");
    display.drawLine(0,9,127,9,SSD1306_WHITE);
    display.setTextSize(2); display.setCursor(14,13); display.printf("%2d:%02d %s",editH,editM,editAM?"AM":"PM");
    if(editAM){display.fillRoundRect(4,36,56,20,3,SSD1306_WHITE);display.setTextColor(SSD1306_BLACK);}
    display.setCursor(18,39); display.print("AM"); display.setTextColor(SSD1306_WHITE);
    if(!editAM){display.fillRoundRect(68,36,56,20,3,SSD1306_WHITE);display.setTextColor(SSD1306_BLACK);}
    display.setCursor(82,39); display.print("PM"); display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1); display.setCursor(0,57); display.print("Turn=AM/PM  ENT=Save  CAN=Back");
}
void drawAlarming(){
    bool blink=(millis()/400)%2;
    display.setTextSize(2); display.setCursor(4,0); if(blink) display.print("!! ALARM !!");
    drawStatusBadge();
    display.setTextSize(1); display.setCursor(0,20);
    if(!espnowConnected)       display.print("Waiting for sensor...");
    else if(!calibDone)        display.printf("Calibrating...%d%%", map(calibCount,0,CALIB_SAMPLES,0,100));
    else                       display.printf("X%+.1f Y%+.1f Z%+.1f",gX,gY,gZ);
    int pct=map(motorProgress,0,MOTOR_MAX,0,100);
    display.setCursor(0,30); if(espnowConnected) display.printf("Move! %3d%%",pct);
    display.drawRect(0,40,102,8,SSD1306_WHITE);
    if(pct>0) display.fillRect(0,40,pct,8,SSD1306_WHITE);
    display.setCursor(0,53); display.print(espnowConnected?"Any key = force stop":"Or press key to stop");
}
void updateDisplay(){
    if(millis()-lastDisplayMillis<100) return;
    lastDisplayMillis=millis();
    display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
    switch(appState){
        case STATE_SHOW:     drawShowTime(); break;
        case STATE_MENU:     drawMenu();     break;
        case STATE_SET_H:    drawSetH();     break;
        case STATE_SET_M:    drawSetM();     break;
        case STATE_SET_AMPM: drawSetAMPM();  break;
        case STATE_ALARMING: drawAlarming(); break;
    }
    display.display();
}

// ─── Setup ───────────────────────────────────────────────────
void setup(){
    Serial.begin(115200); delay(400);
    Serial.println("=== Alarm Clock + ESP-NOW ===");
    pinMode(BTN_ENTER,INPUT_PULLUP); pinMode(BTN_CANCEL,INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BTN_ENTER), isrEnter, FALLING);
    attachInterrupt(digitalPinToInterrupt(BTN_CANCEL),isrCancel,FALLING);
    pinMode(LED_PIN,OUTPUT); pinMode(BUZZER_PIN,OUTPUT);
    analogReadResolution(12);
    Wire.begin(6,7);
    if(!display.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)){ Serial.println("[ERROR] OLED"); while(1) delay(500); }
    // ── Boot Splash — animated alarm clock ──────────────────────────────────
    // Draw alarm clock face + shake animation + "Wake to Shake!" text
    for (int frame = 0; frame < 18; frame++) {
        display.clearDisplay();

        // Shake offset: alternates -2, 0, +2 pixels
        int shake = 0;
        if (frame < 12) {  // shake for first 12 frames
            int s = frame % 3;
            shake = (s == 0) ? -2 : (s == 1) ? 0 : 2;
        }

        int cx = 38 + shake;   // clock center X
        int cy = 30;           // clock center Y
        int cr = 22;           // clock radius

        // Clock body (outer circle)
        display.drawCircle(cx, cy, cr, SSD1306_WHITE);
        display.drawCircle(cx, cy, cr - 1, SSD1306_WHITE);

        // Bell bumps on top
        display.fillCircle(cx - 14 + shake, cy - 20, 4, SSD1306_WHITE);
        display.fillCircle(cx + 14 + shake, cy - 20, 4, SSD1306_WHITE);

        // Clock legs on bottom
        display.fillTriangle(cx - 12 + shake, cy + cr,
                             cx - 18 + shake, cy + cr + 7,
                             cx - 6  + shake, cy + cr + 7, SSD1306_WHITE);
        display.fillTriangle(cx + 12 + shake, cy + cr,
                             cx + 6  + shake, cy + cr + 7,
                             cx + 18 + shake, cy + cr + 7, SSD1306_WHITE);

        // Hour hand (pointing ~10 o'clock)
        display.drawLine(cx, cy, cx - 8 + shake, cy - 12, SSD1306_WHITE);
        // Minute hand (pointing ~12 o'clock, slightly right)
        display.drawLine(cx, cy, cx + 3 + shake, cy - 16, SSD1306_WHITE);

        // Center dot
        display.fillCircle(cx, cy, 2, SSD1306_WHITE);

        // Vibration lines when shaking
        if (frame < 12 && frame % 2 == 0) {
            display.drawLine(cx - cr - 5 + shake, cy - 5, cx - cr - 10 + shake, cy - 8, SSD1306_WHITE);
            display.drawLine(cx - cr - 5 + shake, cy + 3, cx - cr - 10 + shake, cy + 6, SSD1306_WHITE);
            display.drawLine(cx + cr + 5 + shake, cy - 5, cx + cr + 10 + shake, cy - 8, SSD1306_WHITE);
            display.drawLine(cx + cr + 5 + shake, cy + 3, cx + cr + 10 + shake, cy + 6, SSD1306_WHITE);
        }

        // "Wake to Shake!" text on right side
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(70, 18);
        display.print("Shake to");
        display.setTextSize(1);
        display.setCursor(70, 30);
        display.print("Wake!");

        // Dots animation under text
        display.setCursor(70, 44);
        for (int d = 0; d <= (frame % 4); d++) display.print(".");

        display.display();
        delay(120);
    }
    display.clearDisplay();
    display.display();

    Serial.println("[MOTOR] Zeroing...");
    motor.zero();
    Serial.println("[MOTOR] Ready");
    Serial.println("[ESP-NOW] Standby until alarm");
    lastSecMillis=millis();
    for(int i=0;i<3;i++){ digitalWrite(LED_PIN,HIGH);delay(80);digitalWrite(LED_PIN,LOW);delay(80); }
}

// ─── Loop ────────────────────────────────────────────────────
void loop(){
    motor.update();
    clockTick();
    handleInput();
    alarmEffects();
    updateStatusLed();
    updateDisplay();
    readPot();
}
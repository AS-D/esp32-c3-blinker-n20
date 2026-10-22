/**
 * ================================================================
 * ESP32-C3 SuperMini + GM12-N20 电机 远程控制
 * 配网方式：Blinker 官方 APCONFIG（BLINKER_APCONFIG）
 * 基于 Blinker 点灯科技 / PlatformIO
 * ================================================================
 *
 * 【配网流程（改用 Blinker 自带 AP 配网）】
 *  1. 把下方 auth[] 换成你自己在点灯科技 APP 里的 12 位设备密钥（只需烧录一次）
 *  2. 首次上电（或长按 BOOT 清除配置后）设备自动开启配网热点
 *     热点名：DiyArduino_XXXXXX（由 MAC 生成，开放网络，无密码）
 *  3. 手机打开 Blinker APP → 添加设备 → 选择设备类型 →
 *     网络接入方式选择「AP 配网」，手机连上上面那个热点，
 *     APP 会自动把 WiFi 名称/密码下发给设备
 *  4. 配网信息保存在设备 EEPROM 中，之后换网也走同样的 APP 配网流程
 *  5. 长按 BOOT 键 3 秒 = 清除 WiFi 配置（Blinker.reset()）并重启重新配网
 *
 * 【板载 LED（GPIO8）】
 *   本板 ESP32-C3 SuperMini 的板载 LED 是【单色蓝色 LED】（2 脚），
 *   只能用亮度/闪烁节奏表达状态，无法显示颜色：
 *     呼吸        = 配网模式 / 未连上 WiFi（热点已开启，等待 APP 配网）
 *     慢闪        = WiFi 已连上，但 Blinker 未上线（检查密钥或网络）
 *     常亮（暗）  = 已连接 WiFi 并连上 Blinker，正常工作
 *     快闪        = 长按 BOOT 触发清除配网配置
 *
 * 【安全】
 *   - 上电默认停电机
 *   - Blinker 掉线超过 3 秒自动停电机
 * ================================================================
 */
#define BLINKER_WIFI
#define BLINKER_APCONFIG

#include <Arduino.h>
#include <Blinker.h>

// ============ 引脚定义 ============
#define MOTOR_PIN       6       // MOS 管 IN 脚
#define BOOT_BTN_PIN    9       // SuperMini BOOT 按键（GPIO9）
#define LED_PIN         8       // SuperMini 板载单色蓝灯（GPIO8）

// ============ Blinker 设备密钥（必填）============
// 点灯科技 APP -> 设备 -> 设备密钥，12 位
char auth[] = "2e0886e80aff";

// Blinker 按键组件
BlinkerButton MotorBtn("btn_motor");

// 当前是否处于 AP（配网热点）模式
bool apEnabled() {
  wifi_mode_t m = WiFi.getMode();
  return m == WIFI_MODE_AP || m == WIFI_MODE_APSTA;
}

// ============ LED 控制（板载单色蓝灯）============
// SuperMini 板载 LED 接 GPIO8，是 2 脚单色蓝灯，只能用 LEDC(PWM) 调亮度/闪烁，
// 无法显示颜色，用不同节奏/亮度表达状态。
#define LED_PWM_CH     0
#define LED_PWM_FREQ   5000
#define LED_PWM_RES    8

// 多数 SuperMini 是「GPIO 高电平点亮」；若发现亮灭相反，改成 1
#define LED_ACTIVE_LOW 0

void ledInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LED_PIN, LED_PWM_FREQ, LED_PWM_RES);     // Arduino core 3.x
#else
  ledcSetup(LED_PWM_CH, LED_PWM_FREQ, LED_PWM_RES);    // Arduino core 2.x
  ledcAttachPin(LED_PIN, LED_PWM_CH);
#endif
}

// level: 0(灭) ~ 255(最亮)
void setLED(uint8_t level) {
#if LED_ACTIVE_LOW
  level = 255 - level;
#endif
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LED_PIN, level);
#else
  ledcWrite(LED_PWM_CH, level);
#endif
}

// 根据当前状态刷新板载 LED，非阻塞
void updateLED() {
  static uint32_t lastTick = 0;
  static int16_t  breath   = 0;
  static int16_t  step     = 3;

  uint32_t now = millis();
  if (now - lastTick < 25) return;
  lastTick = now;

  if (WiFi.status() != WL_CONNECTED) {
    // 配网 / 未连上 WiFi：呼吸
    breath += step;
    if (breath <= 0)       { breath = 0;  step = 3;  }
    else if (breath >= 90) { breath = 90; step = -3; }
    setLED((uint8_t)breath);
  }
  else if (!Blinker.connected()) {
    // WiFi 已连、Blinker 未连：慢闪
    bool on = (now / 400) % 2 == 0;
    setLED(on ? 80 : 0);
  }
  else {
    // 全部在线：常亮（亮度压低，减少发热与干扰，与呼吸/闪烁可区分）
    setLED(35);
  }
}

// ============ 电机控制 ============
void motorStop()  { digitalWrite(MOTOR_PIN, LOW);  }
void motorStart() { digitalWrite(MOTOR_PIN, HIGH); }

void motorCallback(const String & state) {
  // Blinker 长按模式：按下 = press，松开 = pressup；点按 = tap
  if (state == "press") {
    motorStart();
    MotorBtn.print("on");
    BLINKER_LOG("电机启动");
  } else if (state == "pressup" || state == "tap") {
    motorStop();
    MotorBtn.print("off");
    BLINKER_LOG("电机停止");
  }
}

// ============ 长按 BOOT 清除 WiFi 配置（非阻塞）============
void handleResetButton() {
  static bool     pressed    = false;
  static bool     fired      = false;
  static uint32_t pressStart = 0;

  bool down = (digitalRead(BOOT_BTN_PIN) == LOW);

  if (!down) {
    pressed = false;
    fired   = false;
    return;
  }

  if (!pressed) {
    pressed = true;
    pressStart = millis();
    return;
  }

  if (!fired && millis() - pressStart > 3000) {
    fired = true;
    BLINKER_LOG("长按 BOOT：清除 WiFi 配置并重启");
    motorStop();
    for (int i = 0; i < 5; i++) {
      setLED(60); delay(90);
      setLED(0);  delay(90);
    }
    Blinker.reset();   // 擦除配网标记后内部直接重启
  }
}

// ============ 诊断：打印当前网络状态 ============
void printStatus() {
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint < 5000) return;
  lastPrint = millis();

  Serial.printf("[STA] millis=%lu wifiMode=%d staStatus=%d blinker=%s ",
                (unsigned long)millis(), WiFi.getMode(), WiFi.status(),
                Blinker.connected() ? "在线" : "离线");

  if (apEnabled()) {
    Serial.printf("| AP: ssid=%s ip=%s 已连接终端=%d",
                  WiFi.softAPSSID().c_str(),
                  WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPgetStationNum());
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("| WiFi: %s ip=%s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  }
  Serial.println();
}

// ============ 自愈：脏配网数据时不至于“既连不上又不开热点”============
// Blinker 的 autoInit() 只要 EEPROM 里有 "OK" 标记就直接返回 true（不管 WiFi 到底能不能连上），
// 于是软永远不会有 AP 热点。下面这段用于把这种死局自动救回来。
void checkStuck() {
  static bool handled = false;
  if (handled) return;

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool apOn   = apEnabled();

  if (wifiOk || apOn) {
    handled = true;              // 正常情形，不再干预
    return;
  }

  if (millis() > 45000) {
    handled = true;
    Serial.println("[AUTO] 45s 内既没连上 WiFi 也没有配网热点，判定为脏配网数据，自动清除并重启");
    motorStop();
    Blinker.reset();            // 清掉电梯内的配网标记并重启，重启后进入 AP 配网
  }
}

// ============ setup ============
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n====================================");
  Serial.println(" ESP32-C3 SuperMini 电机远程控制");
  Serial.println(" 配网方式: Blinker APCONFIG");
  Serial.println("====================================");

  pinMode(MOTOR_PIN, OUTPUT);
  motorStop();

  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  ledInit();            // 初始化板载 LED 的 PWM 通道
  setLED(0);            // 上电先灭灯

  if (strlen(auth) != 12) {
    Serial.printf("[WARN] auth 长度 %d，Blinker 密钥应为 12 位，请先填写正确的 key\n", strlen(auth));
  }

  // 打开库自身的日志（否则 BLINKER_LOG 全部静默，看不到 "Wait for APConfig" 等关键信息）
  BLINKER_DEBUG.stream(Serial);

  // 一开机就看 EEPROM 里是否残留配网信息（决定是否会有热点）
  char wlanOk[BLINKER_WLAN_CHECK_SIZE + 1] = {0};
  EEPROM.begin(BLINKER_EEP_SIZE);
  EEPROM.get(BLINKER_EEP_ADDR_WLAN_CHECK, wlanOk);
  EEPROM.end();
  Serial.printf("[CFG] EEPROM 配网标记 = \"%s\" -> %s\n",
                wlanOk, String(wlanOk) == String("OK") ? "已有旧配置，会直接尝试连 WiFi（不会开热点）"
                                                       : "无有效配置，将开启 AP 配网热点");

  // Blinker 内部处理：
  //  - EEPROM 里有配网信息 -> 直接连 WiFi 并上线
  //  - 没有配网信息        -> 开启 AP 热点 DiyArduino_xxxx，等待 APP 配网
  Blinker.begin(auth);

  MotorBtn.attach(motorCallback);

  if (apEnabled()) {
    Serial.printf("[AP] 配网热点已开启: %s  管理地址=http://%s\n",
                  WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("[WiFi] 已有配网信息，正在连接 WiFi（长按 BOOT 3 秒可清除）");
  }
}

// ============ loop ============
void loop() {
  // Blinker 主循环：AI 配网状态机、按键回调、心跳、OTA 都在这里
  Blinker.run();

  updateLED();
  handleResetButton();
  printStatus();
  checkStuck();

  // 安全保护：Blinker 掉线超过 3 秒自动停电机
  static uint32_t lastOnline = 0;
  if (Blinker.connected()) {
    lastOnline = millis();
  } else if (lastOnline != 0 && millis() - lastOnline > 3000) {
    motorStop();
  } else if (lastOnline == 0) {
    // 启动后从未连上过，保持电机停止
    motorStop();
  }
}

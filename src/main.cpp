/**
 * ================================================================
 * ESP32-C3 SuperMini + GM12-N20 电机 远程控制
 * 配网方式：Blinker 官方 APCONFIG（BLINKER_APCONFIG）
 *          + 自建 NVS(Preferences) 持久化，解决「配完网断电就丢」
 * 基于 Blinker 点灯科技 / PlatformIO
 * ================================================================
 *
 * 【为什么自己做一层持久化】
 *  Blinker 库把配网信息存在 EEPROM。Arduino-ESP32 3.x 的 EEPROM 改成 NVS blob 实现后，
 *  EEPROM.commit() 内部只调了 nvs_set_blob()，没有调 nvs_commit()；
 *  而 IDF 明确要求 nvs_commit() 之后数据才真正写进 flash。
 *  结果：配完网当时能连（数据在 RAM 里），一断电就没了 -> 开机读不到标记 -> 又开热点。
 *  Preferences 的 put/remove 内部都有 nvs_commit()，是可靠的，所以这里自己存一份。
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
 *     呼吸        = 配网模式（热点已开启，等待 APP 配网）
 *     慢闪(1s)    = 有存档配置、正在自动重连 WiFi
 *     快闪        = WiFi 已连上，但 Blinker 未上线（检查密钥或网络）
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
#include <EEPROM.h>
#include <Preferences.h>
#include <nvs.h>
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

// ============ WiFi 凭据持久化（自建 NVS，真正落盘）============
// Preferences 的 put*/remove 内部都会 nvs_commit()，断电不丢；
// Blinker 自带的 EEPROM 在 Arduino-ESP32 3.x 上不可靠（见文件头说明）。
#define PREFS_NS         "n20cfg"
#define PREFS_KEY_SSID   "ssid"
#define PREFS_KEY_PSWD   "pswd"
#define PREFS_KEY_FIXCNT "eefix"   // EEPROM 同步重试计数，防止无限重启

static Preferences prefs;
static bool   g_hasCfg    = false;   // NVS 里是否有存档
static String g_savedSsid;
static String g_savedPswd;

static bool cfgLoad(String & ssid, String & pswd) {
  prefs.begin(PREFS_NS, true);
  ssid = prefs.getString(PREFS_KEY_SSID, "");
  pswd = prefs.getString(PREFS_KEY_PSWD, "");
  prefs.end();
  return ssid.length() > 0;
}

static void cfgSave(const String & ssid, const String & pswd) {
  prefs.begin(PREFS_NS, false);
  prefs.putString(PREFS_KEY_SSID, ssid);
  prefs.putString(PREFS_KEY_PSWD, pswd);
  prefs.end();
}

static void cfgClear() {
  prefs.begin(PREFS_NS, false);
  prefs.remove(PREFS_KEY_SSID);
  prefs.remove(PREFS_KEY_PSWD);
  prefs.remove(PREFS_KEY_FIXCNT);
  prefs.end();
}

static uint8_t cfgGetFixCnt() {
  prefs.begin(PREFS_NS, true);
  uint8_t n = prefs.getUChar(PREFS_KEY_FIXCNT, 0);
  prefs.end();
  return n;
}

static void cfgSetFixCnt(uint8_t n) {
  prefs.begin(PREFS_NS, false);
  prefs.putUChar(PREFS_KEY_FIXCNT, n);
  prefs.end();
}

// Arduino-ESP32 3.x 的 EEPROM.commit() 缺少 nvs_commit()，这里手动补一次。
// EEPROM 库使用的 NVS namespace 就是 "eeprom"。
static void eepromFlush() {
  nvs_handle_t h;
  if (nvs_open("eeprom", NVS_READWRITE, &h) == ESP_OK) {
    nvs_commit(h);
    nvs_close(h);
  }
}

// 把凭据写进 Blinker 用的 EEPROM 区域（Blinker.begin() 会读它来决定连网还是开热点）
// 注意：用 snprintf 保证 '\0' 结尾，Blinker 原版直接 memcpy 32 字节，密码满 32 位会越界
static void blinkerEEPROMSave(const char * ssid, const char * pswd) {
  char s[BLINKER_SSID_SIZE] = {0};
  char p[BLINKER_PSWD_SIZE] = {0};
  char okmark[BLINKER_WLAN_CHECK_SIZE] = {0};

  snprintf(s, sizeof(s), "%s", ssid);
  snprintf(p, sizeof(p), "%s", pswd);
  okmark[0] = 'O';
  okmark[1] = 'K';

  EEPROM.begin(BLINKER_EEP_SIZE);
  EEPROM.put(BLINKER_EEP_ADDR_SSID, s);
  EEPROM.put(BLINKER_EEP_ADDR_PSWD, p);
  EEPROM.put(BLINKER_EEP_ADDR_WLAN_CHECK, okmark);
  EEPROM.commit();
  eepromFlush();          // 关键：补上 nvs_commit，否则掉电丢失
  EEPROM.end();
}

// 校验 Blinker EEPROM 里的 SSID + "OK" 标记是否已经是我们要的
static bool blinkerEEPROMHas(const char * ssid) {
  char s[BLINKER_SSID_SIZE + 1] = {0};
  char mark[BLINKER_WLAN_CHECK_SIZE + 1] = {0};

  EEPROM.begin(BLINKER_EEP_SIZE);
  EEPROM.get(BLINKER_EEP_ADDR_SSID, s);
  EEPROM.get(BLINKER_EEP_ADDR_WLAN_CHECK, mark);
  EEPROM.commit();
  EEPROM.end();

  s[BLINKER_SSID_SIZE] = 0;
  mark[BLINKER_WLAN_CHECK_SIZE] = 0;
  return String(s) == String(ssid) && String(mark) == String("OK");
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
    if (apEnabled()) {
      // 配网热点已开、等待 APP 配网：呼吸
      breath += step;
      if (breath <= 0)       { breath = 0;  step = 3;  }
      else if (breath >= 90) { breath = 90; step = -3; }
      setLED((uint8_t)breath);
    } else {
      // 已有存档但还没连上（正在自动重连）：慢闪 1s，和「等待配网」区分开
      bool on = (now / 1000) % 2 == 0;
      setLED(on ? 80 : 0);
    }
  }
  else if (!Blinker.connected()) {
    // WiFi 已连、Blinker 未连：快闪
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
    cfgClear();        // 清掉自建 NVS 存档
    Blinker.reset();   // 擦除 Blinker 配网标记并重启（不会返回）
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
  Serial.printf("| NVS存档: %s\n", g_hasCfg ? g_savedSsid.c_str() : "无");
}

// ============ 抓凭据：WiFi 一连通就把 SSID/密码落盘 ============
// 这一步是关键：Blinker 的 AP 配网成功后，凭据只在 RAM 里，
// 靠它自己写 EEPROM 在 core 3.x 上不落盘，所以我们连上即刻存到 NVS。
void captureWiFiConfig() {
  static uint32_t lastTry = 0;
  if (millis() - lastTry < 3000) return;
  lastTry = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  String cur = WiFi.SSID();
  if (!cur.length()) return;
  if (g_hasCfg && cur == g_savedSsid) return;   // 已经是同一个，不重复写 flash

  String psk = WiFi.psk();
  Serial.printf("[CFG] WiFi 已连通 -> 写入 NVS + Blinker EEPROM: %s\n", cur.c_str());

  cfgSave(cur, psk);                            // NVS：真正持久化
  if (!blinkerEEPROMHas(cur.c_str())) {         // 顺带给 Blinker 补一份，避免重开热点
    blinkerEEPROMSave(cur.c_str(), psk.c_str());
  }
  g_hasCfg   = true;
  g_savedSsid = cur;
  g_savedPswd = psk;
  cfgSetFixCnt(0);                              // 连上了，重试计数清零
}

// ============ 断线自愈：自己重连已配好的网络 ============
// Blinker 连上之后（AUTO_DONE）内部就再没有重连 WiFi 的逻辑了，
// 路由器重启/信号抖动导致掉线后它会一直卡着，所以这里自己重试。
void wifiKeepAlive() {
  static uint32_t lastRetry = 0;
  static uint32_t gap       = 15000;

  if (WiFi.status() == WL_CONNECTED) {
    lastRetry = millis();
    gap       = 15000;
    return;
  }
  if (!g_hasCfg || apEnabled()) return;   // 没存档 / 正在配网，都不打扰

  if (lastRetry == 0) { lastRetry = millis(); return; }   // 给首次连接留时间
  if (millis() - lastRetry < gap) return;

  lastRetry = millis();
  gap = (gap < 60000) ? gap * 2 : 60000;
  Serial.printf("[WiFi] 已断开，重新连接 %s（下次间隔 %lus）\n",
                g_savedSsid.c_str(), (unsigned long)(gap / 1000));
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_savedSsid.c_str(), g_savedPswd.c_str());
}

// ============ 自愈：没有任何配置却又连不上 WiFi ============
// 只在「确实没配置」时才动手清除；只要有存档就绝不自毁，交给 wifiKeepAlive 慢慢重试。
void checkStuck() {
  static bool handled = false;
  if (handled) return;

  if (WiFi.status() == WL_CONNECTED || apEnabled() || g_hasCfg) {
    handled = true;              // 正常情形，不再干预
    return;
  }

  if (millis() > 90000) {
    handled = true;
    Serial.println("[AUTO] 90s 内既没连上 WiFi、没有配网热点、也没有任何存档 -> 自动重启进配网");
    motorStop();
    Blinker.reset();            // 清掉 Blinker 配网标记并重启，重启后进入 AP 配网
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

  // ============ 开机先取回自己存的 WiFi 凭据，并同步给 Blinker ============
  String _ssid, _pswd;
  if (cfgLoad(_ssid, _pswd)) {
    g_hasCfg     = true;
    g_savedSsid  = _ssid;
    g_savedPswd  = _pswd;
    Serial.printf("[CFG] NVS 存档 WiFi = %s\n", _ssid.c_str());

    // 把凭据写进 Blinker 的 EEPROM，这样它 autoInit() 能读到 "OK"，就不会去开热点。
    // （Blinker 自己那次写入没走 nvs_commit，断电后是读不到的）
    if (!blinkerEEPROMHas(_ssid.c_str())) {
      Serial.println("[CFG] Blinker EEPROM 标记缺失/不一致，重新写入并补 nvs_commit");
      blinkerEEPROMSave(_ssid.c_str(), _pswd.c_str());
    }
  } else {
    Serial.println("[CFG] NVS 中没有 WiFi 存档");
  }

  // 打印 Blinker 实际会读到的 EEPROM 标记
  char wlanOk[BLINKER_WLAN_CHECK_SIZE + 1] = {0};
  EEPROM.begin(BLINKER_EEP_SIZE);
  EEPROM.get(BLINKER_EEP_ADDR_WLAN_CHECK, wlanOk);
  EEPROM.commit();
  EEPROM.end();
  Serial.printf("[CFG] EEPROM 配网标记 = \"%s\" -> %s\n",
                wlanOk, String(wlanOk) == String("OK") ? "Blinker 会直接连 WiFi（不会开热点）"
                                                       : "Blinker 会开启 AP 配网热点");

  // Blinker 内部处理：
  //  - EEPROM 里有配网信息 -> 直接连 WiFi 并上线
  //  - 没有配网信息        -> 开启 AP 热点 DiyArduino_xxxx，等待 APP 配网
  Blinker.begin(auth);

  MotorBtn.attach(motorCallback);

  if (apEnabled()) {
    // 明明有存档却还是开了热点 = Blinker 没读到 EEPROM（写入未落盘）
    if (g_hasCfg) {
      uint8_t fix = cfgGetFixCnt();
      if (fix < 3) {
        cfgSetFixCnt(fix + 1);
        Serial.printf("[CFG] 有存档但 Blinker 仍开了热点（第 %d 次），重启重试\n", fix + 1);
        delay(300);
        ESP.restart();
      }
      // 多次重试仍不行：清掉自己的存档，老老实实停在配网模式
      Serial.println("[CFG] EEPROM 多次写入仍不可读，保持 AP 配网模式，请重新配一次");
      cfgSetFixCnt(0);
      cfgClear();
      g_hasCfg = false;
    }
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
  captureWiFiConfig();   // WiFi 一连上就把凭据写入 NVS（断电不丢的关键）
  wifiKeepAlive();       // 掉线后自己重连
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

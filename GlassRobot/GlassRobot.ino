/* ============================================================================
 *  跨障碍擦玻璃机器人 · ESP32-S3 手机遥控主程序
 * ----------------------------------------------------------------------------
 *  功能：
 *    · 双直流电机差速行走（前进/后退/原地转向/单轮驱动画弧）
 *    · 电动推杆正反转（伸出/收回，带换向死区 & 超时保护）
 *    · 舵机角度控制（0~180°）
 *    · 真空泵 / 2 路继电器 开关控制
 *    · 手机网页遥控：ESP32 自建热点，连上自动弹出控制页
 *
 *  烧录：Arduino IDE -> 开发板选 "ESP32S3 Dev Module"
 *        PSRAM 按你的模组选（N16R8 选 OPI PSRAM，N8R2 选 QSPI PSRAM）
 *        USB CDC On Boot 选 Enabled（这样串口才能打印日志）
 *        依赖库：无（全部用 ESP32 官方 core 自带的库）
 *
 *  驱动：两颗 L298N，纯数字直驱，EN 跳线帽短接不使用 EN 脚
 *        IN 脚只给高低电平，电机只有 正转/反转/停 三种状态，没有速度调节
 *        模块1 带两个行走电机（A路左轮 / B路右轮）
 *        模块2 带电动推杆（A路），B路空着可另作他用
 *        全板只有舵机用硬件 PWM（50Hz，GPIO10）
 *
 *  使用：① 手机 WiFi 连热点 "GlassBot-XXXX"（密码 12345678）
 *        ② 手机会自动弹出控制页；没弹出就在浏览器输入 http://192.168.4.1
 * ========================================================================== */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

#include "config.h"
#include "drive.h"
#include "web_page.h"

// ---------------------------------------------------------------- 全局对象
WebServer server(80);
DNSServer dnsServer;

DriveMotor motorL;
DriveMotor motorR;
Actuator   actuator;
ServoOut   servo;

struct RuntimeState {
  bool     pump     = false;
  bool     relay1   = false;
  bool     relay2   = false;
  int      lDir     = 0;         // 左轮 1=正转 -1=反转 0=停
  int      rDir     = 0;         // 右轮 1=正转 -1=反转 0=停

  bool     moveActive = false;
  uint32_t lastMove   = 0;

  bool     actActive  = false;
  bool     actBlocked = false;   // 超时/急停后锁定，必须松手(v=0)才能再动
  uint32_t lastAct    = 0;
  uint32_t actStart   = 0;

  String   apSsid;
} g;

// ---------------------------------------------------------------- 工具函数
static void setWheels(int lDir, int rDir);   // 前置声明

static String macSuffix() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

/** 差速驱动核心：直接给左右轮状态（1=正转 -1=反转 0=停） */
static void setWheels(int lDir, int rDir) {
  lDir = (lDir > 0) ? 1 : (lDir < 0 ? -1 : 0);
  rDir = (rDir > 0) ? 1 : (rDir < 0 ? -1 : 0);

  motorL.set(lDir);
  motorR.set(rDir);

  g.lDir = lDir;
  g.rDir = rDir;

  if (lDir == 0 && rDir == 0) {
    g.moveActive = false;        // 停车就关掉看门狗
  } else {
    g.moveActive = true;         // 走起来就必须持续刷新
    g.lastMove   = millis();
  }
}

static void stopDrive() {
  setWheels(0, 0);
}

/** 方向码 -> 左右轮（串口调试与兼容旧接口用） */
static void applyMove(const String& d) {
  if      (d == "f") setWheels( 1,  1);   // 直行
  else if (d == "b") setWheels(-1, -1);   // 倒退
  else if (d == "l") setWheels(-1,  1);   // 原地左转（两轮反向）
  else if (d == "r") setWheels( 1, -1);   // 原地右转（两轮反向）
  else stopDrive();
}

static void stopActuator() {
  actuator.set(0);
  g.actActive = false;
}

/** 保护性停止：锁定推杆，必须等客户端松手(v=0)才允许再次动作 */
static void lockActuator() {
  actuator.set(0);
  g.actActive  = false;
  g.actBlocked = true;
}

// ---------------------------------------------------------------- 硬件初始化
static void hardwareInit() {
  // ① 先锁死所有开关量输出，避免上电期间引脚悬空导致继电器误吸合
  outBegin(PIN_PUMP,   PUMP_ACTIVE_HIGH);
  outBegin(PIN_RELAY1, RELAY_ACTIVE_HIGH);
  outBegin(PIN_RELAY2, RELAY_ACTIVE_HIGH);

  // ② 两颗 L298N 的 EN 跳线帽保持短接（通道常使能），IN 脚纯数字直驱
  motorL.begin(PIN_M_LEFT_IN1,  PIN_M_LEFT_IN2);
  motorR.begin(PIN_M_RIGHT_IN1, PIN_M_RIGHT_IN2);
  stopDrive();

  actuator.begin(PIN_ACT_IN1, PIN_ACT_IN2);

  // ③ 舵机（全板唯一的 PWM）：开机只停在这个初始角度，不做扫描动作
  if (!servo.begin(PIN_SERVO)) {
    Serial.println("[警告] 舵机不可用，其余功能不受影响");
  }
#if SERVO_BOOT_TEST
  else { servo.sweepTest(); }        // 只有手动把该宏设为 1 才会自检
#endif

  Serial.printf("[输出] 真空泵 高电平触发 | 继电器1/2 %s电平触发\n",
                RELAY_ACTIVE_HIGH ? "高" : "低");
}

// ---------------------------------------------------------------- WiFi 热点
static void wifiInit() {
  g.apSsid = String(AP_SSID_PREFIX) + macSuffix();

  WiFi.persistent(false);          // 不把 WiFi 配置写进 Flash，省擦写寿命
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);            // 关掉省电，遥控响应更快、延迟更稳

  IPAddress ip(AP_IP);
  IPAddress gw(AP_IP);
  IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, mask);

  bool ok;
  if (strlen(AP_PASSWORD) >= 8) {
    ok = WiFi.softAP(g.apSsid.c_str(), AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  } else {
    ok = WiFi.softAP(g.apSsid.c_str(), NULL, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  }

  Serial.println();
  Serial.println("------------------------------------");
  Serial.printf("[热点] 名称: %s\n", g.apSsid.c_str());
  Serial.printf("[热点] 密码: %s\n", strlen(AP_PASSWORD) ? AP_PASSWORD : "(无密码)");
  Serial.printf("[热点] 状态: %s  信道: %d\n", ok ? "已启动" : "启动失败", AP_CHANNEL);
  Serial.printf("[热点] 手机浏览器打开: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("------------------------------------");

#if CAPTIVE_PORTAL
  // 把所有域名都解析到本机，这样手机一探测网络就会弹出控制页
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("[门户] 强制门户已开启，手机连上会自动弹出控制页");
#endif

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
  }
}

// ---------------------------------------------------------------- HTTP 接口
static void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleMove() {
  String d = server.arg("d");
  d.toLowerCase();
  if (d.length() == 0 || d == "x" || d == "stop") stopDrive();
  else applyMove(d);
  server.send(200, "text/plain", "ok");
}

/** 网页摇杆专用：收左右轮状态，差速判定在手机端算好 */
static void handleDrive() {
  if (server.hasArg("l") || server.hasArg("r")) {
    setWheels(server.arg("l").toInt(), server.arg("r").toInt());
  } else {
    stopDrive();
  }
  server.send(200, "text/plain", "ok");
}

static void handleAct() {
  int v = server.arg("v").toInt();
  v = constrain(v, -1, 1);

  if (v == 0) {
    g.actBlocked = false;      // 松手即解锁
    stopActuator();
  } else if (g.actBlocked) {
    // 被锁定时忽略动作请求，等客户端先发一次 v=0
  } else {
    if (actuator.value() != v) g.actStart = millis();
    actuator.set(v);
    g.actActive = true;
    g.lastAct   = millis();
  }
  server.send(200, "text/plain", "ok");
}

static void handleServo() {
  int a = server.arg("a").toInt();
  servo.setTarget(a);            // 只设目标；舵机任务会 1 度 1 度慢慢走过去
  server.send(200, "text/plain", "ok");
}

static void handlePump() {
  g.pump = server.arg("on").toInt() != 0;
  writeActive(PIN_PUMP, g.pump, PUMP_ACTIVE_HIGH);
  server.send(200, "text/plain", "ok");
}

static void handleRelay() {
  int  id = server.arg("id").toInt();
  bool on = server.arg("on").toInt() != 0;
  if (id == 1) {
    g.relay1 = on;
    writeActive(PIN_RELAY1, on, RELAY_ACTIVE_HIGH);
  } else if (id == 2) {
    g.relay2 = on;
    writeActive(PIN_RELAY2, on, RELAY_ACTIVE_HIGH);
  }
  server.send(200, "text/plain", "ok");
}

/** 急停：只停运动和推杆，真空泵保持吸附（玻璃上别掉下来） */
static void handleEstop() {
  stopDrive();
  lockActuator();
  servo.halt();                  // 舵机就地停住，别再继续转
  Serial.println("[急停] 行走、推杆、舵机已停止");
  server.send(200, "text/plain", "ok");
}

static void handleStatus() {
  String j;
  j.reserve(360);
  j += "{";
  j += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  j += "\"ssid\":\"" + g.apSsid + "\",";
  j += "\"cli\":" + String(WiFi.softAPgetStationNum()) + ",";
  j += "\"up\":" + String(millis() / 1000) + ",";
  j += "\"l\":" + String(g.lDir) + ",";
  j += "\"r\":" + String(g.rDir) + ",";
  j += "\"servo\":" + String(servo.angle()) + ",";
  j += "\"svT\":" + String(servo.target()) + ",";
  j += "\"svMove\":" + String(servo.moving() ? 1 : 0) + ",";
  j += "\"act\":" + String(actuator.value()) + ",";
  j += "\"blk\":" + String(g.actBlocked ? 1 : 0) + ",";
  j += "\"pump\":" + String(g.pump ? 1 : 0) + ",";
  j += "\"r1\":" + String(g.relay1 ? 1 : 0) + ",";
  j += "\"r2\":" + String(g.relay2 ? 1 : 0) + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap());
  j += "}";
  server.send(200, "application/json", j);
}

static void handleReboot() {
  stopDrive();
  stopActuator();
  server.send(200, "text/plain", "rebooting");
  delay(300);
  ESP.restart();
}

static void handleFavicon() { server.send(204, "text/plain", ""); }

/** 强制门户探测：手机一联网就查这些地址，全部跳转到控制页 */
static void handlePortal() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// ---------------------------------------------------------------- Web 服务
static void webInit() {
  server.on("/",               handleRoot);
  server.on("/api/drive",      handleDrive);
  server.on("/api/move",       handleMove);
  server.on("/api/act",        handleAct);
  server.on("/api/servo",      handleServo);
  server.on("/api/pump",       handlePump);
  server.on("/api/relay",      handleRelay);
  server.on("/api/estop",      handleEstop);
  server.on("/api/status",     handleStatus);
  server.on("/api/reboot",     handleReboot);
  server.on("/favicon.ico",    handleFavicon);

#if CAPTIVE_PORTAL
  // 安卓 / iOS / Windows 的联网探测地址
  server.on("/generate_204",                     handlePortal);  // 安卓
  server.on("/gen_204",                          handlePortal);  // 安卓
  server.on("/hotspot-detect.html",              handlePortal);  // iOS
  server.on("/library/test/success.html",        handlePortal);  // iOS 旧版
  server.on("/connecttest.txt",                  handlePortal);  // Windows
  server.on("/redirect",                         handlePortal);  // Windows
  server.on("/ncsi.txt",                         handlePortal);  // Windows
  server.on("/canonical.html",                   handlePortal);  // 火狐
  server.on("/success.txt",                      handlePortal);
#endif

  server.onNotFound(handlePortal);   // 其他任何地址也拽回控制页

  server.begin();
  Serial.println("[HTTP] 服务已启动，端口 80");
}

// ---------------------------------------------------------------- 安全看门狗
// 断线/关网页/手机锁屏时，客户端的刷新会中断 -> 自动停车，避免机器人跑飞
static void safetyTick() {
  uint32_t now = millis();

  if (g.moveActive && (now - g.lastMove > MOVE_WATCHDOG_MS)) {
    stopDrive();
    Serial.println("[安全] 行走指令超时，已停车");
  }

  if (g.actActive) {
    if (now - g.lastAct > ACT_WATCHDOG_MS) {
      stopActuator();
      Serial.println("[安全] 推杆指令超时，已停止");
    } else if (now - g.actStart > ACT_MAX_MS) {
      lockActuator();            // 连续跑太久：锁死，必须松手重按
      Serial.println("[安全] 推杆连续运行超时，已锁定");
    }
  }
}

// ---------------------------------------------------------------- 串口调试
// 没连手机时也能用串口快速验证接线：发送单字符
//   f/b/l/r/x  前后左右停        u/d/s  推杆伸出/收回/停
//   0~9        舵机转 0~180 度   v      舵机自检扫一遍
static void serialTick() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'f': applyMove("f"); Serial.println("[串口] 前进"); break;
      case 'b': applyMove("b"); Serial.println("[串口] 后退"); break;
      case 'l': applyMove("l"); Serial.println("[串口] 左转"); break;
      case 'r': applyMove("r"); Serial.println("[串口] 右转"); break;
      case 'x': stopDrive();  Serial.println("[串口] 停车"); break;
      case 'u': actuator.set(1);  g.actActive = true; g.lastAct = g.actStart = millis();
                Serial.println("[串口] 推杆伸出"); break;
      case 'd': actuator.set(-1); g.actActive = true; g.lastAct = g.actStart = millis();
                Serial.println("[串口] 推杆收回"); break;
      case 's': stopActuator(); Serial.println("[串口] 推杆停止"); break;
      // 舵机调试：v = 慢速自检扫一遍，0~9 = 慢慢转到 0/20/40.../180 度
      case 'v': servo.sweepTest(); Serial.println("[串口] 舵机自检"); break;
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9': {
        int deg = (c - '0') * 20;
        servo.setTarget(deg);                    // 同样逐度慢慢走过去
        Serial.printf("[串口] 舵机 %d° -> %d°（%d°/s，%dms/度）\n",
                      servo.angle(), deg, servo.stepSpeed(), SERVO_STEP_DELAY_MS);
        break;
      }
      default: break;
    }
  }
}

// ---------------------------------------------------------------- 主流程
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("====================================");
  Serial.println(" 跨障碍擦玻璃机器人 ESP32-S3 启动");
  Serial.println("====================================");

  hardwareInit();
  wifiInit();
  webInit();

  Serial.println("[系统] 初始化完成，等待手机连接");
}

void loop() {
#if CAPTIVE_PORTAL
  dnsServer.processNextRequest();
#endif
  server.handleClient();
  safetyTick();
  serialTick();
  delay(1);
}

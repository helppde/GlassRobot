#pragma once
#include <Arduino.h>
#include "config.h"

/* ============================================================================
 *  硬件抽象层
 *  ┌─────────────┬──────────────────────────────────────────────┐
 *  │ DriveMotor  │ L298N 纯数字直驱：正转 / 反转 / 刹车 / 滑行  │
 *  │ Actuator    │ L298N 纯数字直驱：伸出 / 收回 / 停           │
 *  │ ServoOut    │ 唯一的硬件 PWM 输出（50Hz）                  │
 *  │ 开关量      │ digitalWrite，支持高/低电平两种触发方式      │
 *  └─────────────┴──────────────────────────────────────────────┘
 *  只有舵机需要 PWM，其余全部是高低电平。
 * ========================================================================== */

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 1
#endif

// ------------------------------------------------- 单路直流电机（L298N 直驱）
/*  EN 跳线帽短接 -> 通道常使能 -> 永远全速。
 *  IN 脚只看高低电平，所以只有两种转法，没有中间速度：
 *      dir =  1  IN1=1 IN2=0  正转
 *      dir = -1  IN1=0 IN2=1  反转
 *      dir =  0  见 MOTOR_STOP_BRAKE（刹车 or 滑行）
 */
class DriveMotor {
public:
  void begin(int in1, int in2) {
    _in1 = in1;
    _in2 = in2;
    pinMode(_in1, OUTPUT);
    pinMode(_in2, OUTPUT);
    _dir = 99;          // 强制第一次 set() 真正写一次引脚
    set(0);
  }

  /** dir: 1=正转, -1=反转, 0=停 */
  void set(int dir) {
    dir = (dir > 0) ? 1 : (dir < 0 ? -1 : 0);
    if (dir == _dir) return;   // 状态没变就不重复写引脚
    _dir = dir;

    if (dir > 0) {
      digitalWrite(_in1, HIGH);
      digitalWrite(_in2, LOW);
    } else if (dir < 0) {
      digitalWrite(_in1, LOW);
      digitalWrite(_in2, HIGH);
    } else {
#if MOTOR_STOP_BRAKE
      digitalWrite(_in1, HIGH);     // 两脚同高 -> 电机绕组短接 -> 刹车
      digitalWrite(_in2, HIGH);
#else
      digitalWrite(_in1, LOW);
      digitalWrite(_in2, LOW);
#endif
    }
  }

  int value() const { return _dir; }

private:
  int _in1 = -1;
  int _in2 = -1;
  int _dir = 99;
};

// --------------------------------------------------- 电动推杆（L298N 直驱）
// 由 L298N 模块 2 的 A 路驱动，EN 常使能，只有两个 IN 脚
// dir: 1 = 伸出, -1 = 收回, 0 = 停止
class Actuator {
public:
  void begin(int in1, int in2) {
    _in1 = in1;
    _in2 = in2;
    pinMode(_in1, OUTPUT);
    pinMode(_in2, OUTPUT);
    _raw(false, false);
  }

  void set(int dir) {
    dir = constrain(dir, -1, 1);
    if (dir == _dir) return;

    _raw(false, false);               // 先全部断开
    if (dir != 0) delay(ACT_DEAD_MS); // 换向死区，防止瞬间直通冲击 L298N

    _dir = dir;
    if (dir == 1)       _raw(true,  false);
    else if (dir == -1) _raw(false, true);
  }

  int value() const { return _dir; }

private:
  void _raw(bool a, bool b) {
    digitalWrite(_in1, a ? HIGH : LOW);
    digitalWrite(_in2, b ? HIGH : LOW);
  }

  int _in1 = -1;
  int _in2 = -1;
  int _dir = 0;
};

// ------------------------------------------------ 舵机（唯一使用 PWM 的外设）
/*  ★ 关于 PWM 分辨率的坑（已在 core 3.1.1 源码里核实）：
 *
 *      esp32-hal-ledc.c:40    #define LEDC_MAX_BIT_WIDTH SOC_LEDC_TIMER_BIT_WIDTH
 *      soc_caps.h  (esp32s3)  #define SOC_LEDC_TIMER_BIT_WIDTH  (14)
 *      esp32-hal-ledc.c:103   if (resolution > LEDC_MAX_BIT_WIDTH) { log_e(...); return false; }
 *
 *    ESP32-S3 的 LEDC 计数器只有 14 位，请求 16 位会被直接拒绝且静默失败。
 *    （老 ESP32 是 20 位，所以网上的 16 位例程在 S3 上会不工作。）
 *
 *  ★ 关于速度（就是"逐度 + delay"的写法）：
 *    舵机只会"尽最大速度冲到目标脉宽"，硬件层面没有调速功能，
 *    所以把角度整数化，一次只走 1 度，走完 delay(SERVO_STEP_DELAY_MS)：
 *
 *        while (_angle != target) {
 *          _angle += ±1;           // 一步一步来，绝不跳变
 *          writePulse(_angle);
 *          delay(SERVO_STEP_DELAY_MS);
 *        }
 *
 *    速度 = 1000 / SERVO_STEP_DELAY_MS 度/秒。
 *        delay(20) -> 50°/s  -> 180° 走 3.6 秒
 *        delay(40) -> 25°/s  -> 180° 走 7.2 秒
 *
 *  ★ 为什么放在独立任务里：
 *    上面这段是阻塞的。如果直接放在 Web 请求处理函数里跑，
 *    转动那几秒 ESP32 就没法响应别的请求，网页会像死机一样。
 *    所以整段逐度循环放进一个独立的 FreeRTOS 任务：
 *      · delay() 只是让这个任务睡一会，loopTask 照常跑，网页不卡
 *      · 每走一度写一次 PWM，严格 1 度 1 步，速度完全由 delay 决定
 *      · 转动途中收到新角度只需改一个变量，当前段走完自动接上新的
 */
class ServoOut {
public:
  bool begin(int pin) {
    _pin = pin;
    if (_pin < 0) {
      Serial.println("[舵机] 引脚未配置，跳过");
      return false;
    }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // 3.x：先试显式通道，失败再退回自动分配
    _ok = ledcAttachChannel(_pin, SERVO_FREQ, SERVO_RES, SERVO_CHANNEL);
    if (!_ok) {
      Serial.println("[舵机] ledcAttachChannel 失败，改用 ledcAttach 重试");
      _ok = ledcAttach(_pin, SERVO_FREQ, SERVO_RES);
    }
    if (!_ok) {
      Serial.printf("[舵机] 初始化失败！GPIO%d 拿不到 LEDC 通道\n", _pin);
      Serial.println("[舵机] 请检查该引脚是否被 PSRAM/Flash 占用");
      return false;
    }
    Serial.printf("[舵机] OK  GPIO%d  通道%d  频率%uHz  分辨率%d位  脉宽%u~%uus\n",
                  _pin, SERVO_CHANNEL, (unsigned)ledcReadFreq(_pin),
                  SERVO_RES, SERVO_MIN_US, SERVO_MAX_US);
#else
    ledcSetup(SERVO_CHANNEL, SERVO_FREQ, SERVO_RES);
    ledcAttachPin(_pin, SERVO_CHANNEL);
    _ok = true;
    Serial.printf("[舵机] OK  GPIO%d  通道%d  频率%uHz  分辨率%d位  脉宽%u~%uus\n",
                  _pin, SERVO_CHANNEL, (unsigned)ledcReadFreq(SERVO_CHANNEL),
                  SERVO_RES, SERVO_MIN_US, SERVO_MAX_US);
#endif

    // 开机只停在这个初始角度，之后不再自己动（不做自检扫描）
    _angle  = constrain((int)SERVO_DEFAULT_ANGLE, 0, 180);
    _target = _angle;
    writePulse(_angle);

    // 逐度移动任务：核心 1（和 loop 同核，但优先级更高，delay 时会让出 CPU）
    BaseType_t r = xTaskCreatePinnedToCore(
        &ServoOut::taskThunk, "servo", 2048, this, 2, &_task, 1);
    if (r != pdPASS) {
      Serial.println("[舵机] 任务创建失败，逐度移动不可用");
      return false;
    }

    Serial.printf("[舵机] 初始角度 %d°（开机保持不动）\n", _angle);
    Serial.printf("[舵机] 逐度移动 %d°/s（%dms/度），180° 约需 %.1f 秒\n",
                  stepSpeed(), SERVO_STEP_DELAY_MS, 180.0f * SERVO_STEP_DELAY_MS / 1000.0f);
    return true;
  }

  /** 设定目标角度：任务会 1 度 1 度地慢慢走过去，立即返回不阻塞 */
  void setTarget(int deg) { _target = constrain(deg, 0, 180); }

  /** 急停：就停在当前这一度，不再继续走 */
  void halt() { _target = _angle; }

  /** 立即跳到位，不逐度移动。只用于初始化 */
  void setNow(int deg) {
    _angle  = constrain(deg, 0, 180);
    _target = _angle;
    writePulse(_angle);
  }

  /** 串口调试：慢速走 90->0->180->90（会阻塞，仅调试用） */
  void sweepTest() {
    if (!_ok) return;
    Serial.println("[舵机] 自检中 90 -> 0 -> 180 -> 90");
    const int seq[] = {90, 0, 180, 90};
    for (uint8_t i = 0; i < 4; i++) {
      _target = seq[i];
      while (_angle != _target) delay(2);        // 等任务慢慢走完
    }
    Serial.println("[舵机] 自检结束");
  }

  int      angle()  const { return _angle; }     // 当前角度
  int      target() const { return _target; }    // 目标角度
  bool     moving() const { return _angle != _target; }
  uint32_t duty()   const { return _duty; }
  bool     ok()     const { return _ok; }
  int      stepSpeed() const {
    return SERVO_STEP_DELAY_MS > 0 ? (1000 / SERVO_STEP_DELAY_MS) : 0;
  }

private:
  /** 逐度移动主循环：1 度 1 步，每步 delay 一次 */
  static void taskThunk(void *self) {
    ServoOut *s = static_cast<ServoOut *>(self);
    for (;;) {
      int  t = s->_target;
      int  c = s->_angle;
      if (c == t) {                 // 已到位：歇着等新目标
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      s->_angle = c + ((t > c) ? 1 : -1);   // 只走 1 度
      s->writePulse(s->_angle);
      delay(SERVO_STEP_DELAY_MS);           // 等舵机跟上这一度
    }
  }

  /** 角度 -> 脉宽 -> 占空比。脉宽没变就不重复写 LEDC */
  void writePulse(int deg) {
    uint32_t us = SERVO_MIN_US +
                  (uint32_t)((long)(SERVO_MAX_US - SERVO_MIN_US) * deg / 180);
    if (us == _lastUs) return;
    _lastUs = us;

    uint32_t maxD = (1UL << SERVO_RES) - 1;      // 14 位 -> 16383
    uint32_t duty = (uint32_t)((uint64_t)us * maxD / 20000ULL);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(_pin, duty);
#else
    ledcWrite(SERVO_CHANNEL, duty);
#endif
    _duty = duty;
  }

  int          _pin    = -1;
  volatile int _angle  = SERVO_DEFAULT_ANGLE;
  volatile int _target = SERVO_DEFAULT_ANGLE;
  uint32_t     _lastUs = 0xFFFFFFFF;             // 强制第一次一定写一次
  volatile uint32_t _duty = 0;
  bool         _ok     = false;
  TaskHandle_t _task   = nullptr;
};

// ------------------------------------------------------------ 开关量输出工具
/*  关键点：必须先把输出寄存器写成"无效"电平，再切换成 OUTPUT。
 *  否则上电瞬间引脚悬空，低电平触发或高电平触发的继电器模块都可能误吸合一下。
 */
inline void outBegin(int pin, bool activeHigh) {
  digitalWrite(pin, activeHigh ? LOW : HIGH);   // 先写无效电平
  pinMode(pin, OUTPUT);
  digitalWrite(pin, activeHigh ? LOW : HIGH);   // 再确认一次
}

inline void writeActive(int pin, bool on, bool activeHigh) {
  digitalWrite(pin, (on == activeHigh) ? HIGH : LOW);
}

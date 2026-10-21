/*
  HUB-8735 Ultra - 人脸识别 + 伺服马达（IO21）：认识的人转到 70 度，陌生人转到 -20 度

  这是 15_face_detection_rtsp（人脸识别 + 网页画面 + 控制台）加上 16_servo_func（伺服马达）：

    画面里有陌生人                -> 伺服马达转到 -20 度（优先）
    画面里只有认识的人            -> 伺服马达转到  70 度
    画面里没有人                  -> 维持在最后一次的位置
    状态要稳定 0.3 秒才会动，避免人脸在「认识 / 陌生」边缘跳来跳去时马达抖动。
    转动速度是 16 的两倍：每 15 ms 转 6 度（约 400 度/秒）。

  关于 -20 度：
    一般伺服马达的角度是 0~180 度（脉宽约 544~2400 微秒），-20 度已经超出这个范围。
    这里把脉宽下限放宽到 340 微秒（约等于 -20 度），伺服马达是不是真的会转到那里
    取决于型号：有的会转到机械极限就停住并发出嗡嗡声，长时间嗡嗡声会让马达发热，
    如果听到这种声音，把 ANGLE_STRANGER 改成 0 或把 SERVO_MIN_US 调高即可。

  接线：
    伺服马达讯号线 -> IO21（PF_8，支援 PWM）；电源建议接外部 5V，GND 要跟板子共地
    （马达启动电流大，直接用板子 5V 脚供电可能让板子重开机）。

  其余功能同 15：
    - 相机 VGA 640x480 JPEG，网页一张接一张向 /snap 要快照当作影像；
    - 板载 NPU 人脸侦测（SCRFD）+ 识别（MobileFaceNet），人脸框与代号由网页叠在画面上；
    - 自动记住陌生人（单独出现超过 2 秒 -> P01、P02...，备份到快闪记忆体）；
      注意：登记完成后这个人就变成「认识的人」，伺服马达会从 -20 度转到 70 度；
    - 网页控制台 http://<板子 IP>/ ：登记 / 改名 / 删除 / 备份 / 还原 / 严格度 / 伺服马达测试；
    - FUNC 键（IO12）= 登记画面中的人脸；OLED 显示 IP、人脸数、代号、伺服马达角度。

  限制同 15：最多登记 20 人；名单是程序自己记的（重开机还原后要等每个人被识别到才会出现）；
  代号 / 名称只允许英文、数字、- 和 _；没有表情辨识；HTTP（SDK 没有 HTTPS 伺服器）。

  WiFi：SSID / 密码放在同一个资料夹的 secrets.h（已 gitignore，不会被 commit）。
  画面文字使用简体中文（OLED 字型只保证涵盖 GB2312 字集）；网页使用繁体中文。
*/

#include <stdarg.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <AmebaServo.h>
#include "StreamIO.h"
#include "VideoStream.h"
#include "NNFaceDetectionRecognition.h"
#include "secrets.h"

#define CHANNEL   0  // HD JPEG，给网页的 MJPEG
#define CHANNELNN 3  // RGB，给人脸侦测 / 识别

#define NNWIDTH  576
#define NNHEIGHT 320

#define JPEG_RES VIDEO_VGA  // 640x480，跟 00-3 验证过会动的设定一样；要更清晰再改 VIDEO_HD

const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;
const uint8_t FUNC_PIN = 12;

const uint8_t NN_FPS = 15;
const uint32_t MAX_VIDEO_FPS = 10;  // 网页画面最高张数 / 秒（用来限制频宽）

const uint8_t MAX_REG = 20;          // SDK 上限（MAX_FRC_REG_NUM）
const uint8_t NAME_LEN = 16;
const uint32_t UNKNOWN_HOLD_MS = 2000;     // 陌生人持续出现多久才自动登记
const uint32_t ENROLL_COOLDOWN_MS = 8000;  // 两次自动登记的最短间隔
const uint32_t PENDING_TIMEOUT_MS = 4000;  // 登记后多久没被认出来就放弃等待
const uint32_t BACKUP_DELAY_MS = 1500;

// ---- 伺服马达（IO21）----
const uint8_t SERVO_PIN = 21;
const int ANGLE_KNOWN = 70;       // 画面里只有认识的人
const int ANGLE_STRANGER = -20;   // 画面里有陌生人
const int SERVO_US_AT_0 = 544;    // 0 度的脉宽（微秒），跟 AmebaServo 预设一样
const int SERVO_US_AT_180 = 2400; // 180 度的脉宽
const int SERVO_MIN_US = 340;     // 允许的最小脉宽（约 -20 度）；马达嗡嗡响就调高
const uint8_t SERVO_STEP_DEG = 6; // 每次转几度（16 是 3 度，这里快 2 倍）
const uint16_t SERVO_STEP_MS = 15;
const uint32_t SERVO_HOLD_MS = 300;  // 状态要稳定多久才动

AmebaServo servo;
int servoAngle = 0;               // 马达目前被指令到的角度
int servoTarget = 0;              // 目标角度
uint32_t lastServoStepMs = 0;
int8_t seenState = 0;             // 画面里的状态：0 没人，1 只有认识的人，2 有陌生人
int8_t appliedState = 0;          // 已经套用到马达的状态
uint32_t seenSinceMs = 0;

U8G2 display;
WiFiServer server(80);

VideoSetting configV(JPEG_RES, CAM_FPS, VIDEO_JPEG, 1);
VideoSetting configNN(NNWIDTH, NNHEIGHT, NN_FPS, VIDEO_RGB, 0);
NNFaceDetectionRecognition facerecog;
StreamIO videoStreamerNN(1, 1);  // 相机 channel 3 -> 人脸识别

// ---- 长连线：MJPEG 影像 / SSE 事件 ----
WiFiClient eventClient;
bool eventActive = false;

// ---- 已登记名单（程序自己记，SDK 没有列出名单的函式）----
char regNames[MAX_REG][NAME_LEN];
uint8_t regCount = 0;
uint8_t nextId = 1;

// ---- 回呼函式（NN 工作执行绪）写入、主迴圈 / 网页读取 ----
volatile uint8_t faceCount = 0;
volatile uint32_t faceSeq = 0;      // 每处理一帧 +1，SSE 靠它判断有没有新结果
char faceNames[4][NAME_LEN];        // 画面里前 4 张脸的识别结果（"unknown" 表示陌生人）
int16_t faceBox[4][4];              // 座标 0~1000（千分比）：x0, y0, x1, y1
volatile uint32_t lastFaceMs = 0;   // 最近一次看到人脸的时间
volatile uint32_t unknownSinceMs = 0;  // 单一陌生人开始持续出现的时间，0 = 没有
volatile bool enrollRequest = false;   // 回呼函式要求主迴圈自动登记
volatile uint32_t lastEnrollMs = 0;

// ---- 登记流程 ----
char pendingFallback[NAME_LEN] = "";  // 改名失败时要还原回去的旧名字
char pendingName[NAME_LEN] = "";  // 已送出登记、等着被认出来确认的名字
uint32_t pendingSinceMs = 0;
bool backupDue = false;
uint32_t backupDueMs = 0;

// ---- 设定 ----
bool autoEnroll = true;
int threshold = -1;  // -1 = 没调整过，用 SDK 预设
char lastMsg[64] = "就绪";

uint8_t u8x8_gpio_and_delay_fast(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
      pinMode(OLED_SDA, OUTPUT);
      pinMode(OLED_SCL, OUTPUT);
      digitalWrite(OLED_SDA, HIGH);
      digitalWrite(OLED_SCL, HIGH);
      break;
    case U8X8_MSG_GPIO_I2C_CLOCK:
      digitalWrite(OLED_SCL, arg_int);
      break;
    case U8X8_MSG_GPIO_I2C_DATA:
      digitalWrite(OLED_SDA, arg_int);
      break;
    case U8X8_MSG_DELAY_MILLI:
      delay(arg_int);
      break;
    case U8X8_MSG_DELAY_I2C:
      delayMicroseconds(arg_int <= 2 ? 5 : 2);
      break;
    default:
      return 0;
  }
  return 1;
}

// 把格式化的文字接在 buf 后面。
// 注意：这个核心把 snprintf 换成了自己的版本，遇到含中文（多位元组）的 %s 时，回传值
// 会少算（之前拿回传值累加位置，JSON 就被后面的字蓋掉一截，网页因此一直显示离线），
// 所以这里不信回传值，每次都用 strlen() 重新量。
void appendf(char *buf, size_t cap, const char *fmt, ...) {
  const size_t len = strlen(buf);
  if (len + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
}

// ---------------------------------------------------------------- 伺服马达

// 角度（可以是负的）-> 脉宽，再限制在 [SERVO_MIN_US, 2400]。
void servoWrite(int angle) {
  int us = SERVO_US_AT_0 + static_cast<int>((static_cast<long>(angle) * (SERVO_US_AT_180 - SERVO_US_AT_0)) / 180);
  us = constrain(us, SERVO_MIN_US, SERVO_US_AT_180);
  servo.writeMicroseconds(us);
}

// 依画面里的人脸决定目标角度，并平顺地转过去。
void updateServo() {
  // 画面里的状态：有陌生人 > 只有认识的人 > 没人
  const uint8_t n = faceCount > 4 ? 4 : faceCount;
  int8_t state = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (strcmp(faceNames[i], "unknown") == 0) {
      state = 2;
      break;
    }
    state = 1;
  }
  if (state != seenState) {
    seenState = state;
    seenSinceMs = millis();
  }
  if (state != 0 && state != appliedState && millis() - seenSinceMs >= SERVO_HOLD_MS) {
    appliedState = state;
    servoTarget = (state == 1) ? ANGLE_KNOWN : ANGLE_STRANGER;
    Serial.print("[SERVO] ");
    Serial.print(state == 1 ? "认识的人" : "陌生人");
    Serial.print(" -> ");
    Serial.print(servoTarget);
    Serial.println(" 度");
  }

  if (servoAngle != servoTarget && millis() - lastServoStepMs >= SERVO_STEP_MS) {
    lastServoStepMs = millis();
    // 注意：NN 的标头会把 Arduino 的 min()/max() 宏取消掉，所以这里不用它们
    if (servoAngle < servoTarget) {
      servoAngle += SERVO_STEP_DEG;
      if (servoAngle > servoTarget) servoAngle = servoTarget;
    } else {
      servoAngle -= SERVO_STEP_DEG;
      if (servoAngle < servoTarget) servoAngle = servoTarget;
    }
    servoWrite(servoAngle);
  }
}

// ---------------------------------------------------------------- 名单管理

int findReg(const char *name) {
  for (uint8_t i = 0; i < regCount; i++) {
    if (strcmp(regNames[i], name) == 0) return i;
  }
  return -1;
}

void addReg(const char *name) {
  if (findReg(name) >= 0 || regCount >= MAX_REG) return;
  snprintf(regNames[regCount], NAME_LEN, "%s", name);
  regCount++;

  // 名字是 P01 这种格式的话，下一个自动代号往后排，避免撞号
  if (name[0] == 'P' && strlen(name) <= 3) {
    const int n = atoi(name + 1);
    if (n >= nextId) nextId = n + 1;
  }
}

void removeReg(const char *name) {
  const int idx = findReg(name);
  if (idx < 0) return;
  for (uint8_t i = idx; i + 1 < regCount; i++) {
    memcpy(regNames[i], regNames[i + 1], NAME_LEN);
  }
  regCount--;
}

// 名称只允许英数字、- 和 _，1~12 字，不能叫 unknown。
bool validName(const char *name) {
  const size_t len = strlen(name);
  if (len == 0 || len > 12) return false;
  for (size_t i = 0; i < len; i++) {
    const char c = name[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return strcmp(name, "unknown") != 0;
}

void setMsg(const char *msg) {
  snprintf(lastMsg, sizeof(lastMsg), "%s", msg);
  Serial.print("[MSG] ");
  Serial.println(msg);
}

// 登记画面中的人脸（必须刚好 1 张脸）。name 为空就用下一个自动代号。
// 回传 true = 已送出登记（要等 NN 下一帧才会真的认得）。
bool enrollCurrentFace(const char *requested) {
  if (faceCount != 1) {
    setMsg("登記失敗：畫面中需要剛好 1 張臉");
    return false;
  }
  if (pendingName[0] != 0) {
    setMsg("上一筆登記還在處理中，請稍候");
    return false;
  }
  if (regCount >= MAX_REG) {
    setMsg("登記失敗：已達 20 人上限，請先刪除");
    return false;
  }

  char name[NAME_LEN];
  if (requested && requested[0]) {
    if (!validName(requested)) {
      setMsg("名稱只能用英文、數字、- 與 _（最長 12 字）");
      return false;
    }
    if (findReg(requested) >= 0) {
      setMsg("這個名稱已經登記過了");
      return false;
    }
    snprintf(name, sizeof(name), "%s", requested);
  } else {
    do {
      snprintf(name, sizeof(name), "P%02u", nextId);
      nextId++;
    } while (findReg(name) >= 0);
  }

  facerecog.registerFace(name);
  snprintf(pendingName, sizeof(pendingName), "%s", name);
  pendingSinceMs = millis();
  lastEnrollMs = millis();
  Serial.print("[FACE] 送出登记：");
  Serial.println(name);
  setMsg("已送出登記，辨識中…");
  return true;
}

// 改名：SDK 没有改名函式，只能「删掉旧名字 + 用同一张脸重新登记成新名字」，
// 所以该本人必须单独正对镜头、并且现在被认成旧名字（确认是同一个人才不会改错）。
// 重新登记失败（逾时）时，主迴圈会用旧名字再登记回去，不会让这个人整个消失。
void renameFace(const char *oldName, const char *newName) {
  if (findReg(oldName) < 0) {
    setMsg("找不到這個名稱");
    return;
  }
  if (!validName(newName)) {
    setMsg("名稱只能用英文、數字、- 與 _（最長 12 字）");
    return;
  }
  if (strcmp(oldName, newName) == 0) {
    setMsg("新舊名稱一樣");
    return;
  }
  if (findReg(newName) >= 0) {
    setMsg("這個名稱已經有人用了");
    return;
  }
  if (pendingName[0] != 0) {
    setMsg("上一筆登記還在處理中，請稍候");
    return;
  }
  if (faceCount != 1 || strcmp(faceNames[0], oldName) != 0) {
    setMsg("改名需要該本人單獨正對鏡頭（目前沒有認出這個人）");
    return;
  }

  facerecog.removeFace(oldName);
  removeReg(oldName);
  facerecog.registerFace(newName);
  snprintf(pendingName, sizeof(pendingName), "%s", newName);
  snprintf(pendingFallback, sizeof(pendingFallback), "%s", oldName);
  pendingSinceMs = millis();
  lastEnrollMs = millis();
  Serial.print("[FACE] 改名：");
  Serial.print(oldName);
  Serial.print(" -> ");
  Serial.println(newName);
  setMsg("改名中，請保持不動…");
}

// ---------------------------------------------------------------- 人脸识别回呼

// 每处理完一帧 NN 影像就被呼叫一次。这里只记录结果（画框交给网页），不呼叫
// registerFace（登记动作交给主迴圈，避免在 NN 执行绪里改 NN 的状态）。
void faceCallback(std::vector<FaceRecognitionResult> results) {
  const int count = facerecog.getResultCount();

  uint8_t unknownCount = 0;
  for (int i = 0; i < count; i++) {
    FaceRecognitionResult item = results[i];
    const bool known = strcmp(item.name(), "unknown") != 0;

    if (i < 4) {
      snprintf(faceNames[i], NAME_LEN, "%s", item.name());
      faceBox[i][0] = static_cast<int16_t>(item.xMin() * 1000);
      faceBox[i][1] = static_cast<int16_t>(item.yMin() * 1000);
      faceBox[i][2] = static_cast<int16_t>(item.xMax() * 1000);
      faceBox[i][3] = static_cast<int16_t>(item.yMax() * 1000);
    }
    if (!known) unknownCount++;

    // 登记过的名字，补进名单（重开机还原后靠这个把名单补回来）；
    // 并确认刚刚送出的登记已经生效。
    if (known) {
      addReg(item.name());
      if (pendingName[0] != 0 && strcmp(pendingName, item.name()) == 0) {
        pendingName[0] = 0;
        pendingFallback[0] = 0;
        backupDue = true;
        backupDueMs = millis();
        Serial.print("[FACE] 登记成功：");
        Serial.println(item.name());
      }
    }
  }

  if (count != faceCount) {
    Serial.print("[FACE] 人脸数 = ");
    Serial.println(count);
  }
  faceCount = count;
  if (count > 0) lastFaceMs = millis();
  faceSeq = faceSeq + 1;

  // 只有「刚好 1 张脸、而且是陌生人」才开始计时；持续够久就要求自动登记
  if (count == 1 && unknownCount == 1) {
    if (unknownSinceMs == 0) unknownSinceMs = millis();
    if (autoEnroll && regCount < MAX_REG && pendingName[0] == 0 &&
        millis() - unknownSinceMs >= UNKNOWN_HOLD_MS &&
        millis() - lastEnrollMs >= ENROLL_COOLDOWN_MS) {
      enrollRequest = true;
    }
  } else {
    unknownSinceMs = 0;
  }
}

// ---------------------------------------------------------------- 网页

// 控制台页面。用 C++11 原始字串，里面的引号、反斜线都不用跳脱。
static const char PAGE[] = R"HTML(<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>人臉辨識控制台</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--tx:#1d2733;--mu:#6b7785;--ac:#1f7aec;--ok:#1b9e5a;--bad:#d64545;--bd:#e1e6ec}
@media(prefers-color-scheme:dark){:root{--bg:#10151b;--card:#1a222b;--tx:#e6ebf0;--mu:#8b97a5;--ac:#5aa2ff;--ok:#3ec27e;--bad:#ff6b6b;--bd:#2a3541}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.5 system-ui,"Noto Sans TC",sans-serif;padding:16px}
main{max-width:720px;margin:0 auto;display:grid;gap:12px}
header{display:flex;justify-content:space-between;align-items:center}h1{font-size:20px;margin:0}
h2{font-size:14px;margin:0 0 8px;color:var(--mu);font-weight:600}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px}
.pill{font-size:12px;padding:2px 10px;border-radius:99px;background:var(--bd);color:var(--mu)}
.pill.on{background:var(--ok);color:#fff}.pill.off{background:var(--bad);color:#fff}
#stage{position:relative;aspect-ratio:16/9;background:#000;border-radius:12px;overflow:hidden;box-shadow:0 2px 12px rgba(0,0,0,.25)}
#stage img,#stage canvas{position:absolute;inset:0;width:100%;height:100%}#stage img{object-fit:fill}#stage canvas{pointer-events:none}
.row{display:flex;gap:14px;align-items:center}.big{font-size:44px;font-weight:700;min-width:56px;text-align:center}
.muted{color:var(--mu);font-size:13px}
input[type=text]{width:100%;padding:9px 10px;border:1px solid var(--bd);border-radius:8px;background:var(--bg);color:var(--tx);font-size:15px}
button{padding:8px 14px;border:0;border-radius:8px;background:var(--ac);color:#fff;font-size:14px;cursor:pointer;margin:6px 6px 0 0}
button.danger{background:var(--bad)}button.ghost{background:var(--bd);color:var(--tx)}
.chips{display:flex;flex-wrap:wrap;gap:8px}.chip{display:flex;align-items:center;gap:6px;background:var(--bg);border:1px solid var(--bd);border-radius:99px;padding:3px 6px 3px 12px}
.chip.live{border-color:var(--ok);color:var(--ok)}.chip button{margin:0;padding:0 6px;background:transparent;color:var(--bad);font-size:18px}.chip button.ed{color:var(--ac);font-size:15px}
label{display:block;margin:6px 0}input[type=range]{vertical-align:middle;width:55%}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:var(--tx);color:var(--bg);padding:8px 16px;border-radius:99px;opacity:0;transition:.2s;pointer-events:none}
#toast.show{opacity:.92}
</style></head><body><main>
<header><h1>人臉辨識控制台</h1><span id="st" class="pill">連線中…</span></header>
<div id="stage"><img id="cam" alt=""><canvas id="ov"></canvas></div>
<section class="card"><h2>即時狀態</h2><div class="row"><div class="big" id="cnt">0</div>
<div><div id="faces">未偵測到人臉</div><div class="muted" id="meta"></div></div></div></section>
<section class="card"><h2>登記人臉</h2><input type="text" id="nm" maxlength="12" placeholder="名稱（英數字、- _，留空自動取代號 P01…）">
<button id="reg">登記畫面中的人臉</button><div class="muted">畫面中需要剛好 1 張臉。板子上按 FUNC 鍵也能登記。</div></section>
<section class="card"><h2>已登記（<span id="rc">0</span> / 20）</h2><div class="chips" id="regs"></div>
<div class="muted" id="regtip"></div></section>
<section class="card"><h2>伺服馬達（IO21）</h2><div class="row"><div class="big" id="svA" style="font-size:34px">0°</div>
<div><div id="svT" class="muted"></div><button id="svK" class="ghost">測試：認識的人</button><button id="svS" class="ghost">測試：陌生人</button></div></div>
<div class="muted">認識的人轉到 70°，陌生人轉到 -20°；狀態穩定 0.3 秒才會轉。</div></section>
<section class="card"><h2>設定</h2>
<label><input type="checkbox" id="auto"> 自動登記陌生人（單獨出現超過 2 秒）</label>
<label>辨識嚴格度　<input type="range" id="thr" min="0" max="100" value="50"> <span id="tv">預設</span>
<button id="ta" class="ghost">套用</button></label>
<div class="muted">數值越高越嚴格（較不容易認錯人，但也較容易認不出來）。</div></section>
<section class="card"><h2>記憶</h2><button id="bk">備份到板子</button><button id="rs" class="ghost">從板子還原</button>
<button id="rst" class="danger">清除全部</button></section></main><div id="toast"></div>
<script>
const $=i=>document.getElementById(i);let tt;
function toast(m){const t=$('toast');t.textContent=m;t.className='show';clearTimeout(tt);tt=setTimeout(()=>t.className='',2200)}
async function api(q){try{const r=await fetch('/api/'+q,{cache:'no-store'});return await r.json()}catch(e){return null}}
async function cmd(q){const j=await api('cmd?'+q);toast(j?j.msg:'連線失敗');refresh()}
/* 即時畫面（MJPEG）與人臉框（SSE） */
const cam=$('cam'),ov=$('ov'),octx=ov.getContext('2d');
let snapN=0,snapT=0;function nextSnap(){snapT=performance.now();cam.src='/snap?'+(snapN++)}
cam.onload=()=>setTimeout(nextSnap,Math.max(0,60-(performance.now()-snapT)));cam.onerror=()=>setTimeout(nextSnap,600);nextSnap();
let faces=[],facesT=0;
function fitOv(){const r=$('stage').getBoundingClientRect();ov.width=r.width;ov.height=r.height}
function drawOv(){octx.clearRect(0,0,ov.width,ov.height);if(performance.now()-facesT>1500)return;
 for(const f of faces){const k=f[0],n=f[1],x=f[2]/1000*ov.width,y=f[3]/1000*ov.height,w=(f[4]-f[2])/1000*ov.width,h=(f[5]-f[3])/1000*ov.height;
  const col=k?'#3ec27e':'#ff5a5a',lb=k?n:'?';octx.lineWidth=3;octx.strokeStyle=col;octx.strokeRect(x,y,w,h);
  octx.font='bold 14px system-ui';const tw=octx.measureText(lb).width+10;octx.fillStyle=col;octx.fillRect(x-1.5,y-22,tw,20);
  octx.fillStyle='#fff';octx.fillText(lb,x+4,y-7)}}
new ResizeObserver(()=>{fitOv();drawOv()}).observe($('stage'));fitOv();
const es=new EventSource('/events');
es.onmessage=e=>{try{faces=JSON.parse(e.data).f;facesT=performance.now();drawOv()}catch(x){}};
setInterval(drawOv,500);
async function refresh(){const j=await api('status');const st=$('st');
if(!j){st.textContent='離線';st.className='pill off';return}
st.textContent='已連線';st.className='pill on';
$('cnt').textContent=j.faces.length;
$('faces').innerHTML=j.faces.length?j.faces.map(f=>f.k?'<b style="color:var(--ok)">'+f.n+'</b>':'<b style="color:var(--bad)">陌生人</b>').join('　'):'未偵測到人臉';
$('meta').textContent=j.res+' · AI '+j.nn+' fps　'+j.msg;
$('rc').textContent=j.reg.length;
const live=j.faces.filter(f=>f.k).map(f=>f.n);
$('regs').innerHTML=j.reg.map(n=>'<span class="chip'+(live.includes(n)?' live':'')+'">'+n+'<button class="ed" data-r="'+n+'" title="改名">✎</button><button data-d="'+n+'" title="刪除">×</button></span>').join('');
$('regtip').textContent=j.reg.length?'點 ✎ 可改名：該人要單獨正對鏡頭、保持不動，約 1~2 秒完成。':'還沒有登記的人。重開機還原後，名單要等每個人再被辨識到一次才會出現在這裡。';
$('auto').checked=j.auto;
$('svA').textContent=j.servo+'°';$('svT').textContent='目標 '+j.stgt+'°';
if(j.thr>=0&&document.activeElement!==$('thr')){$('thr').value=j.thr;$('tv').textContent=j.thr}}
$('reg').onclick=()=>cmd('c=reg&name='+encodeURIComponent($('nm').value.trim()));
$('regs').onclick=e=>{const d=e.target.dataset.d,r=e.target.dataset.r;
 if(d){if(confirm('刪除 '+d+'？'))cmd('c=del&name='+encodeURIComponent(d))}
 else if(r){const to=prompt('把 '+r+' 改名為（英數字、- _，最長 12 字）。請先讓這個人單獨正對鏡頭並保持不動。');if(to&&to.trim())cmd('c=ren&name='+encodeURIComponent(r)+'&to='+encodeURIComponent(to.trim()))}};
$('auto').onchange=()=>cmd('c=auto&v='+($('auto').checked?1:0));
$('thr').oninput=()=>$('tv').textContent=$('thr').value;
$('ta').onclick=()=>cmd('c=thr&v='+$('thr').value);
$('svK').onclick=()=>cmd('c=sv&v=1');$('svS').onclick=()=>cmd('c=sv&v=0');
$('bk').onclick=()=>cmd('c=backup');$('rs').onclick=()=>cmd('c=restore');
$('rst').onclick=()=>{if(confirm('確定清除所有已登記的人臉？'))cmd('c=reset')};
refresh();setInterval(refresh,1000);
</script></body></html>)HTML";

// 从 "a=1&b=2" 里取出 key 对应的值（找不到回传空字串）。
void getParam(const char *query, const char *key, char *out, size_t outSize) {
  out[0] = 0;
  const size_t keyLen = strlen(key);
  const char *p = query;
  while (p && *p) {
    if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
      p += keyLen + 1;
      size_t n = 0;
      while (*p && *p != '&' && n < outSize - 1) out[n++] = *p++;
      out[n] = 0;
      return;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
}

// 一定要带 Content-Length：浏览器靠它判断内容有没有收完整。
void sendHeader(WiFiClient &c, const char *type, size_t length) {
  c.print("HTTP/1.1 200 OK\r\nContent-Type: ");
  c.print(type);
  c.print("\r\nContent-Length: ");
  c.print(static_cast<unsigned long>(length));
  c.print("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
}

// 分段送出大字串，一次送太多可能被网路堆叠截断。
void sendBig(WiFiClient &c, const char *text) {
  const size_t total = strlen(text);
  size_t sent = 0;
  while (sent < total && c.connected()) {
    const size_t n = (total - sent) > 512 ? 512 : (total - sent);
    c.write(reinterpret_cast<const uint8_t *>(text + sent), n);
    sent += n;
  }
}

void sendStatus(WiFiClient &c) {
  IPAddress ip = WiFi.localIP();
  static char json[1400];
  json[0] = 0;

  appendf(json, sizeof(json),
          "{\"url\":\"http://%u.%u.%u.%u/\",\"res\":\"%ux%u\",\"fps\":%d,\"nn\":%d,\"auto\":%s,\"thr\":%d,\"servo\":%d,\"stgt\":%d,\"msg\":\"%s\",\"faces\":[",
          ip[0], ip[1], ip[2], ip[3], configV.width(), configV.height(), CAM_FPS, NN_FPS, autoEnroll ? "true" : "false",
          threshold, servoAngle, servoTarget, lastMsg);

  const uint8_t shown = faceCount > 4 ? 4 : faceCount;
  for (uint8_t i = 0; i < shown; i++) {
    const bool known = strcmp(faceNames[i], "unknown") != 0;
    appendf(json, sizeof(json), "%s{\"n\":\"%s\",\"k\":%d}", i ? "," : "", known ? faceNames[i] : "?", known ? 1 : 0);
  }
  appendf(json, sizeof(json), "],\"reg\":[");
  for (uint8_t i = 0; i < regCount; i++) {
    appendf(json, sizeof(json), "%s\"%s\"", i ? "," : "", regNames[i]);
  }
  appendf(json, sizeof(json), "]}");

  sendHeader(c, "application/json; charset=utf-8", strlen(json));
  sendBig(c, json);
}

// 处理 /api/cmd?c=...，结果文字放进 lastMsg 再回传 {"msg":...}。
void handleCommand(WiFiClient &c, const char *query) {
  char cmd[12];
  char name[24];
  char value[8];
  char toName[24];
  getParam(query, "to", toName, sizeof(toName));
  getParam(query, "c", cmd, sizeof(cmd));
  getParam(query, "name", name, sizeof(name));
  getParam(query, "v", value, sizeof(value));

  if (strcmp(cmd, "reg") == 0) {
    enrollCurrentFace(name);
  } else if (strcmp(cmd, "sv") == 0) {
    // 测试用：直接把马达转到「认识的人」或「陌生人」的角度（下一次人脸状态改变时会被覆盖）
    servoTarget = atoi(value) ? ANGLE_KNOWN : ANGLE_STRANGER;
    appliedState = atoi(value) ? 1 : 2;
    setMsg(atoi(value) ? "伺服馬達 -> 認識的人角度" : "伺服馬達 -> 陌生人角度");
  } else if (strcmp(cmd, "ren") == 0) {
    renameFace(name, toName);
  } else if (strcmp(cmd, "del") == 0) {
    if (findReg(name) < 0) {
      setMsg("找不到這個名稱");
    } else {
      facerecog.removeFace(name);
      removeReg(name);
      backupDue = true;
      backupDueMs = millis();
      setMsg("已刪除");
    }
  } else if (strcmp(cmd, "reset") == 0) {
    facerecog.resetRegisteredFace();
    regCount = 0;
    nextId = 1;
    backupDue = true;
    backupDueMs = millis();
    setMsg("已清除全部人臉");
  } else if (strcmp(cmd, "backup") == 0) {
    facerecog.backupRegisteredFace();
    setMsg("已備份到板子");
  } else if (strcmp(cmd, "restore") == 0) {
    facerecog.restoreRegisteredFace();
    setMsg("已從板子還原（名單等被辨識到才會顯示）");
  } else if (strcmp(cmd, "auto") == 0) {
    autoEnroll = atoi(value) != 0;
    unknownSinceMs = 0;
    setMsg(autoEnroll ? "已開啟自動登記" : "已關閉自動登記");
  } else if (strcmp(cmd, "thr") == 0) {
    threshold = constrain(atoi(value), 0, 100);
    facerecog.setThreshold(threshold);
    setMsg("已套用辨識嚴格度");
  } else {
    setMsg("未知的指令");
  }

  char body[96];
  body[0] = 0;
  appendf(body, sizeof(body), "{\"msg\":\"%s\"}", lastMsg);
  sendHeader(c, "application/json; charset=utf-8", strlen(body));
  sendBig(c, body);
}

// 把 client 变成长连线（MJPEG 影像 / SSE 事件）保留下来。
// WiFiClient 的解构函式会把 socket 关掉，而且复制出来的物件跟原本的共用同一个
// socket，所以要先复制，再把「原本那个」改成非阻塞模式（解构时就不会关 socket）。
void adoptClient(WiFiClient &client, WiFiClient &slot, bool &active) {
  // 不要在这里 stop() 旧连线：旧连线多半已经被浏览器关掉，它的 socket 编号可能已经被
  // 刚接受的新连线重复使用，stop() 会把新连线一起关掉（之前画面送出去就中断就是这样）。
  // 旧的会在 pumpVideo / pumpEvents 写入失败时被清掉。
  slot = client;
  client.setNonBlockingMode();
  active = true;
}

// ---- 画面：一张一张的 JPEG 快照（/snap）----
// 之前用 multipart/x-mixed-replace 连续串流，浏览器只显示第一张就不动了（原因没查出来）；
// 改成最保险的做法：网页每收到一张就再要下一张，每张都是一个独立的、带 Content-Length 的
// 一般 HTTP 回应，跟 /api/status 同一条路，浏览器一定看得懂。
// Camera.getImage() 在 snapshot 模式下每次呼叫都会拍一张新的（等到画面送达才返回）。
uint32_t snapCount = 0;
uint32_t snapFail = 0;
uint32_t lastSnapLen = 0;
uint32_t lastSnapMs = 0;

// 一次 write 太大一包可能整个送不出去，所以切成小段送；
// 任何一段失败就回传 false。
bool writeAll(WiFiClient &client, const uint8_t *buf, uint32_t len) {
  const uint32_t SLICE = 1400;
  while (len > 0) {
    const uint32_t n = len > SLICE ? SLICE : len;
    if (client.write(buf, n) == 0) return false;
    buf += n;
    len -= n;
  }
  return true;
}

void serveSnapshot(WiFiClient &client) {
  uint32_t imageAddress = 0;
  uint32_t imageLength = 0;
  Camera.getImage(CHANNEL, &imageAddress, &imageLength);
  if (imageAddress == 0 || imageLength == 0) {
    snapFail++;
    client.print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n");
    return;
  }
  char header[160];
  snprintf(header, sizeof(header),
           "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
           (unsigned long)imageLength);
  if (writeAll(client, (const uint8_t *)header, strlen(header)) && writeAll(client, (const uint8_t *)imageAddress, imageLength)) {
    snapCount++;
    lastSnapLen = imageLength;
    lastSnapMs = millis();
  } else {
    snapFail++;
  }
}

// 每 5 秒在序列埠印一次统计，网页没画面时靠这行判断是「相机没出图」还是「传输出问题」。
void printVideoStats() {
  static uint32_t lastReportMs = 0;
  if (millis() - lastReportMs < 5000) return;
  lastReportMs = millis();
  Serial.print("[VIDEO] 已送出 ");
  Serial.print(snapCount);
  Serial.print(" 张，最后一张 ");
  Serial.print(lastSnapLen);
  Serial.print(" bytes，失败 ");
  Serial.print(snapFail);
  Serial.println(" 次");
}

void startEventStream(WiFiClient &client) {
  client.print("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n");
  client.print("retry: 1000\n\n");
}

bool hadRequest = false;  // 这次 accept 到的连线有没有真的送来请求（没有人连时 accept 会回传空连线）

void handleClient(WiFiClient &client, bool &adopted) {
  adopted = false;
  hadRequest = false;
  // 读第一行（"GET /path?query HTTP/1.1"），再把剩下的 header 读掉直到空行；
  // 最多等 1.5 秒，避免连上来却不说话的客户端卡住主迴圈。
  char line[160];
  size_t len = 0;
  bool first = true;
  char request[160] = "";
  const uint32_t startMs = millis();
  while (client.connected() && millis() - startMs < 1500) {
    if (!client.available()) continue;
    const char ch = client.read();
    if (ch == '\n') {
      if (len == 0) break;  // 空行：header 结束
      line[len] = 0;
      if (first) {
        snprintf(request, sizeof(request), "%s", line);
        first = false;
      }
      len = 0;
    } else if (ch != '\r' && len < sizeof(line) - 1) {
      line[len++] = ch;
    }
  }

  if (request[0]) hadRequest = true;
  // request 形如 "GET /api/cmd?c=reg HTTP/1.1"
  char *path = strchr(request, ' ');
  if (!path) return;
  path++;
  char *end = strchr(path, ' ');
  if (end) *end = 0;
  char *query = strchr(path, '?');
  if (query) *query++ = 0;
  if (strcmp(path, "/snap") != 0 && strcmp(path, "/api/status") != 0 && strcmp(path, "/video") != 0) {
    Serial.print("[HTTP] ");
    Serial.println(request);
  }

  if (strcmp(path, "/") == 0) {
    sendHeader(client, "text/html; charset=utf-8", strlen(PAGE));
    sendBig(client, PAGE);
  } else if (strcmp(path, "/snap") == 0 || strcmp(path, "/video") == 0) {
    serveSnapshot(client);
  } else if (strcmp(path, "/events") == 0) {
    startEventStream(client);
    adoptClient(client, eventClient, eventActive);
    adopted = true;
  } else if (strcmp(path, "/api/status") == 0) {
    sendStatus(client);
  } else if (strcmp(path, "/api/cmd") == 0) {
    handleCommand(client, query ? query : "");
  } else {
    client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
  }
}

// NN 每处理完一帧，就把人脸座标 / 识别结果推给网页（SSE）；没新结果时每 2 秒送一个
// 心跳，让浏览器和中间的路由器知道连线还活着。
void pumpEvents() {
  if (!eventActive) return;
  if (!eventClient.connected()) {
    eventClient.stop();
    eventActive = false;
    return;
  }
  static uint32_t lastSeq = 0;
  static uint32_t lastSendMs = 0;

  if (faceSeq != lastSeq) {
    lastSeq = faceSeq;
    char msg[360];
    msg[0] = 0;
    appendf(msg, sizeof(msg), "data: {\"f\":[");
    const uint8_t shown = faceCount > 4 ? 4 : faceCount;
    for (uint8_t i = 0; i < shown; i++) {
      const bool known = strcmp(faceNames[i], "unknown") != 0;
      appendf(msg, sizeof(msg), "%s[%d,\"%s\",%d,%d,%d,%d]", i ? "," : "", known ? 1 : 0, known ? faceNames[i] : "?",
              faceBox[i][0], faceBox[i][1], faceBox[i][2], faceBox[i][3]);
    }
    appendf(msg, sizeof(msg), "]}\n\n");
    eventClient.print(msg);
    lastSendMs = millis();
  } else if (millis() - lastSendMs > 2000) {
    eventClient.print(": hb\n\n");
    lastSendMs = millis();
  }
}

// ---------------------------------------------------------------- 绘图

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

// WiFi 讯号格数 0~4：依 RSSI 分级，没连线就是 0。
uint8_t wifiBars() {
  if (!wifiConnected()) return 0;
  const long rssi = WiFi.RSSI();
  if (rssi > -55) return 4;
  if (rssi > -65) return 3;
  if (rssi > -75) return 2;
  if (rssi > -85) return 1;
  return 0;
}

// 左上角讯号格：4 根由矮到高的长条；没讯号的只留底下一个点，完全没连线加小叉。
// 画在反白标题列上，所以用黑色画。
void drawWifiIcon(int16_t x, int16_t baseY) {
  const uint8_t bars = wifiBars();
  display.setDrawColor(0);
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t h = 3 + i * 2;
    if (i < bars) {
      display.drawBox(x + i * 4, baseY - h + 1, 3, h);
    } else {
      display.drawPixel(x + i * 4 + 1, baseY);
    }
  }
  if (!wifiConnected()) {
    display.drawLine(x + 9, baseY - 5, x + 13, baseY - 1);
    display.drawLine(x + 9, baseY - 1, x + 13, baseY - 5);
  }
  display.setDrawColor(1);
}

// 反白标题列：左边 WiFi 讯号格，中间标题，右边有网页在看画面时会闪烁的圆点（LIVE）。
void drawTitleBar(const char *title, bool live) {
  display.drawBox(0, 0, 128, 14);
  drawWifiIcon(3, 11);

  display.setDrawColor(0);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8((128 - display.getUTF8Width(title)) / 2, 11, title);

  if (live) {
    if ((millis() / 500) % 2 == 0) {
      display.drawDisc(118, 7, 3);
    } else {
      display.drawCircle(118, 7, 3);
    }
  }
  display.setDrawColor(1);
}

// 人脸图示：lit 为 true 画实心脸（侦测到），否则只画空心外框；
// known 的脸画笑嘴，陌生人画一条直线嘴。
void drawFaceIcon(int16_t cx, int16_t cy, bool lit, bool known) {
  if (!lit) {
    display.drawCircle(cx, cy, 6);
    return;
  }
  display.drawDisc(cx, cy, 6);
  display.setDrawColor(0);
  display.drawPixel(cx - 2, cy - 2);
  display.drawPixel(cx + 2, cy - 2);
  if (known) {
    display.drawPixel(cx - 2, cy + 1);
    display.drawHLine(cx - 1, cy + 2, 3);
    display.drawPixel(cx + 2, cy + 1);
  } else {
    display.drawHLine(cx - 2, cy + 2, 5);
  }
  display.setDrawColor(1);
}

void showStatus(const char *line1, const char *line2) {
  display.clearBuffer();
  drawTitleBar("人脸识别", false);

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8((128 - display.getUTF8Width(line1)) / 2, 36, line1);
  if (line2) {
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr((128 - display.getStrWidth(line2)) / 2, 54, line2);
  }
  display.sendBuffer();
}

// 主画面：人脸数大字 + 识别到的代号 + 已登记人数 + 人脸图示 + 网址。
void showMainScreen() {
  const uint8_t count = faceCount;

  display.clearBuffer();
  drawTitleBar("人脸识别", (millis() - lastSnapMs < 2000));

  char countText[6];
  snprintf(countText, sizeof(countText), "%u", count);
  display.setFont(u8g2_font_logisoso24_tn);
  display.drawStr(8, 44, countText);

  display.setFont(u8g2_font_wqy12_t_gb2312);
  if (count > 0) {
    // 第一张脸的识别结果：已登记就显示代号，否则「陌生人」
    const bool known = strcmp(faceNames[0], "unknown") != 0;
    char line[24];
    snprintf(line, sizeof(line), "%.12s", known ? faceNames[0] : "陌生人");
    display.drawUTF8(36, 30, line);
  } else {
    display.drawUTF8(36, 30, "未侦测到");
  }
  char regText[24];
  snprintf(regText, sizeof(regText), "舵机 %d度", servoAngle);
  display.drawUTF8(36, 44, regText);

  // 右边 2x2 个人脸图示：侦测到几个就亮几个，已登记的有笑脸
  for (uint8_t i = 0; i < 4; i++) {
    const bool lit = i < count;
    const bool known = lit && strcmp(faceNames[i], "unknown") != 0;
    drawFaceIcon(100 + (i % 2) * 17, 24 + (i / 2) * 17, lit, known);
  }

  display.drawHLine(4, 49, 120);
  IPAddress ip = WiFi.localIP();
  char ipText[28];
  snprintf(ipText, sizeof(ipText), "http://%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  display.setFont(u8g2_font_6x10_tr);
  display.drawStr((128 - display.getStrWidth(ipText)) / 2, 61, ipText);

  display.sendBuffer();
}

// ---------------------------------------------------------------- 连线与启动

void connectWifi() {
  char footer[24];
  for (uint16_t attempt = 1; !wifiConnected(); attempt++) {
    Serial.print("[WiFi] 连线 ");
    Serial.print(WIFI_SSID);
    Serial.print("（第 ");
    Serial.print(attempt);
    Serial.println(" 次）");
    WiFi.begin(const_cast<char *>(WIFI_SSID), const_cast<char *>(WIFI_PASS));

    snprintf(footer, sizeof(footer), "%.12s  #%u", WIFI_SSID, attempt);
    const uint32_t startMs = millis();
    while (!wifiConnected() && millis() - startMs < 8000) {
      showStatus("连接 WiFi...", footer);
      delay(200);
    }
  }
  Serial.print("[WiFi] 已连线，IP = ");
  Serial.println(WiFi.localIP());
}

void printInfo() {
  Serial.println("------------------------------");
  Serial.println("- 网页控制台（画面 + 控制）-");
  Serial.print("http://");
  Serial.println(WiFi.localIP());
  Serial.println("------------------------------");
  Camera.printInfo();
}

void startCamera() {
  showStatus("启动相机与 AI...", NULL);

  Camera.configVideoChannel(CHANNEL, configV);
  Camera.configVideoChannel(CHANNELNN, configNN);
  Camera.videoInit();

  facerecog.configVideo(configNN);
  facerecog.modelSelect(FACE_RECOGNITION, NA_MODEL, DEFAULT_SCRFD, DEFAULT_MOBILEFACENET);
  facerecog.begin();
  facerecog.setResultCallback(faceCallback);
  facerecog.restoreRegisteredFace();  // 还原之前备份的人脸（没有备份就什么都不会发生）

  Camera.channelBegin(CHANNEL);  // JPEG 通道：网页用 Camera.getImage() 取画面

  videoStreamerNN.registerInput(Camera.getStream(CHANNELNN));
  videoStreamerNN.setStackSize();
  videoStreamerNN.setTaskPriority();
  videoStreamerNN.registerOutput(facerecog);
  if (videoStreamerNN.begin() != 0) {
    Serial.println("NN StreamIO link start failed");
  }
  Camera.channelBegin(CHANNELNN);

  delay(500);
  printInfo();
}

// FUNC 键按下（高 -> 低的瞬间）回传 true，带简单防弹跳。
bool funcPressed() {
  static bool wasHigh = true;
  const bool high = (digitalRead(FUNC_PIN) == HIGH);
  const bool pressed = wasHigh && !high;
  wasHigh = high;
  if (pressed) delay(20);
  return pressed;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("15 人脸识别 + MJPEG 网页画面 + 控制台 启动");

  pinMode(FUNC_PIN, INPUT_PULLUP);
  servo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_US_AT_180);
  servoWrite(servoAngle);  // 开机先到 0 度
  for (uint8_t i = 0; i < 4; i++) {
    snprintf(faceNames[i], NAME_LEN, "unknown");
    faceBox[i][0] = faceBox[i][1] = faceBox[i][2] = faceBox[i][3] = 0;
  }

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  showStatus("启动中", NULL);
  connectWifi();
  startCamera();
  server.begin();
}

void loop() {
  if (!wifiConnected()) {
    Serial.println("[WiFi] 断线，重新连线");
    connectWifi();
    server.begin();
    printInfo();  // IP 可能变了
  }

  // 新的连线：网页 / API 请求处理完就关；影像、事件是长连线，留下来
  WiFiClient client = server.available();
  if (client) {
    bool adopted = false;
    handleClient(client, adopted);
    if (!adopted) {
      if (hadRequest) delay(20);  // 让最后的资料有时间送出去，再关连线
      client.stop();
    }
  }

  printVideoStats();
  updateServo();
  pumpEvents();

  // FUNC 键 = 登记画面中的人脸
  if (funcPressed()) {
    Serial.println("[FUNC] 登记目前画面中的人脸");
    enrollCurrentFace("");
  }

  // 回呼函式要求的自动登记
  if (enrollRequest) {
    enrollRequest = false;
    if (autoEnroll) enrollCurrentFace("");
  }

  // 登记后一直没被认出来（脸移开了、或认不出来），不要永远卡在「处理中」
  if (pendingName[0] != 0 && millis() - pendingSinceMs > PENDING_TIMEOUT_MS) {
    Serial.print("[FACE] 登记逾时：");
    Serial.println(pendingName);
    if (pendingFallback[0] != 0) {
      // 改名没成功：用旧名字把这张脸登记回去
      facerecog.registerFace(pendingFallback);
      snprintf(pendingName, sizeof(pendingName), "%s", pendingFallback);
      pendingFallback[0] = 0;
      pendingSinceMs = millis();
      setMsg("改名失敗，正在還原成舊名稱…");
    } else {
      pendingName[0] = 0;
      setMsg("登記逾時，請讓臉正對鏡頭再試一次");
    }
  }

  // 登记 / 删除后稍等一下再备份到快闪记忆体，避免连续操作时反复写入
  if (backupDue && millis() - backupDueMs > BACKUP_DELAY_MS) {
    backupDue = false;
    facerecog.backupRegisteredFace();
    Serial.println("[FACE] 已自动备份到快闪记忆体");
  }

  // 超过 1.5 秒没再看到人脸，把数字归零（万一 NN 暂停，OLED 不会停在旧的人脸数上）
  if (faceCount > 0 && millis() - lastFaceMs > 1500) {
    faceCount = 0;
    unknownSinceMs = 0;
  }

  static uint32_t lastDrawMs = 0;
  if (millis() - lastDrawMs >= 250) {
    lastDrawMs = millis();
    showMainScreen();
  }
  delay(2);
}

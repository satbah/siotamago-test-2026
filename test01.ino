#include <Arduino.h>
#include <Wire.h>

// Fallback test pins in case board-specific macros are unavailable.
#ifndef TEST_LED_PIN
  #ifdef LED_BUILTIN
    #define TEST_LED_PIN LED_BUILTIN
  #else
    #define TEST_LED_PIN 13
  #endif
#endif

// ---------- BG96 pin fallbacks (defined in nectis_nrf52840/variant.h) ----------
#ifndef MODULE_PWR_PIN
  #define MODULE_PWR_PIN    36
#endif
#ifndef MODULE_PWRKEY_PIN
  #define MODULE_PWRKEY_PIN 42
#endif
#ifndef MODULE_RESET_PIN
  #define MODULE_RESET_PIN  43
#endif
#ifndef MODULE_STATUS_PIN
  #define MODULE_STATUS_PIN 37
#endif
#ifndef MODULE_DTR_PIN
  #define MODULE_DTR_PIN    44
#endif

// ---------- GROVE pin fallbacks (defined in nectis_nrf52840/variant.h) ----------
// GROVEのVCCはBG96電源レール（MODULE_PWR_PIN）から生成されるため、
// GROVE使用前に MODULE_PWR_PIN → GROVE_VCCB_PIN の順でHIGHにする必要がある
#ifndef GROVE_VCCB_PIN
  #define GROVE_VCCB_PIN    25
#endif

#define BG96_UART         Serial1
#define BG96_BAUD         115200
#define BG96_STATUS_READY HIGH   // STATUS pin goes HIGH when BG96 is on

#define SORACOM_APN  "soracom.io"
#define SORACOM_USER "sora"
#define SORACOM_PASS "sora"

#ifdef PIN_BUTTON1
  #define TEST_BUTTON_PIN PIN_BUTTON1
#endif

#ifdef PIN_VBAT
  #define TEST_ANALOG_PIN PIN_VBAT
#elif defined(A0)
  #define TEST_ANALOG_PIN A0
#endif

unsigned long lastHeartbeatMs = 0;
static bool bg96UartReady = false;

// BG96_UARTを初回だけ初期化（再initによるUARTリセットを防ぐ）
static void ensureBG96Uart() {
  if (!bg96UartReady) {
    BG96_UART.begin(BG96_BAUD);
    bg96UartReady = true;
    delay(300);
    while (BG96_UART.available()) BG96_UART.read(); // 受信バッファをクリア
  }
}

// =============================================================================
// BG96 helpers
// =============================================================================

// BG96 UARTを無音になるまでドレイン（最大waitMs、silenceMs無音で抜ける）
static void drainBG96(unsigned long waitMs = 5000, unsigned long silenceMs = 500) {
  unsigned long lastRx = millis();
  unsigned long start  = millis();
  while (millis() - start < waitMs) {
    if (BG96_UART.available()) {
      BG96_UART.read();
      lastRx = millis();
    } else if (millis() - lastRx >= silenceMs) {
      break;
    }
  }
}

// APP RDYをUARTから待つ（最大waitMs）
static void bg96WaitAppRdy(unsigned long waitMs = 15000) {
  Serial.print("[BG96] waiting for APP RDY");
  String boot = "";
  unsigned long t = millis();
  while (millis() - t < waitMs) {
    while (BG96_UART.available()) boot += (char)BG96_UART.read();
    if (boot.indexOf("APP RDY") >= 0) { Serial.println(" OK"); return; }
    delay(200);
  }
  Serial.println(" TIMEOUT (continuing)");
}

// BG96電源オフ（PWRKEY長押し）→ STATUS LOW確認
static void bg96PowerOff() {
  Serial.println("[BG96] power off (PWRKEY hold)");
  drainBG96(500, 200);
  digitalWrite(MODULE_PWRKEY_PIN, HIGH);
  delay(800);                            // BG96仕様: >650ms でパワーオフ
  digitalWrite(MODULE_PWRKEY_PIN, LOW);
  // STATUSがLOWになるまで最大10秒待つ
  Serial.print("[BG96] waiting for STATUS LOW");
  unsigned long t = millis();
  while (digitalRead(MODULE_STATUS_PIN) == BG96_STATUS_READY) {
    if (millis() - t > 10000) { Serial.println(" TIMEOUT"); return; }
    delay(300);
    Serial.print(".");
  }
  Serial.println(" OK");
  delay(500);
}

// BG96パワーサイクル（電源OFF→ON）による完全リセット
// 戻り値: 電源ON成功ならtrue
static bool bg96PowerCycle() {
  Serial.println("[BG96] power cycle for clean state");
  bg96PowerOff();
  return bg96PowerOn();
}

// BG96電源ONシーケンス（NectisMcu.cpp の PowerSupplyCellular() 準拠）
// 戻り値: STATUSがREADYになればtrue、タイムアウトでfalse
static bool bg96PowerOn() {
  pinMode(MODULE_PWR_PIN,    OUTPUT); digitalWrite(MODULE_PWR_PIN,    LOW);
  pinMode(MODULE_PWRKEY_PIN, OUTPUT); digitalWrite(MODULE_PWRKEY_PIN, LOW);
  pinMode(MODULE_RESET_PIN,  OUTPUT); digitalWrite(MODULE_RESET_PIN,  LOW);
  pinMode(MODULE_DTR_PIN,    OUTPUT); digitalWrite(MODULE_DTR_PIN,    LOW);
  pinMode(MODULE_STATUS_PIN, INPUT_PULLUP);

  // UARTをPWRKEY前に開始（APP RDYはSTATUS HIGHより早く来ることがある）
  ensureBG96Uart();

  Serial.println("[BG96] power supply ON");
  digitalWrite(MODULE_PWR_PIN, HIGH);
  delay(200);
  digitalWrite(MODULE_PWRKEY_PIN, HIGH);
  delay(600);
  digitalWrite(MODULE_PWRKEY_PIN, LOW);

  // STATUSピンがHIGHになるまで最大15秒待つ
  Serial.print("[BG96] waiting for STATUS");
  unsigned long t = millis();
  while (digitalRead(MODULE_STATUS_PIN) != BG96_STATUS_READY) {
    if (millis() - t > 15000) { Serial.println(" TIMEOUT"); return false; }
    delay(500);
    Serial.print(".");
  }
  Serial.println(" READY");
  bg96WaitAppRdy(15000);
  return true;
}

// BG96初期化：起動済みならパワーサイクル、未起動なら電源ON
// 戻り値: 初期化成功ならtrue、電源ONタイムアウトならfalse
static bool bg96Init() {
  pinMode(MODULE_STATUS_PIN, INPUT_PULLUP);
  pinMode(MODULE_RESET_PIN,  OUTPUT); digitalWrite(MODULE_RESET_PIN, LOW);
  ensureBG96Uart();

  if (digitalRead(MODULE_STATUS_PIN) == BG96_STATUS_READY) {
    Serial.println("[BG96] already on, performing power cycle for clean state");
    return bg96PowerCycle();
  } else {
    return bg96PowerOn();
  }
}

// ATコマンドを送信してレスポンスを返す（戻り値: レスポンス文字列にexpectが含まれていればtrue）
static bool sendAT(const char* cmd, const char* expect = "OK", unsigned long timeoutMs = 5000) {
  // 受信バッファをクリア
  while (BG96_UART.available()) BG96_UART.read();

  Serial.print("[BG96] >> "); Serial.println(cmd);
  BG96_UART.print(cmd);
  BG96_UART.print("\r\n");

  String resp = "";
  unsigned long t = millis();
  while (millis() - t < timeoutMs) {
    while (BG96_UART.available()) {
      char c = (char)BG96_UART.read();
      resp += c;
    }
    if (resp.indexOf(expect) >= 0) break;
    if (resp.indexOf("ERROR") >= 0) break;
  }

  // レスポンスを表示（改行を見やすく整形）
  resp.trim();
  if (resp.length() > 0) {
    Serial.print("[BG96] << "); Serial.println(resp);
  } else {
    Serial.println("[BG96] << (no response)");
  }

  return resp.indexOf(expect) >= 0;
}

// =============================================================================
// BG96 テスト 3ステップ
// =============================================================================

void testBG96All() {
  Serial.println();
  Serial.println("=== BG96 Test: Step 1 - UART & AT ===");

  if (!bg96Init()) {
    Serial.println("=== BG96 Test ABORTED (init failed: power on timeout) ===");
    return;
  }

  // エコーOFF
  sendAT("ATE0", "OK", 3000);

  bool step1 = sendAT("AT", "OK", 3000);
  sendAT("ATI", "OK", 3000);               // モジュール情報
  sendAT("AT+CPIN?", "READY", 5000);       // SIM状態

  Serial.print("[BG96] Step1 (AT): ");
  Serial.println(step1 ? "PASS" : "FAIL");
  if (!step1) {
    Serial.println("=== BG96 Test ABORTED (Step1 failed) ===");
    return;
  }

  // ------------------------------------------------------------------
  Serial.println();
  Serial.println("=== BG96 Test: Step 2 - Network Registration ===");

  // LTE登録確認（+CEREG: 0,1 or 0,5 が成功）
  // 最大60秒待つ
  bool registered = false;
  Serial.print("[BG96] waiting for LTE registration");
  for (int i = 0; i < 12; i++) {
    while (BG96_UART.available()) BG96_UART.read();
    BG96_UART.print("AT+CEREG?\r\n");
    String r = "";
    unsigned long t = millis();
    while (millis() - t < 3000) {
      while (BG96_UART.available()) r += (char)BG96_UART.read();
      if (r.indexOf("OK") >= 0) break;
    }
    r.trim();
    Serial.print("[BG96] << "); Serial.println(r);
    // CEREG: 0,1 (home) または 0,5 (roaming)
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { registered = true; break; }
    delay(5000);
    Serial.print(".");
  }
  Serial.println();

  sendAT("AT+CSQ", "OK", 3000);            // 電波強度 (0-31, 99=不明)
  sendAT("AT+QCSQ", "OK", 3000);          // RSSI/RSRP/SINR/RSRQ (dBm)
  sendAT("AT+COPS?", "OK", 3000);          // オペレータ
  sendAT("AT+QCFG=\"band\"", "OK", 3000); // バンド設定 (GSM/LTE-M/NB-IoT)

  Serial.print("[BG96] Step2 (Network): ");
  Serial.println(registered ? "PASS" : "FAIL");
  if (!registered) {
    Serial.println("=== BG96 Test ABORTED (Step2 failed) ===");
    return;
  }

  // ------------------------------------------------------------------
  Serial.println();
  Serial.println("=== BG96 Test: Step 3 - Internet (SORACOM) ===");

  // PDPコンテキスト設定
  sendAT("AT+QICSGP=1,1,\"" SORACOM_APN "\",\"" SORACOM_USER "\",\"" SORACOM_PASS "\",1", "OK", 5000);
  // PDP起動（最大150秒）。既にアクティブな場合はERRORが返るので、その後QIACTで確認する
  bool pdp = sendAT("AT+QIACT=1", "OK", 150000);
  // 割り当てIPアドレス確認（QIACT=1がERRORでも既にアクティブなら+QIACTにIPが入る）
  String qiactResp = "";
  {
    while (BG96_UART.available()) BG96_UART.read();
    BG96_UART.print("AT+QIACT?\r\n");
    unsigned long t = millis();
    while (millis() - t < 5000) {
      while (BG96_UART.available()) qiactResp += (char)BG96_UART.read();
      if (qiactResp.indexOf("OK") >= 0) break;
    }
    qiactResp.trim();
    Serial.print("[BG96] << "); Serial.println(qiactResp);
    // IPアドレスが含まれていれば実質アクティブ成功
    if (!pdp && qiactResp.indexOf("+QIACT: 1,1,1,") >= 0) {
      Serial.println("[BG96] PDP already active, treating as OK");
      pdp = true;
    }
  }

  // ping (8.8.8.8 に1回)
  bool ping = sendAT("AT+QPING=1,\"8.8.8.8\",1,1", "+QPING: 0", 10000);
  drainBG96(2000, 500);

  Serial.print("[BG96] Step3 (Internet): ");
  Serial.println((pdp && ping) ? "PASS" : "FAIL");
  if (!pdp || !ping) {
    Serial.println("=== BG96 Test ABORTED (Step3 failed) ===");
    return;
  }

  // ------------------------------------------------------------------
  Serial.println();
  Serial.println("=== BG96 Test: Step 4 - HTTP (d-and.jp) ===");

  // HTTP GET http://d-and.jp （200/301/302/701 全てPASS）
  bool httpOk = bg96HttpGet("http://d-and.jp");
  Serial.print("[BG96] Step4 (HTTP): ");
  Serial.println(httpOk ? "PASS" : "FAIL");

  Serial.println();
  Serial.println("=== BG96 Test Done ===");
  Serial.print("  Step1 (AT):      "); Serial.println(step1       ? "PASS" : "FAIL");
  Serial.print("  Step2 (Network): "); Serial.println(registered  ? "PASS" : "FAIL");
  Serial.print("  Step3 (Internet):"); Serial.println((pdp&&ping) ? "PASS" : "FAIL");
  Serial.print("  Step4 (HTTP):    "); Serial.println(httpOk      ? "PASS" : "FAIL");
  Serial.println();
}

#define HTTP_TEST_URL "http://metadata.soracom.io/v1/subscriber"

// HTTP GETを実行してレスポンス本文を表示。200/301/302はPASS
static bool bg96HttpGet(const char* url) {
  sendAT("AT+QHTTPCFG=\"contextid\",1", "OK", 5000);

  int urlLen = strlen(url);
  char urlCmd[64];
  snprintf(urlCmd, sizeof(urlCmd), "AT+QHTTPURL=%d,80", urlLen);
  while (BG96_UART.available()) BG96_UART.read();
  BG96_UART.print(urlCmd);
  BG96_UART.print("\r\n");
  Serial.print("[BG96] >> "); Serial.println(urlCmd);
  String connResp = "";
  unsigned long t0 = millis();
  while (millis() - t0 < 5000) {
    while (BG96_UART.available()) connResp += (char)BG96_UART.read();
    if (connResp.indexOf("CONNECT") >= 0) break;
  }
  connResp.trim();
  Serial.print("[BG96] << "); Serial.println(connResp);
  BG96_UART.print(url);
  Serial.print("[BG96] >> (url) "); Serial.println(url);
  delay(500);

  // GET実行（200/301/302 全てOK）
  while (BG96_UART.available()) BG96_UART.read();
  BG96_UART.print("AT+QHTTPGET=80\r\n");
  Serial.println("[BG96] >> AT+QHTTPGET=80");
  String getResp = "";
  unsigned long t1 = millis();
  while (millis() - t1 < 80000) {
    while (BG96_UART.available()) getResp += (char)BG96_UART.read();
    // +QHTTPGET: の行全体（改行まで）が揃ってからbreak
    int qhttpIdx = getResp.indexOf("+QHTTPGET:");
    if ((qhttpIdx >= 0 && getResp.indexOf('\n', qhttpIdx) >= 0) ||
        getResp.indexOf("ERROR") >= 0) break;
  }
  getResp.trim();
  Serial.print("[BG96] << "); Serial.println(getResp);
  // +QHTTPGET: 0,2xx / 0,3xx → PASS（正常応答・リダイレクト）
  // +QHTTPGET: 701 → PASS（SSL未対応リダイレクト。TCPでサーバー到達済み）
  bool got = getResp.indexOf("+QHTTPGET: 0,2") >= 0 ||
             getResp.indexOf("+QHTTPGET: 0,3") >= 0 ||
             getResp.indexOf("+QHTTPGET: 701") >= 0;
  if (!got) { Serial.println("[BG96] HTTP GET: FAIL"); return false; }

  // レスポンス読み出し（3xxの場合はボディなしでもOK）
  while (BG96_UART.available()) BG96_UART.read();
  BG96_UART.print("AT+QHTTPREAD=80\r\n");
  Serial.println("[BG96] >> AT+QHTTPREAD=80");
  String body = "";
  unsigned long t2 = millis();
  while (millis() - t2 < 30000) {
    while (BG96_UART.available()) body += (char)BG96_UART.read();
    if (body.indexOf("OK") >= 0 || body.indexOf("ERROR") >= 0) break;
  }
  body.trim();
  Serial.println("[BG96] --- HTTP Response ---");
  Serial.println(body);
  Serial.println("[BG96] -----------------------");
  return true;
}

void testBG96HttpGet() {
  Serial.println();
  Serial.println("=== BG96 HTTP GET Test ===");

  // Serial1未初期化だとsendAT内のBG96_UART.available()でハングするため必ず初期化
  ensureBG96Uart();

  // PDPコンテキスト設定・起動（既にアクティブでも続行）
  sendAT("AT+QICSGP=1,1,\"" SORACOM_APN "\",\"" SORACOM_USER "\",\"" SORACOM_PASS "\",1", "OK", 5000);
  bool pdp = sendAT("AT+QIACT=1", "OK", 150000);
  if (!pdp) {
    String r = "";
    while (BG96_UART.available()) BG96_UART.read();
    BG96_UART.print("AT+QIACT?\r\n");
    unsigned long t = millis();
    while (millis() - t < 5000) {
      while (BG96_UART.available()) r += (char)BG96_UART.read();
      if (r.indexOf("OK") >= 0) break;
    }
    r.trim();
    Serial.print("[BG96] << "); Serial.println(r);
    if (r.indexOf("+QIACT: 1,1,1,") < 0) {
      Serial.println("[BG96] HTTP GET: FAIL (no PDP context)");
      return;
    }
    Serial.println("[BG96] PDP already active, continuing");
  }

  bool pass = bg96HttpGet(HTTP_TEST_URL);
  Serial.print("[BG96] HTTP GET: ");
  Serial.println(pass ? "PASS" : "FAIL");
  Serial.println();
}

// =============================================================================
// BG96 PSM（省電力モード）テスト
// TAU=1分, ActiveTime=10秒 で動作確認
// =============================================================================

void testBG96PSM() {
  Serial.println();
  Serial.println("=== BG96 Test: PSM (Sleep/Wake) ===");
  ensureBG96Uart();

  // DTRパルスで強制起動（NVMにPSMが残っていてスリープ中の場合に備える）
  Serial.println("[PSM] DTR pulse to wake BG96...");
  pinMode(MODULE_DTR_PIN, OUTPUT);
  digitalWrite(MODULE_DTR_PIN, LOW);
  delay(100);
  digitalWrite(MODULE_DTR_PIN, HIGH);
  delay(2000);

  // 前テストの残留URC（QPING/CME ERROR等）を吐き出してから開始
  drainBG96(3000, 500);

  // 疎通確認（最大3回リトライ）
  bool atOk = false;
  for (int i = 0; i < 3 && !atOk; i++) {
    atOk = sendAT("AT", "OK", 2000);
    if (!atOk) delay(1000);
  }
  if (!atOk) {
    Serial.println("[PSM] FAIL: BG96 not responding even after DTR");
    return;
  }
  Serial.println("[PSM] BG96 responding OK");

  // PSM有効化
  // T3412 "10100001": unit=101(1min) * value=00001(1) = 1分
  // T3324 "00000101": unit=000(2s)   * value=00101(5) = 10秒
  if (!sendAT("AT+CPSMS=1,,,\"10100001\",\"00000101\"", "OK", 5000)) {
    Serial.println("[PSM] FAIL: could not enable PSM");
    return;
  }
  sendAT("AT+CPSMS?", "OK", 3000);

  // ネットワークが実際に割り当てたTAU/ActiveTimeを確認
  // AT+CEREG=4: 拡張通知モード（PSMタイマー情報含む）
  sendAT("AT+CEREG=4", "OK", 3000);
  sendAT("AT+CEREG?", "OK", 3000);
  // レスポンス例: +CEREG: 4,1,"xxxx","xxxxxxxx",8,0,0,"T3412value","T3324value"
  // T3412(TAU)とT3324(ActiveTime)がバイナリ文字列で返る
  // 終了後は通知モードを戻す
  sendAT("AT+CEREG=0", "OK", 3000);

  // ActiveTime(10s)+余裕(10s) 待ってPSM突入を確認
  Serial.println("[PSM] Waiting for PSM entry (~20s)...");
  delay(20000);

  // AT無応答ならスリープ中
  bool asleep = !sendAT("AT", "OK", 1500);
  Serial.print("[PSM] Sleep check: ");
  Serial.println(asleep ? "sleeping (no AT response)" : "still awake");

  // TAU(1分)経過後の自律復帰をポーリング（最大90秒）
  Serial.println("[PSM] Waiting for TAU wake (~60s)...");
  bool woke = false;
  unsigned long t = millis();
  while (millis() - t < 90000) {
    if (sendAT("AT", "OK", 2000)) { woke = true; break; }
    delay(3000);
    Serial.print(".");
  }
  Serial.println();

  // TAU復帰を検出できなかった場合はDTRパルスで強制起動
  if (!woke) {
    Serial.println("[PSM] TAU wake not detected, trying DTR pulse...");
    digitalWrite(MODULE_DTR_PIN, LOW);
    delay(100);
    digitalWrite(MODULE_DTR_PIN, HIGH);
    delay(2000); // BG96が応答できるまで待つ
    woke = sendAT("AT", "OK", 3000);
    Serial.print("[PSM] DTR wake: ");
    Serial.println(woke ? "OK" : "FAIL");
  } else {
    Serial.println("[PSM] Wake: TAU (autonomous)");
  }

  if (!woke) {
    sendAT("AT+CPSMS=0", "OK", 3000);
    return;
  }

  // 再接続確認
  sendAT("AT+CEREG?", "OK", 3000);

  // HTTP GET で通信確認
  bool httpPass = bg96HttpGet(HTTP_TEST_URL);
  Serial.print("[PSM] HTTP GET after wake: ");
  Serial.println(httpPass ? "PASS" : "FAIL");

  // PSM無効化（後続テストへの影響を避ける）
  sendAT("AT+CPSMS=0", "OK", 3000);
  Serial.println("[PSM] PSM disabled");
  Serial.println("=== BG96 PSM Test Done ===");
}

// =============================================================================
// BG96 eDRX テスト
// LTE-M (Cat-M1), サイクル約20.48秒
// =============================================================================

void testBG96Edrx() {
  Serial.println();
  Serial.println("=== BG96 Test: eDRX ===");
  ensureBG96Uart();

  // eDRX有効化: mode=2(有効+URC), AcT=4(LTE-M), cycle="0010"(~20.48s)
  if (!sendAT("AT+CEDRXS=2,4,\"0010\"", "OK", 5000)) {
    Serial.println("[eDRX] FAIL: could not enable eDRX");
    return;
  }

  // ネットワークネゴシエーション後の実際のパラメータを確認
  sendAT("AT+CEDRXRDP", "OK", 3000);

  // eDRX中も接続維持されているか確認（30秒間、10秒ごとに3回AT確認）
  Serial.println("[eDRX] Checking connectivity during eDRX (30s)...");
  int okCount = 0;
  for (int i = 0; i < 3; i++) {
    delay(10000);
    bool ok = sendAT("AT", "OK", 3000);
    Serial.print("[eDRX] AT check ");
    Serial.print(i + 1);
    Serial.print("/3: ");
    Serial.println(ok ? "OK" : "no response");
    if (ok) okCount++;
  }

  // HTTP GET で通信確認
  bool httpPass = bg96HttpGet(HTTP_TEST_URL);
  Serial.print("[eDRX] HTTP GET: ");
  Serial.println(httpPass ? "PASS" : "FAIL");

  // eDRX無効化
  sendAT("AT+CEDRXS=0", "OK", 3000);
  Serial.println("[eDRX] eDRX disabled");

  Serial.print("[eDRX] Result: ");
  Serial.println((okCount == 3 && httpPass) ? "PASS" : "FAIL");
  Serial.println("=== BG96 eDRX Test Done ===");
}

void printBanner() {
  Serial.println();
  Serial.println("=== sIoTamago nRF52 Hardware Test ===");
  Serial.println("Commands:");
  Serial.println("  h : show this help");
  Serial.println("  l : LED blink test");
  Serial.println("  i : I2C scan");
  Serial.println("  a : analog read (if available)");
  Serial.println("  b : button read (if available)");
  Serial.println("  r : run all quick tests");
  Serial.println("  q : BG96 test (AT / network / internet)");
  Serial.println("  g : BG96 HTTP GET test (SORACOM metadata)");
  Serial.println("  p : BG96 PSM test (sleep/wake, ~90s)");
  Serial.println("  e : BG96 eDRX test (~30s)");
  Serial.println("  m : EEPROM read/write test (24LC256, 0x50)");
  Serial.println();
}

void testLed(unsigned int times = 3, unsigned int onMs = 150, unsigned int offMs = 150) {
  Serial.println("[LED] test start");
  for (unsigned int i = 0; i < times; ++i) {
    digitalWrite(TEST_LED_PIN, HIGH);
    delay(onMs);
    digitalWrite(TEST_LED_PIN, LOW);
    delay(offMs);
  }
  Serial.println("[LED] test done");
}

void testButton() {
#ifdef TEST_BUTTON_PIN
  pinMode(TEST_BUTTON_PIN, INPUT_PULLUP);
  int v = digitalRead(TEST_BUTTON_PIN);
  Serial.print("[BUTTON] pin=");
  Serial.print(TEST_BUTTON_PIN);
  Serial.print(" state=");
  Serial.print(v);
  Serial.println(v == LOW ? " (PRESSED)" : " (RELEASED)");
#else
  Serial.println("[BUTTON] no board macro found (PIN_BUTTON1)");
#endif
}

void testAnalog() {
#ifdef TEST_ANALOG_PIN
  int raw = analogRead(TEST_ANALOG_PIN);
  Serial.print("[ANALOG] pin=");
  Serial.print(TEST_ANALOG_PIN);
  Serial.print(" raw=");
  Serial.println(raw);
#else
  Serial.println("[ANALOG] no board macro found (PIN_VBAT/A0)");
#endif
}

void testI2CScan() {
  Serial.println("[I2C] scan start");

  // GROVEのVCCはBG96電源レール経由のため、先に電源投入が必要
  pinMode(MODULE_PWR_PIN,  OUTPUT); digitalWrite(MODULE_PWR_PIN,  HIGH);
  delay(100);
  pinMode(GROVE_VCCB_PIN,  OUTPUT); digitalWrite(GROVE_VCCB_PIN,  HIGH);
  delay(200);  // ICの起動を待つ
  Wire.begin();

  uint8_t found = 0;

  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
      Serial.print("[I2C] found device at 0x");
      if (addr < 16) {
        Serial.print('0');
      }
      Serial.println(addr, HEX);
      ++found;
    }
  }

  Serial.print("[I2C] scan done, devices=");
  Serial.println(found);
}

// 24LC256 EEPROM 読み書きテスト（アドレス0x50固定、4KB、400kHz）
// GROVEの電源はtestI2CScan()と同様に先に投入する
void testEEPROM() {
  Serial.println("[EEPROM] test start (24LC256, addr=0x50, 4096 bytes, 400kHz)");

  // GROVE電源ON
  pinMode(MODULE_PWR_PIN, OUTPUT); digitalWrite(MODULE_PWR_PIN, HIGH);
  delay(100);
  pinMode(GROVE_VCCB_PIN, OUTPUT); digitalWrite(GROVE_VCCB_PIN, HIGH);
  delay(200);
  Wire.begin();
  Wire.setClock(400000);  // 400kHz Fast Mode

  const uint8_t  devAddr   = 0x50;
  const uint16_t startAddr = 0x0000;
  const uint16_t dataLen   = 4096;
  const uint8_t  pageSize  = 64;   // 24LC256のページバッファサイズ
  const uint8_t  readChunk = 32;

  // --- Step1: 現在のデータを読み出す ---
  Serial.println("[EEPROM] step1: read current data...");
  Wire.beginTransmission(devAddr);
  Wire.write((uint8_t)(startAddr >> 8));
  Wire.write((uint8_t)(startAddr & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    Serial.println("[EEPROM] read seek FAIL"); return;
  }

  // 読み出したデータをRAMに保存（4KB、nRF52840は余裕）
  static uint8_t buf[4096];
  for (uint16_t offset = 0; offset < dataLen; offset += readChunk) {
    Wire.requestFrom((uint8_t)devAddr, readChunk);
    for (uint8_t i = 0; i < readChunk; i++) {
      if (!Wire.available()) {
        Serial.print("[EEPROM] read underrun at offset="); Serial.println(offset + i); return;
      }
      buf[offset + i] = Wire.read();
    }
  }
  Serial.print("[EEPROM] current[0..3]: ");
  for (uint8_t i = 0; i < 4; i++) { Serial.print(buf[i], HEX); Serial.print(" "); }
  Serial.println();

  // --- Step2: ビット反転して書き込む ---
  Serial.println("[EEPROM] step2: write inverted data...");
  unsigned long writeStart = millis();
  for (uint16_t offset = 0; offset < dataLen; offset += pageSize) {
    uint16_t addr = startAddr + offset;
    Wire.beginTransmission(devAddr);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    for (uint8_t i = 0; i < pageSize; i++) Wire.write((uint8_t)(~buf[offset + i]));
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
      Serial.print("[EEPROM] write FAIL at offset="); Serial.print(offset);
      Serial.print(" (I2C error="); Serial.print(err); Serial.println(")"); return;
    }
    delay(10);  // ページ書き込みサイクル最大5ms、余裕を持って10ms
  }
  unsigned long writeMs = millis() - writeStart;
  Serial.print("[EEPROM] write done in "); Serial.print(writeMs); Serial.println(" ms");

  // --- Step3: 読み出して検証（反転値と一致するか）---
  Wire.beginTransmission(devAddr);
  Wire.write((uint8_t)(startAddr >> 8));
  Wire.write((uint8_t)(startAddr & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    Serial.println("[EEPROM] read seek FAIL"); return;
  }

  Serial.println("[EEPROM] step3: read back & verify...");
  unsigned long readStart = millis();
  bool pass = true;
  uint16_t verified = 0;
  for (uint16_t offset = 0; offset < dataLen && pass; offset += readChunk) {
    Wire.requestFrom((uint8_t)devAddr, readChunk);
    for (uint8_t i = 0; i < readChunk; i++) {
      if (!Wire.available()) {
        Serial.print("[EEPROM] read underrun at offset="); Serial.println(offset + i);
        pass = false; break;
      }
      uint8_t got      = Wire.read();
      uint8_t expected = (uint8_t)(~buf[offset + i]);
      if (got != expected) {
        Serial.print("[EEPROM] mismatch at offset="); Serial.print(offset + i);
        Serial.print(" expected=0x"); Serial.print(expected, HEX);
        Serial.print(" got=0x"); Serial.println(got, HEX);
        pass = false; break;
      }
      verified++;
    }
  }
  unsigned long readMs = millis() - readStart;

  Serial.print("[EEPROM] read done in "); Serial.print(readMs); Serial.println(" ms");
  Serial.print("[EEPROM] verified "); Serial.print(verified); Serial.println(" bytes");
  Serial.print("[EEPROM] result: "); Serial.println(pass ? "PASS" : "FAIL");
}

void runAllQuickTests() {
  testLed();
  testButton();
  testAnalog();
  testI2CScan();
}

void setup() {
  pinMode(TEST_LED_PIN, OUTPUT);
  digitalWrite(TEST_LED_PIN, LOW);

  Serial.begin(115200);
  unsigned long waitStart = millis();
  while (!Serial && (millis() - waitStart < 3000)) {
    // Wait briefly for USB serial connection.
  }

  Wire.begin();

  printBanner();
  Serial.print("CPU freq: ");
  Serial.print(F_CPU);
  Serial.println(" Hz");
  Serial.print("LED pin: ");
  Serial.println(TEST_LED_PIN);

  runAllQuickTests();
}

void loop() {
  if (Serial.available() > 0) {
    char c = (char)Serial.read();
    switch (c) {
      case 'h':
      case '?':
        printBanner();
        break;
      case 'l':
        testLed();
        break;
      case 'i':
        testI2CScan();
        break;
      case 'a':
        testAnalog();
        break;
      case 'b':
        testButton();
        break;
      case 'r':
        runAllQuickTests();
        break;
      case 'q':
        testBG96All();
        break;
      case 'g':
        testBG96HttpGet();
        break;
      case 'p':
        testBG96PSM();
        break;
      case 'e':
        testBG96Edrx();
        break;
      case 'm':
        testEEPROM();
        break;
      case '\n':
      case '\r':
        break;
      default:
        Serial.print("Unknown command: ");
        Serial.println(c);
        printBanner();
        break;
    }
  }

  // Heartbeat: blink every second so you can confirm firmware is alive.
  unsigned long now = millis();
  if (now - lastHeartbeatMs >= 1000) {
    lastHeartbeatMs = now;
    digitalWrite(TEST_LED_PIN, !digitalRead(TEST_LED_PIN));
  }
}

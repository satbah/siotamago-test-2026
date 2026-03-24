# sIoTamago (nRF52) Hardware Test Sketch

このディレクトリには、`cami:nrf52:siotamago` 向けのハードウェアテスト用スケッチ `test01.ino` が入っています。

## 開発環境

本プロジェクトは **VS Code 拡張機能「Arduino Community Edition」** を使用してください。

- Extension: `Arduino Community Edition`
- Board: `cami:nrf52:siotamago`

## 使い方

1. VS Code でこのフォルダを開く。
2. `Arduino Community Edition` をインストールする。
3. ボードを `cami:nrf52:siotamago` に設定する。
4. `test01.ino` を開いて書き込み（Upload）する。
5. シリアルモニタを `115200` bps で開いてテスト結果を確認する。

## スケッチ概要

`test01.ino` は以下を確認できます。

- LED点滅
- I2Cスキャン
- ボタン読み取り（定義がある場合）
- アナログ読み取り（定義がある場合）
- シリアルコマンドによる個別テスト実行

## シリアルポート割り当て

| ポート | ペリフェラル | 用途 | ピン（基板番号） |
|---|---|---|---|
| `Serial` | USB CDC | PC接続（シリアルモニター） | USB |
| `Serial1` | UARTE0 | BG96 LTE-Mモジュール | RX=7, TX=6 |
| `Serial2` | UARTE1 | GROVE UARTコネクタ | RX=13, TX=14 |

> `Serial1` は `PIN_SERIAL1_RX/TX`、`Serial2` は BSP内部で `GROVE_UART_RX/TX` に固定されている。
> `Serial2` の extern 宣言は Uart.h から自動公開されないため、スケッチ側で `extern Uart Serial2;` が必要。

## GROVEコネクタへの電源供給

GROVEコネクタの VCC は **BG96の電源レール（MODULE_PWR_PIN）から3.3Vを生成** している。
そのため、GROVEに接続したICを使用する前に以下の順序で電源を投入する必要がある。

```cpp
// 1. BG96電源レール投入（GROVEのVCC源）
pinMode(MODULE_PWR_PIN, OUTPUT);   // pin 36
digitalWrite(MODULE_PWR_PIN, HIGH);
delay(100);

// 2. GROVEコネクタへのVCC供給を有効化
pinMode(GROVE_VCCB_PIN, OUTPUT);   // pin 25
digitalWrite(GROVE_VCCB_PIN, HIGH);
delay(200);  // ICの起動を待つ
```

> BG96自体（PWRKEY制御）を起動する必要はない。`MODULE_PWR_PIN` を HIGH にするだけでよい。

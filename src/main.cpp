#include <esp_now.h>
#include <WiFi.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

// ==== Настройки ====
#define DEBOUNCE_TIME    50    // мс для антидребезга
#define ledpin 19


LiquidCrystal_I2C lcd(0x27, 20, 4);

// Структура сообщений ESP-NOW (такая же, как у мастера)
typedef struct {
  uint8_t type;       // 1 - авария от ведущего, 2 - подтверждение от ведомого, 3 - квитирование от ведомого, 
  bool alarm;         // флаг аварии (для type=1)
  char tag[12];       // тег аварии (до 11 символов + нуль-терминатор)
  char card[13];    // код карты
} message_t;

// ==== Переменные ====
bool alarmActive = false;           // активна ли авария на этом ведомом
char currentTag[12] = "";           // текущий тег аварии
uint8_t masterMac[6] = {0x10, 0x00, 0x3B, 0x4C, 0x9B, 0xC4};              // MAC-адрес ведущего (запомним при первом получении)
bool masterMacSet = true;          // флаг, что MAC ведущего известен
//10:00:3B:4C:9B:C4
bool delayemKvitirovanye = false;
bool stopSerialRead = false;
bool ackFromMaster = true;
unsigned long alarmSentTime; 

// ==== Отправка ответа ведущему ====
bool sendToMaster(const message_t &msg) {
  if (!masterMacSet) return false;
  esp_err_t result = esp_now_send(masterMac, (uint8_t*)&msg, sizeof(msg));
  if (msg.type == 3) {
    ackFromMaster = false;
  }
  return result == ESP_OK;
}
void lcdPrint(byte stroka = 0, String data = " ", byte stolbets = 0) {
  lcd.setCursor(0, stroka);
  lcd.print("....................");
  String stroka20 = data.substring(0, 19);
  lcd.setCursor(stolbets, stroka);
  lcd.print(stroka20);
}

// ==== Callback получения данных ====
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (len != sizeof(message_t)) return;
  message_t msg;
  memcpy(&msg, incomingData, sizeof(msg));

  // Запоминаем MAC ведущего (если ещё не известен)
  if (!masterMacSet) {
    memcpy(masterMac, mac, 6);
    masterMacSet = true;
    Serial.printf("MAC ведущего сохранён: %d", mac);
  }

  // Обрабатываем только (type=1)
  if (msg.type == 1) {
    stopSerialRead = false;
    // Активируем аварию
    alarmActive = msg.alarm;
    if (msg.alarm == false) {
      delayemKvitirovanye = false;
      currentTag[0] = '0';
    }
    strncpy(currentTag, msg.tag, sizeof(currentTag)-1);
    currentTag[sizeof(currentTag)-1] = '\0';
    Serial.printf("Получена авария: %s\n", currentTag);

    // Отправляем подтверждение (type=2) с тем же тегом
    message_t ack;
    ack.type = 2;
    ack.alarm = true;        // можно не использовать, но для единообразия
    strcpy(ack.tag, currentTag);
    if (sendToMaster(ack)) {
      Serial.println("Подтверждение отправлено");
    }
    lcdPrint(0, (String)currentTag);
    ackFromMaster = true;
  } else if (msg.type == 2) { 
    Serial.println("Подтверждение от мастера на получение карты");
    lcdPrint(3, "OTVET OT ESP-OK");
    ackFromMaster = true;
    stopSerialRead = false;
  }
  delay(10);
  // Другие типы игнорируем
}


// ==== Настройка ====
void setup() {
  Serial.begin(9600);
  Serial2.begin(9600); // Инициализация UART2, RX только
  // Настройка кнопки (внутренняя подтяжка к VCC, кнопка замыкает на GND)
  pinMode(ledpin, OUTPUT);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) {
    Serial.println("Ошибка инициализации ESP-NOW");
    return;
  }
  Serial.println(WiFi.macAddress());
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, masterMac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.printf("еспОшибка добавления master'a");
    }

  esp_now_register_recv_cb(OnDataRecv);
  Wire.begin();
  lcd.init();
  // Включить подсветку (если есть)
  lcd.backlight();
  lcd.clear();
  lcdPrint(0, "GOTOV", 7);
  Serial.println("Ведомый готов V1.0.5");
}

unsigned long lastToneMillis = 0;
unsigned long interval = 1000;
bool output = false;
char cardNumber[13];
byte retryCount = 0;
// ==== Главный цикл ====

void loop() {
if ((Serial2.available() >= 14) && (stopSerialRead == false)) {
    byte buffer[14];
    Serial2.readBytes(buffer, 14);
    // Проверяем стартовый и стоповый байты
    if (buffer[0] == 0x02 && buffer[13] == 0x03) {
      // Собираем 12 символов номера карты (индексы 1..12)
      for (int i = 1; i <= 12; i++) {
        cardNumber[i-1] = (char)buffer[i];
      }
      cardNumber[13] = '\0';
      Serial.print("Card ID: ");
      Serial.println(cardNumber);
      lcdPrint(1, cardNumber);
      if (alarmActive == true) {
        delayemKvitirovanye = true;
        stopSerialRead = true;
        Serial.println("kvit");
      }
    } else {
      // Если пакет невалидный, сбрасываем буфер
      while (Serial2.available()) {Serial2.read();}
      stopSerialRead = false;
    }
  } else if (stopSerialRead == true) {
    while (Serial2.available())
    {
      Serial2.read();
    }
  }

  if (alarmActive && delayemKvitirovanye) {
     delayemKvitirovanye = false;
     noTone(ledpin);
     message_t ack;
     ack.type = 3;
     ack.alarm = false; // не используется
     strcpy(ack.card, cardNumber);
     strcpy(ack.tag, currentTag);
  if (sendToMaster(ack)) {
    Serial.printf("Квитирование отправлено для тега %s\n", currentTag);
    Serial.println(ack.card);
    lcdPrint(3, "SENDING....");
    }   else {
        Serial.println("Ошибка отправки квитирования");
    }
    alarmSentTime = millis();
  }
  // работа биперов и остального
  unsigned long currentMillis = millis();
  if ((currentMillis - lastToneMillis >= interval) && alarmActive) {
    lastToneMillis = currentMillis; // Запоминаем время
    tone(ledpin, 5000, 500);
    }
  


    if (ackFromMaster == false) {
    if (millis() - alarmSentTime > 3000) {
      retryCount++;
      if (retryCount <= 5) {
        // Повторная отправка только тем, кто не ответил? Для простоты шлем всем.
        alarmSentTime = millis();
        Serial.printf("esp retry alarm #%d\n", retryCount);
        message_t ack;
        ack.type = 3;
        ack.alarm = false; // не используется
        strcpy(ack.card, cardNumber);
        strcpy(ack.tag, currentTag);
        sendToMaster(ack);
        String toLcd = "No response,retry: " + String(retryCount);
        lcdPrint(2, toLcd);
      } else {
        // Превышено число попыток — считаем, что связь потеряна
        lcdPrint(2, "No master esp!!!!");
        stopSerialRead = false;
        ackFromMaster = true;
        alarmActive = false;
        Serial.printf("espTimeout retries %s\n", currentTag);
      }
    }
  }
delay(50);
}

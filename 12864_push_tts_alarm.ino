#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Pushsafer.h>

// LCD 핀 정의 (ST7920)
#define LCD_CLK 13
#define LCD_MOSI 11
#define LCD_CS 10
U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R0, LCD_CLK, LCD_MOSI, LCD_CS);

#define BUTTON_PIN 4

bool hasPlayedOnce = false;

bool alarmMode = false;
unsigned long buttonLastPressTime = 0;
unsigned int buttonPressCount = 0;
unsigned long buttonPressStart = 0;

int alarmHour = 7;  // 초기 알람 시간 예시
int alarmMinute = 0;

unsigned int alarmModeButtonPressCount = 0;

enum AlarmAdjustState {
  ADJUST_NONE,
  ADJUST_HOUR,
  ADJUST_MINUTE
};
AlarmAdjustState adjustState = ADJUST_NONE;

unsigned long lastAdjustMillis = 0;
const unsigned long adjustTimeout = 20000;  // 30초 동안 조작 없으면 알람 모드 종료

bool isButtonPressed() {
  return digitalRead(BUTTON_PIN) == LOW;
}

// WiFi 설정
const char* ssid = "SACP";
const char* password = "wjdgns1357";
WiFiClient wifiClient;

// OpenWeatherMap 설정
const char* weatherApiHost = "api.openweathermap.org";                                                     // api 서버 도메인 주소
String apiKey = "7361f92532927c77b5620a08220d6630";                                                        // 사용자 api
String cityName = "Asan";                                                                                  // 날씨 정보 지역
String weatherApiPath = "/data/2.5/weather?q=" + cityName + "&appid=" + apiKey + "&lang=kr&units=metric";  // http 요청 경로
String airPollutionApiPath = "/data/2.5/air_pollution?lat=36.7926&lon=127.0017&appid=" + apiKey;

HttpClient httpClient = HttpClient(wifiClient, weatherApiHost, 80);

const char* pushsaferKey = "1HNGTJv5J3PN0INeyEuR";  // 사용자 api
Pushsafer pushsafer(pushsaferKey, wifiClient);      //pushsafer 라이브러리 파일 사용

// NTP 시간
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 9 * 3600, 60000);  //UTC+9 시간 오프셋 설정 1분마다 시간 정보 갱신

// DFPlayer Mini
SoftwareSerial mySerial(2, 3);
DFRobotDFPlayerMini myDFPlayer;

// 전역 변수
String latestWeatherDesc = "";
int latestTemperature = 0;
int latestPM10 = 0;
int latestPM25 = 0;


void setup() {
  Serial.begin(9600);
  mySerial.begin(9600);
  u8g2.begin();

  if (!myDFPlayer.begin(mySerial)) {
    Serial.println("DFPlayer Mini 초기화 실패!");
    while (true)
      ;
  }
  myDFPlayer.volume(16);

  WiFi.begin(ssid, password);
  Serial.println("Wi-Fi 연결 중...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi 연결 완료");

  timeClient.begin();
  while (!timeClient.update()) {
    Serial.println("시간 동기화 재시도 중...");
    delay(1000);
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP);  // 버튼 핀을 풀업 입력으로 설정

  // WiFi 연결 후 1회만 음성 재생
  if (!hasPlayedOnce && WiFi.status() == WL_CONNECTED) {
    int hour = timeClient.getHours();
    int minute = timeClient.getMinutes();
    Serial.println("현재 시각: " + String(hour) + " : " + String(minute));
    playTimeAsMP3(hour, minute);
    delay(1000);
    getAndPlayWeather();
    delay(1000);
    getAndPlayDust();
    delay(1000);

    hasPlayedOnce = true;
  }
}

unsigned long previousWeatherMillis = 0;
const long weatherInterval = 60000;  // 1분
unsigned long previousDisplayMillis = 0;
const long displayInterval = 1000;  // 1초

void loop() {
  timeClient.update();
  int hour = timeClient.getHours();      // 시 데이터 추출 후 hour 함수에 저장
  int minute = timeClient.getMinutes();  // 분 데이터 추출 후 minute 함수에 저장
  int second = timeClient.getSeconds();  // 초 데이터 추출 후 second 함수에 저장

  unsigned long currentMillis = millis();

  static bool buttonPrevState = HIGH;
  static unsigned long buttonPressStart = 0;
  static bool buttonHeld = false;
  static unsigned long lastRepeatMillis = 0;
  static bool buttonShortPressTriggered = false;

  const unsigned long repeatInterval = 500;  // 시간 증가 반복 간격

  bool buttonState = digitalRead(BUTTON_PIN);

  // 버튼 눌림 감지 시작
  if (buttonPrevState == HIGH && buttonState == LOW) {
    buttonPressStart = currentMillis;
    buttonHeld = false;
  }

  // 버튼 떼짐 감지
  if (buttonPrevState == LOW && buttonState == HIGH) {
    unsigned long pressDuration = currentMillis - buttonPressStart;

    if (!alarmMode) {
      if (pressDuration >= 3000) {
        // 3초 이상 길게 눌러서 알람 모드 진입
        alarmMode = true;
        adjustState = ADJUST_HOUR;
        Serial.println("알람 모드 진입 - 시간 조절 시작 (시 조절)");
        lastAdjustMillis = currentMillis;

        playMP3ByNumber(139);  // 알람 모드 진입 안내 음성
        delay(2000);

        alarmModeButtonPressCount = 0;  // 버튼 눌림 카운터 초기화
      } else if (pressDuration < 1000) {
        // 짧게 눌렀을 때 음성 재생 요청 트리거
        buttonShortPressTriggered = true;
      }
    } else {
      // 알람 모드 내에서 짧게 눌렀을 때 조절 모드 전환 + 종료 체크
      if (pressDuration < 1500) {
        alarmModeButtonPressCount++;
        Serial.print("알람 모드 버튼 누름 횟수: ");
        Serial.println(alarmModeButtonPressCount);

        if (alarmModeButtonPressCount >= 3) {
          // 3번 누르면 알람 모드 종료
          alarmMode = false;
          adjustState = ADJUST_NONE;
          alarmModeButtonPressCount = 0;

          int ampmFile = (alarmHour < 12) ? 2 : 3;  // 오전: 2, 오후: 3
          int hour12 = alarmHour % 12;
          if (hour12 == 0) hour12 = 12;
          int hourFile = 3 + hour12;          // 4~15
          int minuteFile = 16 + alarmMinute;  // 16~75

          playMP3ByNumber(ampmFile);  // 오전/오후
          delay(1000);
          playMP3ByNumber(hourFile);  // 시
          delay(1000);
          playMP3ByNumber(minuteFile);  // 분
          delay(1000);
          playMP3ByNumber(140);  // 알람 모드 종료 안내 음성
          delay(2000);
          Serial.println("알람 모드 종료 (버튼 3회 누름)");
        } else {
          // 조절 모드 전환 기존 기능 유지
          if (adjustState == ADJUST_HOUR) {
            adjustState = ADJUST_MINUTE;
            Serial.println("분 조절 모드로 전환");
          } else {
            adjustState = ADJUST_HOUR;
            Serial.println("시 조절 모드로 전환");
          }
          lastAdjustMillis = currentMillis;
        }
      }
    }

    buttonHeld = false;
  }

  // 알람 모드에서 버튼 길게 누름 처리 (값 증가 반복)
  if (alarmMode && buttonState == LOW) {
    unsigned long pressDuration = currentMillis - buttonPressStart;

    if (pressDuration >= 2000) {
      if (!buttonHeld || (currentMillis - lastRepeatMillis >= repeatInterval)) {
        if (adjustState == ADJUST_HOUR) {
          alarmHour = (alarmHour + 1) % 24;
        } else if (adjustState == ADJUST_MINUTE) {
          alarmMinute = (alarmMinute + 1) % 60;
        }
        lastRepeatMillis = currentMillis;
        buttonHeld = true;
        lastAdjustMillis = currentMillis;
      }
    }
  }

  buttonPrevState = buttonState;

  // 버튼 짧게 눌렀을 때 음성 재생 실행 (알람 모드가 아닐 때만)
  if (buttonShortPressTriggered && !alarmMode) {
    playTimeAsMP3(hour, minute);
    delay(1000);
    getAndPlayWeather();
    delay(1000);
    getAndPlayDust();
    delay(1000);

    sendPushsaferNotification(latestWeatherDesc, latestTemperature, String(latestPM10), String(latestPM25));
    //아두이노에 저장된 통합 정보를 sendPushsaferNotification 함수로 전달

    buttonShortPressTriggered = false;  // 재생 완료 후 플래그 초기화
  }

  // 알람 모드 종료 (조작 없을 때 30초 후 종료)
  if (alarmMode && (currentMillis - lastAdjustMillis > adjustTimeout)) {
    alarmMode = false;
    adjustState = ADJUST_NONE;

    int ampmFile = (alarmHour < 12) ? 2 : 3;  // 오전: 2, 오후: 3
    int hour12 = alarmHour % 12;
    if (hour12 == 0) hour12 = 12;
    int hourFile = 3 + hour12;          // 4~15
    int minuteFile = 16 + alarmMinute;  // 16~75

    playMP3ByNumber(ampmFile);  // 오전/오후
    delay(1000);
    playMP3ByNumber(hourFile);  // 시
    delay(1000);
    playMP3ByNumber(minuteFile);  // 분
    delay(1000);
    playMP3ByNumber(140);  // 알람 모드 종료 안내 음성
    delay(2000);
  }

  // 알람 시간 도달 체크 및 알람음 재생
  static bool alarmTriggered = false;
  if (!alarmMode) {  // 알람 모드가 아닐 때만 알람 체크
    if (hour == alarmHour && minute == alarmMinute && !alarmTriggered) {
      Serial.println("알람 시간 도달! 141.mp3 재생");
      playMP3ByNumber(141);
      delay(13500);
      playMP3ByNumber(141);
      alarmTriggered = true;
    }
    // 분이 바뀌면 알람 트리거 리셋 (한 번만 재생하기 위해)
    if (minute != alarmMinute) {
      alarmTriggered = false;
    }
  }

  // 1초마다 화면 갱신
  static unsigned long previousDisplayMillis = 0;
  const long displayInterval = 500;
  if (currentMillis - previousDisplayMillis >= displayInterval) {
    previousDisplayMillis = currentMillis;
    displayInfo(hour, minute, second);
  }
}


void displayInfo(int hour24, int minute, int second) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_unifont_t_korean2);

  if (alarmMode) {
    // 알람 모드 화면 표시
    String alarmModeStr = "알람 설정 중";
    u8g2.drawUTF8(0, 16, alarmModeStr.c_str());

    String adjustStr = "";
    if (adjustState == ADJUST_HOUR) adjustStr = "시간 조절";
    else if (adjustState == ADJUST_MINUTE) adjustStr = "분 조절";
    else adjustStr = "대기중";

    u8g2.drawUTF8(0, 32, adjustStr.c_str());

    String alarmTimeStr = "시간: ";
    alarmTimeStr += (alarmHour / 12 == 0 ? "오전 " : "오후 ");
    alarmTimeStr += String(alarmHour % 12 == 0 ? 12 : alarmHour % 12);
    alarmTimeStr += (alarmMinute < 10 ? ":0" : ":") + String(alarmMinute);

    u8g2.drawUTF8(0, 48, alarmTimeStr.c_str());

  } else {
    // 일반 모드 화면 표시
    String colon = (second % 2 == 0) ? ":" : " ";

    String ampm = (hour24 < 12) ? "오전" : "오후";
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;

    String timeStr = "시간: " + ampm + " " + String(hour12) + colon + (minute < 10 ? "0" : "") + String(minute);
    u8g2.drawUTF8(0, 16, timeStr.c_str());

    String weatherStr = "날씨: " + latestWeatherDesc;
    u8g2.drawUTF8(0, 32, weatherStr.c_str());

    String tempStr = "기온: " + String(latestTemperature) + " 'C";
    u8g2.drawUTF8(0, 48, tempStr.c_str());


// PM10 수치에 따라 상태 문자열 결정
    String pm10Status = "";
    if (latestPM10 <= 30) {
      pm10Status = "좋음";
    } else if (latestPM10 <= 80) {
      pm10Status = "보통";
    } else if (latestPM10 <= 150) {
      pm10Status = "나쁨";
    } else {
      pm10Status = "매우나쁨";
    }

    // PM10 상태 텍스트만 표시
    String dustStr = "미세먼지: " + pm10Status;
    u8g2.drawUTF8(0, 64, dustStr.c_str());
  }

  u8g2.sendBuffer();
}



void getAndPlayWeather() {
  Serial.println("날씨 정보 요청 중...");
  httpClient.get(weatherApiPath);  // httpClient 라이브러리 파일을 이용해 기상 데이터 다운로드

  int statusCode = httpClient.responseStatusCode();
  if (statusCode != 200) {
    Serial.print("날씨 API 요청 실패: ");
    Serial.println(statusCode);
    return;
  }

  String response = httpClient.responseBody();
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.println("JSON 파싱 실패: " + String(error.c_str()));
    return;
  }

  const char* desc = doc["weather"][0]["description"];  // 날씨 설명 데이터 추출 후 desc 함수로 저장
  float temp = doc["main"]["temp"];                     // 온도 데이터 추출 후 temp 함수로 저장

  // "온흐림" → "약간 흐림", "실 비" → "약한 비"로 변경
  if (String(desc) == "온흐림") {
    latestWeatherDesc = "약간 흐림";
  } else if (String(desc) == "실 비") {
    latestWeatherDesc = "약한 비";
  } else {
    latestWeatherDesc = String(desc);
  }

  latestTemperature = round(temp);

  Serial.println("날씨: " + latestWeatherDesc);
  Serial.print("온도: ");
  Serial.println(latestTemperature);

  // mp3 재생 시에는 원래 desc를 넘기되, 필요하면 playWeatherMP3()에서 처리 가능
  playWeatherMP3(latestWeatherDesc);
  delay(2000);
  playTemperatureMP3(temp);
}



void getAndPlayDust() {
  Serial.println("미세먼지 정보 요청 중...");
  httpClient.get(airPollutionApiPath);

  int statusCode = httpClient.responseStatusCode();
  if (statusCode != 200) {
    Serial.print("대기오염 API 요청 실패: ");
    Serial.println(statusCode);
    return;
  }

  String response = httpClient.responseBody();
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.println("JSON 파싱 실패: " + String(error.c_str()));
    return;
  }

  JsonObject main = doc["list"][0]["components"];  // 대기오염 데이터 리스트
  int pm10 = round(main["pm10"].as<float>());      // pm10 정보 추출 후 저장
  int pm25 = round(main["pm2_5"].as<float>());     // pm2.5 정보 추출 후 저장
  latestPM10 = pm10;                               // 함수 이름 변경
  latestPM25 = pm25;

  Serial.print("PM10: ");
  Serial.println(pm10);

  if (pm10 <= 30) playMP3ByNumber(135);        // 좋음
  else if (pm10 <= 80) playMP3ByNumber(136);   // 보통
  else if (pm10 <= 150) playMP3ByNumber(137);  // 나쁨
  else playMP3ByNumber(138);                   // 매우 나쁨

  delay(2000);
}

void playTimeAsMP3(int hour24, int minute) {
  bool isAM = hour24 < 12;
  int hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;
  int file_timeis = 1;
  int file_ampm = isAM ? 2 : 3;
  int file_hour = 3 + hour12;
  int file_min = 16 + minute;

  playMP3ByNumber(file_timeis);
  delay(1500);
  playMP3ByNumber(file_ampm);
  delay(1400);
  playMP3ByNumber(file_hour);
  delay(1400);
  playMP3ByNumber(file_min);
  delay(1400);
}

void playWeatherMP3(String desc) {
  int file_weather = 81;
  if (desc.indexOf("맑음") >= 0) file_weather = 76;
  else if (desc.indexOf("흐림") >= 0) file_weather = 77;
  else if (desc.indexOf("구름") >= 0) file_weather = 78;
  else if (desc.indexOf("비") >= 0) file_weather = 79;
  else if (desc.indexOf("눈") >= 0) file_weather = 80;
  playMP3ByNumber(file_weather);
}

void playTemperatureMP3(float temp) {
  int tempRounded = round(temp);
  playMP3ByNumber(82);  // "현재 온도는"
  delay(2000);

  if (tempRounded < 0) {
    playMP3ByNumber(83);  // "영하"
    delay(1000);
    tempRounded = abs(tempRounded);
  }

  if (tempRounded >= 0 && tempRounded <= 50) {
    playMP3ByNumber(84 + tempRounded);  // 예: 084.mp3 ~ 134.mp3
  }
}

void playMP3ByNumber(int num) {
  Serial.print("MP3 재생: ");
  Serial.println(num);
  myDFPlayer.play(num);
}

void sendPushsaferNotification(String weather, float temperature, String dustStatus1, String dustStatus2) {
  String message = "날씨: " + weather + "\n" + "기온: " + String(temperature, 1) + "℃\n" + "미세먼지: PM10 " + dustStatus1 + "µm /  PM2.5 " + dustStatus2 + "µm";  // 알림 메세지 내용

  struct PushSaferInput input;  // 알림 메시지 설정
  input.message = message;
  input.title = "아두이노 날씨 알림";
  input.device = "";      // 모든 기기로 전송
  input.icon = "5";       // 알림 아이콘 번호
  input.sound = "6";      // 알림 소리
  input.vibration = "2";  // 진동
  input.priority = "1";   // 우선순위
  input.url = "";
  input.urlTitle = "";
  input.time2live = "";

  pushsafer.sendEvent(input);
}

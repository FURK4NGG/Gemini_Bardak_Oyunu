#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>          // I2C iletişimi için
#include <Adafruit_GFX.h>  // Temel grafik kütüphanesi
#include <Adafruit_SSD1306.h> // SSD1306 OLED ekran kütüphanesi
#include <ctype.h>         // tolower() fonksiyonu için

// Wi-Fi Bilgileri - KENDİ BİLGİLERİNİZLE DEĞİŞTİRİN
const char* ssid = "BARDAK";    // Buraya Wi-Fi ağınızın adını (SSID) girin
const char* password = ""; // Buraya Wi-Fi parolanızı girin
const char* Gemini_Token = ""; // Buraya Gemini API anahtarınızı girin
const char* Gemini_Max_Tokens = "200";           // Gemini'den maksimum çıktı token sayısı

// Google API Sunucusunun SHA1 Fingerprint'i (Güvenli bağlantı için)
// ÖNEMLİ: client.setInsecure() kullanıldığında bu satır devre dışı kalır.
// Güvenlik için, mümkünse her zaman doğru ve güncel bir fingerprint kullanmalısınız.
const char* fingerprint = ""; //Buraya fingerprint inizi girin
bool force_ask = false;

Adafruit_MPU6050 mpu;

// OLED Tanımlamaları
#define SCREEN_WIDTH 128    // OLED ekran genişliği, piksel
#define SCREEN_HEIGHT 64    // OLED ekran yüksekliği, piksel
#define OLED_RESET -1       // Reset pini (ESP8266 için -1 veya herhangi bir kullanılmayan GPIO)
#define OLED_SDA 14         // D6 = GPIO14
#define OLED_SCL 12         // D5 = GPIO12


Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Buton Pinleri (ESP8266 pinlerine göre ayarlayın)
const int BUTTON_YES_PIN = D7;      // GPIO5 (Evet/Onayla)
const int BUTTON_NO_PIN = D6;      // GPIO4 (Hayır/Reddet)
const int BUTTON_NOT_SURE_PIN = D3; // GPIO0 (Emin Değilim)

// Buton Debounce için
unsigned long lastButtonPressTime = 0;
const long debounceDelay = 200; // milisaniye

// Oyun Durumları
enum GameState {
  STATE_SHOW_MPU,
  STATE_CONNECTING_WIFI,
  STATE_GAME_START,
  STATE_ASKING_QUESTION,
  STATE_WAITING_FOR_ANSWER,
  STATE_SENDING_TO_GEMINI,
  STATE_WAITING_FOR_GEMINI_CONFIRMATION,
  STATE_GAME_OVER
};
GameState currentGameState = STATE_CONNECTING_WIFI;

// Oyun Ayarları
const int QUESTIONS_PER_BLOCK = 7;
const int MAX_QUESTION_BLOCKS = 4; // Toplamda 4 blok (7*4 = 28 soru)
String questions[QUESTIONS_PER_BLOCK]; // Her blok için 7 soru tutacak dizi
String allAnswers[QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS]; // Tüm cevapları tutacak dizi (28 cevap)
String allQuestionsAsked[QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS]; // Sorulan tüm soruları tutacak dizi

int currentQuestionInBlockIndex = 0; // Şu anki bloktaki sorunun indeksi (0-6)
int totalQuestionsAsked = 0;         // Başından beri sorulan toplam soru sayısı
String allGuess = "";
String geminiLastGuess = "";         // Gemini'nin son tahmini

// OLED ekranda mesaj göstermek için fonksiyon
void displayMessage(String title, String message, int delayMs = 0) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 0);
  display.print(title);

  display.setTextSize(1);
  display.getTextBounds(message, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 20);
  display.print(message);

  display.display();
  if (delayMs > 0) {
    delay(delayMs);
  }
}

// OLED ekranda kayan metin göstermek için fonksiyon (uzun tahminler için)
void displayTextScroll(String title, String text) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 0);
  display.print(title);

  display.setTextSize(1);
  int textWidth = text.length() * 6; // Yaklaşık karakter genişliği (6 piksel)

  display.setCursor(0, 20); // Metin başlangıç pozisyonu

  if (textWidth <= SCREEN_WIDTH) {
    // Metin ekrana sığıyorsa ortala
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 20);
    display.print(text);
    display.display();
    delay(2000); // Kayan metin olmadığı için sabit bir süre göster
  } else {
    // Metin ekrana sığmıyorsa kaydır
    for (int scrollPos = SCREEN_WIDTH; scrollPos > -textWidth; scrollPos -= 2) { // Sağdan başla sola kaydır
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(2);
      display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
      display.setCursor((SCREEN_WIDTH - w) / 2, 0);
      display.print(title);

      display.setTextSize(1);
      display.setCursor(scrollPos, 20);
      display.print(text);
      display.display();
      delay(50); // Kaydırma hızı
    }
    // Son pozisyonda biraz beklet
    delay(1000);
  }
}

// OLED ekranda soru göstermek için fonksiyon
void displayQuestion(int index) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Soru Sayısı Başlığı
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Soru " + String(totalQuestionsAsked + 1) + "/" + String(QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS) + ":");

  String questionText = questions[index];

  // Soru metni için y pozisyonu: Başlık (8 piksel) + biraz boşluk (4 piksel) = 12. piksel
  int questionTextYPos = 12;

  // Metnin ekranda kaplayacağı tahmini yüksekliği (piksel olarak) hesaplama:
  int textWidth = questionText.length() * 6; // Her karakter 6 piksel genişliğinde (yaklaşık)
  int estimatedLines = (textWidth + SCREEN_WIDTH - 1) / SCREEN_WIDTH; // Kaç tam ekran genişliği kaplar

  // Eğer metin 3 satırdan fazlaysa kaydır, aksi takdirde sabit göster
  if (estimatedLines > 3 || questionText.length() > 63) { // 3 satırı veya 63 karakteri aşarsa kaydır

    for (int scrollPos = SCREEN_WIDTH; scrollPos > -textWidth; scrollPos -= 2) {
      display.clearDisplay(); // Her kaydırmada ekranı temizle
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("Soru " + String(totalQuestionsAsked + 1) + "/" + String(QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS) + ":");

      display.setCursor(scrollPos, questionTextYPos); // Soru metninin kaydırma pozisyonu
      display.print(questionText);
      display.display();
      delay(50); // Kaydırma hızı
    }
    delay(1000); // Soru gösterildikten sonra bir süre bekle
  } else {
    // Metin 3 satıra veya daha azına sığıyorsa sabit göster
    display.setCursor(0, questionTextYPos); // Sol üstten başlama
    display.print(questionText); // Kütüphanenin kendi kelime kaydırma özelliğini kullanma
  }

  // Buton etiketleri (sorunun altında sabit kalır)
  display.setTextSize(1);
  String line1 = "Evet (1) | Hayir (2)";
  String line2 = "Emin Degilim (3)";

  int16_t x1_buttons, y1_buttons;
  uint16_t w_buttons, h_buttons;

  // Buton etiketlerini ortala
  display.getTextBounds(line1, 0, 0, &x1_buttons, &y1_buttons, &w_buttons, &h_buttons);
  display.setCursor((SCREEN_WIDTH - w_buttons) / 2, 40);
  display.print(line1);

  display.getTextBounds(line2, 0, 0, &x1_buttons, &y1_buttons, &w_buttons, &h_buttons);
  display.setCursor((SCREEN_WIDTH - w_buttons) / 2, 50);
  display.print(line2);

  display.display(); // Son hali göster
}

// Yeni soruları veya tahmini Gemini cevabından ayıklamak için yardımcı fonksiyon
String extractGeminiText(String response) {
  DynamicJsonDocument doc(2048); // JSON boyutu ihtiyaçlarınıza göre ayarlanabilir
  DeserializationError error = deserializeJson(doc, response);

  if (error) {
    Serial.print(F("deserializeJson() basarisiz oldu: "));
    Serial.println(error.f_str());
    return "";
  }

  // Cevabı temizle (sadece alfanumerik ve boşluk karakterleri bırak)
  String geminiAnswer = doc["candidates"][0]["content"]["parts"][0]["text"];
  geminiAnswer.trim(); // Baştaki ve sondaki boşlukları temizle

  String filteredAnswer = "";
  for (size_t i = 0; i < geminiAnswer.length(); i++) {
    char c = geminiAnswer[i];
    // İzin verilen karakterler: harfler, rakamlar, boşluk, nokta, virgül, ünlem, soru işareti, kesme işareti, tire
    if (isalnum(c) || isspace(c) || c == '.' || c == ',' || c == '!' || c == '?' || c == '\'' || c == '-') {
      filteredAnswer += c;
    } else {
      // Yeni satır karakterlerini boşluk olarak değiştir, diğerlerini filtrele
      if (c == '\n' || c == '\r') {
        filteredAnswer += ' ';
      }
      // Diğer özel karakterleri yok say
    }
  }
  return filteredAnswer;
}


// Gemini'ye istek gönderme fonksiyonu
String sendRequestToGemini(String prompt) {
  WiFiClientSecure client;
  // client.setFingerprint(fingerprint);
  client.setInsecure(); 

  HTTPClient https;
  String geminiApiUrl = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=" + String(Gemini_Token);

  if (!https.begin(client, geminiApiUrl)) { // Bağlantı başlatılamazsa hata döndür
    Serial.println("[HTTPS] Bağlantı kurulamadı");
    displayMessage("Hata!", "API'ye baglanilamadi.", 3000);
    return ""; // Hata durumunda boş string döndür
  }

  https.addHeader("Content-Type", "application/json");

  Serial.print("Gemini'ye Gonderilen Prompt: ");
  Serial.println(prompt);

  String payload = "{\"contents\": [{\"parts\":[{\"text\":\"" + prompt + "\"}]}],\"generationConfig\": {\"maxOutputTokens\": " + String(Gemini_Max_Tokens) + "}}";

  int httpCode = https.POST(payload);

  if (httpCode == HTTP_CODE_OK) {
    String response = https.getString();
    Serial.println("Gemini Cevabı (Ham): " + response);
    https.end();
    return response;
  } else {
    Serial.printf("[HTTPS] İstek başarısız oldu, hata: %s\n", https.errorToString(httpCode).c_str());
    displayMessage("Hata!", "API Hatasi: " + String(httpCode), 3000);
    https.end();
    return ""; // Hata durumunda boş string döndür
  }
}

void setup() {
  Serial.begin(115200);

  while (!Serial)
    delay(10);

  
  // OLED ekranı için I2C başlangıcı
  Wire.begin(OLED_SDA, OLED_SCL);

  // OLED ekranın başlatılması
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 tahsisi basarisiz oldu"));
    for (;;) delay(100); // Sonsuz döngüde kal, hata durumunda
  }
  display.setRotation(2); // Ekran yönünü ayarlama
  display.clearDisplay();
  display.display();

  // Buton pinlerinin giriş olarak ayarlanması (pull-up dirençleri ile)
  pinMode(BUTTON_YES_PIN, INPUT_PULLUP);
  pinMode(BUTTON_NO_PIN, INPUT_PULLUP);
  pinMode(BUTTON_NOT_SURE_PIN, INPUT_PULLUP);

  // Wi-Fi bağlantısı başlatılıyor
  displayMessage("", "Wi-Fi baglantisi bekleniyor...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
}

void loop() {
  switch (currentGameState) {
    case STATE_CONNECTING_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWi-Fi Baglandi");
        Serial.print("IP Adresi: ");
        Serial.println(WiFi.localIP());
        displayMessage("Baglandi!", "IP: " + WiFi.localIP().toString(), 2000);
        currentGameState = STATE_GAME_START;
      } else {
        Serial.print(".");
        // WiFi bağlantısı sırasında ekranı güncelle
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(64, 40); // Küçük bir nokta animasyonu için cursor'ı kaydır
        display.print(".");
        display.display();
        delay(1000);
      }
      break;

    case STATE_GAME_START:
      displayMessage("", "Akilli Cay Bardagi Basliyor..", 3000);
      // İlk 7 soruyu tanımla
      questions[0] = "Bu sey canli mi?";
      questions[1] = "Bu sey dunyamizin icerisinde mi?";
      questions[2] = "Bu bir insan mi?";
      questions[3] = "Bu sey insan urunu mu?";
      questions[4] = "Bu sey somut bir varlik mi?";
      questions[5] = "Bu sey belli bir amaca hizmet eder mi?";
      questions[6] = "Bu sey bir kavram veya duygu mu?";

      currentQuestionInBlockIndex = 0;
      totalQuestionsAsked = 0;
      // Tüm cevap ve soru dizilerini sıfırla
      for(int i=0; i < (QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS); i++){
        allAnswers[i] = "";
        allQuestionsAsked[i] = "";
      }
      geminiLastGuess = "";
      currentGameState = STATE_ASKING_QUESTION;
      break;

    case STATE_ASKING_QUESTION:
      if (currentQuestionInBlockIndex < QUESTIONS_PER_BLOCK) {
        displayQuestion(currentQuestionInBlockIndex);
        currentGameState = STATE_WAITING_FOR_ANSWER;
      } else {
        // Bloktaki tüm sorular soruldu, Gemini'ye gönder
        currentGameState = STATE_SENDING_TO_GEMINI;
      }
      break;

    case STATE_WAITING_FOR_ANSWER:
      if (millis() - lastButtonPressTime > debounceDelay) {
        bool buttonPressed = false;
        String currentAnswer = "";
        char serialChar = ' ';

        if (Serial.available()) {
          serialChar = Serial.read();
          while (Serial.available()) { // Seri porttaki diğer karakterleri temizle
            Serial.read();
          }
          serialChar = tolower(serialChar); // Büyük/küçük harf duyarsızlığı için küçük harfe çevir
        }

        if (digitalRead(BUTTON_YES_PIN) == LOW || serialChar == 'e') {
          currentAnswer = "Evet";
          buttonPressed = true;
        } else if (digitalRead(BUTTON_NO_PIN) == LOW || serialChar == 'h') {
          currentAnswer = "Hayir";
          buttonPressed = true;
        } else if (digitalRead(BUTTON_NOT_SURE_PIN) == LOW || serialChar == 'b') {
          currentAnswer = "Emin Degilim";
          buttonPressed = true;
        }

        if (buttonPressed) {
          allAnswers[totalQuestionsAsked] = currentAnswer;      // Tüm cevap dizisine kaydet
          allQuestionsAsked[totalQuestionsAsked] = questions[currentQuestionInBlockIndex]; // Sorulan soruyu da kaydet
          Serial.print("Cevap: ");
          Serial.println(currentAnswer);
          displayMessage("Cevap:", currentAnswer, 1000);

          currentQuestionInBlockIndex++; // Bloktaki soru indeksini artır
          totalQuestionsAsked++;          // Toplam soru sayısını artır
          lastButtonPressTime = millis();

          if (currentQuestionInBlockIndex == QUESTIONS_PER_BLOCK || totalQuestionsAsked == (QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS)) {
            // Blok sonu veya maksimum soru sayısına ulaşıldı, Gemini'ye gönder
            currentGameState = STATE_SENDING_TO_GEMINI;
          } else {
            currentGameState = STATE_ASKING_QUESTION; // Sonraki soruya geç
          }
        }
      }
      break;

    case STATE_SENDING_TO_GEMINI: {
      displayMessage("BARDAK", "DUSUNUYOR...");
      Serial.println("\nGemini'ye gonderiliyor...");

      String prompt = "";
      String allInteractionHistory = ""; // Tüm sorular ve cevaplar
      String filteredGeminiResponse = "";
      for (int i = 0; i < totalQuestionsAsked; i++) {
        allInteractionHistory += allQuestionsAsked[i] + ":" + allAnswers[i] + ", ";
      }

      if (!force_ask){
      // Gemini'den önce tahmin etmesini isteyelim, eğer emin değilse belirtmesini.
      // Toplam soru sayısına ulaşıldıysa son tahmini zorla.
      if (totalQuestionsAsked < (QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS)) {
        prompt = "I'm playing a 20 Questions game. Based on the following user responses to my questions so far: " + allInteractionHistory + " who do you think I am thinking of? If you are not certain, respond *only* with the phrase 'Daha fazla bilgiye ihtiyacim var'. If you are making a guess, state *only* the name of the person. Answer in Turkish, using only Latin characters (a-z, A-Z) and numbers (0-9). For Turkish letters like 'ç, ğ, ı, ö, ş, ü', use their Latin equivalents (c, g, i, o, s, u).";
      } else {
        prompt = "I have asked all " + String(QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS) + " questions. Based on the following user responses: " + allInteractionHistory + " make your final guess about who I am thinking of. State *only* the name of the person. Answer in Turkish, using only Latin characters (a-z, A-Z) and numbers (0-9). For Turkish letters like 'ç, ğ, ı, ö, ş, ü', use their Latin equivalents (c, g, i, o, s, u).";
      }

      String geminiResponseRaw = sendRequestToGemini(prompt); // Ham cevabı al
      filteredGeminiResponse = extractGeminiText(geminiResponseRaw); // Filtrelenmiş metni al

      if (filteredGeminiResponse.isEmpty()) {
        displayMessage("Hata!", "Gemini cevabi bos.", 3000);
        currentGameState = STATE_GAME_OVER;
        break;
      }
      }

      // Gemini'nin "Daha fazla bilgiye ihtiyacım var" yanıtını doğrudan kontrol et
      if ((filteredGeminiResponse.indexOf("Daha fazla bilgiye ihtiyacim var") != -1) || (force_ask)){
        Serial.println("Gemini emin degil, daha fazla soru sorulacak.");
        displayMessage("BARDAK", "EMIN DEGIL.", 2000);

        if ((force_ask) && (totalQuestionsAsked < (QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS))) {
          // Yeni sorular üretmesini istiyoruz
          displayMessage("SORULAR", "HAZIRLANIYOR", 1000);
          String newforcedQuestionsPrompt = "Based on the previous user responses to the 20 Questions game: " + allInteractionHistory + " Your previous wrong answers: " + allGuess + " . Generate 7 new, more specific yes/no questions in Turkish, using only Latin characters (a-z, A-Z) and numbers (0-9). For Turkish letters like 'ç, ğ, ı, ö, ş, ü', use their Latin equivalents (c, g, i, o, s, u). Ensure the questions are answerable with 'Evet', 'Hayir', or 'Emin Degilim'. Provide *only* the questions, one per line, without numbers or introductory phrases. Example: 'Bu kisi bir sporcu mu?'";
          String newforcedQuestionsResponseRaw = sendRequestToGemini(newforcedQuestionsPrompt);
          String extractedforcedQuestions = extractGeminiText(newforcedQuestionsResponseRaw);

          force_ask = false;
        
          // Yeni soruları ayrıştırma ve questions dizisine atama
            int questionCount = 0;
            int startIndex = 0;
            // Önce yeni satırları deneme
            for (int i = 0; i < extractedforcedQuestions.length(); i++) {
              if (extractedforcedQuestions.charAt(i) == '\n' || i == extractedforcedQuestions.length() - 1) { // Yeni satır veya string sonu
                String tempQ = extractedforcedQuestions.substring(startIndex, i + 1);
                tempQ.trim();
                String q = tempQ;

                if (q.length() > 5 && questionCount < QUESTIONS_PER_BLOCK) { // Minimum 5 karakter uzunluk kontrolü
                  questions[questionCount] = q;
                  Serial.println("Yeni Soru (Newline): " + questions[questionCount]);
                  questionCount++;
                }
                startIndex = i + 1;
                if (questionCount == QUESTIONS_PER_BLOCK) break;
              }
            }
        }

        if (totalQuestionsAsked < (QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS)) {
          // Yeni sorular üretmesini isteme
          displayMessage("SORULAR", "HAZIRLANIYOR", 1000);
          String newQuestionsPrompt = "Based on the previous user responses to the 20 Questions game: " + allInteractionHistory + " generate 7 new, more specific yes/no questions in Turkish, using only Latin characters (a-z, A-Z) and numbers (0-9). For Turkish letters like 'ç, ğ, ı, ö, ş, ü', use their Latin equivalents (c, g, i, o, s, u). Ensure the questions are answerable with 'Evet', 'Hayir', or 'Emin Degilim'. Provide *only* the questions, one per line, without numbers or introductory phrases. Example: 'Bu kisi bir sporcu mu?'";
          String newQuestionsResponseRaw = sendRequestToGemini(newQuestionsPrompt);
          String extractedQuestions = extractGeminiText(newQuestionsResponseRaw);

          if (extractedQuestions.isEmpty() || extractedQuestions.length() < 10) { // Çok kısa veya boş yanıtı da hata say
            displayMessage("Hata!", "Yeni soru alinmadi.", 3000);
            Serial.println("Yeterli yeni soru alinamadi, varsayilanlara dönuluyor.");
            // Fallback sorular
            questions[0] = "Bu kisi kamuoyunun bildigi biri mi?";
            questions[1] = "Bu kisinin meslegi sahne sanatlarina mi ait?";
            questions[2] = "Bu kisi gercekten yasiyor mu?";
            questions[3] = "Bu kisinin bir unvani var mi?";
            questions[4] = "Bu kisi bir devlet gorevlisi mi?";
            questions[5] = "Bu kisinin ismi iki kelimeden mi olusuyor?";
            questions[6] = "Bu kisi ile ilgili yeni bir gelisme var mi?";
          } else {
            // Yeni soruları ayrıştırma ve questions dizisine atama
            int questionCount = 0;
            int startIndex = 0;
            // Önce yeni satırları deneme
            for (int i = 0; i < extractedQuestions.length(); i++) {
              if (extractedQuestions.charAt(i) == '\n' || i == extractedQuestions.length() - 1) { // Yeni satır veya string sonu
                String tempQ = extractedQuestions.substring(startIndex, i + 1);
                tempQ.trim();
                String q = tempQ;

                if (q.length() > 5 && questionCount < QUESTIONS_PER_BLOCK) { // Minimum 5 karakter uzunluk kontrolü
                  questions[questionCount] = q;
                  Serial.println("Yeni Soru (Newline): " + questions[questionCount]);
                  questionCount++;
                }
                startIndex = i + 1;
                if (questionCount == QUESTIONS_PER_BLOCK) break;
              }
            }

            // Eğer hala 7 soru toplanamadıysa, nokta/soru işareti bazlı ayrıştırmayı deneme
            if (questionCount < QUESTIONS_PER_BLOCK) {
                Serial.println("Yeterli yeni soru ayrıştırılamadı (yeni satır bazlı), nokta/soru işareti bazlı deniyor.");
                questionCount = 0; // Sıfırlayıp ve yeniden deneme
                startIndex = 0;
                for (int i = 0; i < extractedQuestions.length(); i++) {
                    if (extractedQuestions.substring(i, i + 1) == "." || extractedQuestions.substring(i, i + 1) == "?" || extractedQuestions.substring(i, i + 1) == "!") {
                        String tempQ = extractedQuestions.substring(startIndex, i + 1);
                        tempQ.trim();
                        String q = tempQ;

                        if (q.length() > 0 && questionCount < QUESTIONS_PER_BLOCK) {
                            questions[questionCount] = q;
                            Serial.println("Yeni Soru (Punctuation): " + questions[questionCount]);
                            questionCount++;
                        }
                        startIndex = i + 1;
                        if (questionCount == QUESTIONS_PER_BLOCK) break;
                    }
                }
            }

            if (questionCount < QUESTIONS_PER_BLOCK) { // Nihai kontrol, hala eksikse fallback
                Serial.println("Nihai olarak yeterli soru ayrıştırılamadı, varsayılanlara dönülüyor.");
                questions[0] = "Bu kisi kamuoyunun bildigi biri mi?";
                questions[1] = "Bu kisinin meslegi sahne sanatlarina mi ait?";
                questions[2] = "Bu kisi gercekten yasiyor mu?";
                questions[3] = "Bu kisinin bir unvani var mi?";
                questions[4] = "Bu kisi bir devlet gorevlisi mi?";
                questions[5] = "Bu kisinin ismi iki kelimeden mi olusuyor?";
                questions[6] = "Bu kisi ile ilgili yeni bir gelisme var mi?";
            }
          }
          currentQuestionInBlockIndex = 0; // Yeni blok için indeksi sıfırlama
          currentGameState = STATE_ASKING_QUESTION; // Yeni soruları sormaya başla

        } 
        else {
          // Maksimum soru sayısına ulaşıldı ve Gemini hala bilemedi
          displayMessage("Oyun Bitti!", "Gemini tahmin edemedi.", 0);
          currentGameState = STATE_GAME_OVER;
        }

      } else {
        // Gemini bir tahmin yaptı (eminim dediği durum)
        geminiLastGuess = filteredGeminiResponse;
        Serial.println("\nGemini'nin Tahmini:\n");
        Serial.println(geminiLastGuess);
        displayTextScroll("Bu muydu?", geminiLastGuess); // Kayan metin gösterimi
        currentGameState = STATE_WAITING_FOR_GEMINI_CONFIRMATION; // Kullanıcı onayı bekle
      }
      break;
    }

    case STATE_WAITING_FOR_GEMINI_CONFIRMATION:
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);

      // Başlık
      display.setTextSize(2);
      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds("Bu muydu?", 0, 0, &x1, &y1, &w, &h);
      display.setCursor((SCREEN_WIDTH - w) / 2, 0);
      display.print("Bu muydu?");

      // Tahmin metni
      display.setTextSize(1);
      display.getTextBounds(geminiLastGuess, 0, 0, &x1, &y1, &w, &h);
      display.setCursor((SCREEN_WIDTH - w) / 2, 20);
      display.print(geminiLastGuess); // Tahmini ekranda göster

      // Buton etiketleri
      display.setTextSize(1);
      display.getTextBounds("Evet (D1/e) | Hayir (D2/h)", 0, 0, &x1, &y1, &w, &h);
      display.setCursor((SCREEN_WIDTH - w) / 2, 40);
      display.print("Evet (D1/e) | Hayir (D2/h)");
      display.display();

      if (millis() - lastButtonPressTime > debounceDelay) {
        char serialChar = ' ';
        if (Serial.available()) {
          serialChar = Serial.read();
          while (Serial.available()) {
            Serial.read();
          }
          serialChar = tolower(serialChar);
        }

        if (digitalRead(BUTTON_YES_PIN) == LOW || serialChar == 'e') {
          Serial.println("Kullanici: Evet, dogru tahmin!");
          displayMessage("Dogru Tahmin!", "Tebrikler Gemini!", 0);
          currentGameState = STATE_GAME_OVER;
          lastButtonPressTime = millis();
        } else if (digitalRead(BUTTON_NO_PIN) == LOW || serialChar == 'h') {
          Serial.println("Kullanici: Hayir, yanlis tahmin.");
          allGuess += " , " + geminiLastGuess;
          Serial.println(allGuess);
          lastButtonPressTime = millis();

          // Eğer yanlış tahmin edildiyse ve hala soru hakkı varsa
          if (totalQuestionsAsked < (QUESTIONS_PER_BLOCK * MAX_QUESTION_BLOCKS)) {
            // Yeni sorular üretmesi için Gemini'ye geri gönder
            displayMessage("Yanlis Tahmin!", "Yeni sorular bekleniyor...", 2000);
            force_ask = true;
            currentGameState = STATE_ASKING_QUESTION;
          } else {
            // Maksimum soruya ulaşıldı ve Gemini hala bilemedi
            displayMessage("Oyun Bitti!", "Gemini tahmin edemedi.", 0);
            currentGameState = STATE_GAME_OVER;
          }
        }
      }
      break;

    case STATE_GAME_OVER:
      displayMessage("Oyun Bitti!", "Tekrar oynamak icin resetle.", 0); // Süresiz göster
      while(true) {
        // Oyun bitince burada kal
        delay(100);
      }
      break;
  }
}
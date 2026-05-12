#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

String currentTitle = "Waiting...";
String currentArtist = "";
int currentDuration = 0;
int currentPosition = 0;

#define THUMBNAIL_SIZE 12800
uint8_t thumbnailBuffer[THUMBNAIL_SIZE];
bool hasThumbnail = false;

int scrollOffset = 0;
unsigned long lastScrollTime = 0;
String fullTitle = "";

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Starting...");
  
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  drawWaitingScreen();
}

void drawWaitingScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 120);
  tft.println("Waiting for");
  tft.setCursor(10, 150);
  tft.println("music...");
}

void loop() {
  if (Serial.available()) {
    readSerialData();
  }
  
  updateDisplay();
  delay(50);
}

void readSerialData() {
  Serial.println("Reading data...");
  
  String header = "";
  unsigned long timeout = millis() + 3000;
  
  while (millis() < timeout) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') break;
      header += c;
    }
    delay(1);
  }
  
  if (header.length() > 0) {
    int firstPipe = header.indexOf('|');
    int secondPipe = header.indexOf('|', firstPipe + 1);
    int thirdPipe = header.indexOf('|', secondPipe + 1);
    int fourthPipe = header.indexOf('|', thirdPipe + 1);
    
    if (firstPipe > 0 && secondPipe > 0) {
      currentTitle = header.substring(0, firstPipe);
      currentArtist = header.substring(firstPipe + 1, secondPipe);
      currentDuration = header.substring(secondPipe + 1, thirdPipe).toInt();
      currentPosition = header.substring(thirdPipe + 1, fourthPipe).toInt();
      
      Serial.print("Title: ");
      Serial.println(currentTitle);
      Serial.print("Artist: ");
      Serial.println(currentArtist);
      
      fullTitle = currentTitle;
      scrollOffset = 0;
      
      // Read thumbnail
      int bytesRead = 0;
      while (bytesRead < THUMBNAIL_SIZE && millis() < timeout + 2000) {
        if (Serial.available()) {
          thumbnailBuffer[bytesRead++] = Serial.read();
        }
      }
      
      hasThumbnail = (bytesRead == THUMBNAIL_SIZE);
      Serial.print("Thumbnail: ");
      Serial.println(hasThumbnail ? "OK" : "FAIL");
      
      // Check if thumbnail has actual data (not all zeros)
      if (hasThumbnail) {
        bool allZero = true;
        for (int i = 0; i < 100; i++) {
          if (thumbnailBuffer[i] != 0) {
            allZero = false;
            break;
          }
        }
        if (allZero) {
          Serial.println("Thumbnail is all zeros!");
          hasThumbnail = false;
        }
      }
      
      drawMainScreen();
    }
  }
}

void drawMainScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Draw thumbnail in center
  int thumbX = (tft.width() - 80) / 2;
  int thumbY = 10;
  
  if (hasThumbnail) {
    // Draw RGB565 image pixel by pixel
    for (int y = 0; y < 80; y++) {
      for (int x = 0; x < 80; x++) {
        int idx = (y * 80 + x) * 2;
        uint16_t color = thumbnailBuffer[idx] | (thumbnailBuffer[idx + 1] << 8);
        tft.drawPixel(thumbX + x, thumbY + y, color);
      }
    }
    Serial.println("Thumbnail drawn");
  } else {
    // Draw placeholder
    tft.fillRect(thumbX, thumbY, 80, 80, TFT_DARKGREY);
    tft.drawRect(thumbX, thumbY, 80, 80, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(1);
    tft.setCursor(thumbX + 20, thumbY + 35);
    tft.println("NO");
    tft.setCursor(thumbX + 15, thumbY + 50);
    tft.println("IMAGE");
  }
  
  // Title (scrolling if needed)
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 110);
  
  String displayTitle = fullTitle;
  if (displayTitle.length() > 24) {
    // Simple truncation for now
    displayTitle = displayTitle.substring(0, 22) + "...";
  }
  tft.println(displayTitle);
  
  // Artist
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 140);
  
  String displayArtist = currentArtist;
  if (displayArtist.length() > 35) {
    displayArtist = displayArtist.substring(0, 32) + "...";
  }
  tft.println(displayArtist);
  
  // Progress bar
  int barWidth = tft.width() - 20;
  int barX = 10;
  int barY = 170;
  int barHeight = 8;
  
  tft.fillRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
  
  if (currentDuration > 0) {
    int progressWidth = (currentPosition * barWidth) / currentDuration;
    if (progressWidth > 0) {
      tft.fillRect(barX, barY, progressWidth, barHeight, TFT_GREEN);
    }
  }
  
  // Time
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 185);
  tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
  
  // Footer
  tft.fillRect(0, tft.height() - 25, tft.width(), 25, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setCursor(10, tft.height() - 18);
  tft.println("♪ NOW PLAYING ♪");
}

void updateDisplay() {
  // Only update progress bar and time
  if (currentDuration > 0) {
    static int lastPos = -1;
    if (currentPosition != lastPos) {
      int barWidth = tft.width() - 20;
      int barX = 10;
      int barY = 170;
      int barHeight = 8;
      
      int progressWidth = (currentPosition * barWidth) / currentDuration;
      
      tft.fillRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
      if (progressWidth > 0) {
        tft.fillRect(barX, barY, progressWidth, barHeight, TFT_GREEN);
      }
      
      tft.fillRect(10, 185, 200, 15, TFT_BLACK);
      tft.setCursor(10, 185);
      tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
      
      lastPos = currentPosition;
    }
  }
}

String formatTime(int seconds) {
  if (seconds < 0) seconds = 0;
  int mins = seconds / 60;
  int secs = seconds % 60;
  String result = "";
  if (mins < 10) result += "0";
  result += String(mins) + ":";
  if (secs < 10) result += "0";
  result += String(secs);
  return result;
}
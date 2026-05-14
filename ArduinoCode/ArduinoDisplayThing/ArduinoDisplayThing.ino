#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

// SLIP Protocol definitions
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

// Packet buffer
#define MAX_PACKET_SIZE 15000
uint8_t packetBuffer[MAX_PACKET_SIZE];
int packetIndex = 0;
bool inPacket = false;

// Current song data
String currentTitle = "Waiting...";
String currentArtist = "";
int currentDuration = 0;
int currentPosition = 0;

// Thumbnail buffer (80x80 RGB565 = 12800 bytes)
#define THUMBNAIL_SIZE 12800
uint8_t thumbnailBuffer[THUMBNAIL_SIZE];
bool hasThumbnail = false;

// Scrolling text variables
int scrollOffset = 0;
unsigned long lastScrollTime = 0;
String fullTitle = "";

void setup() {
  delay(2000);  // Wait for PC to open serial port
  
  Serial.begin(115200);
  Serial.setTimeout(5000);
  
  Serial.println("ESP32 Starting with SLIP Protocol...");
  
  // Backlight ON for Cheap Yellow Display
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  drawWaitingScreen();
  
  // Flash LED to show ready
  pinMode(4, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(4, HIGH);
    delay(100);
    digitalWrite(4, LOW);
    delay(100);
  }
  
  Serial.println("SLIP Ready - Waiting for packets...");
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
  // Process incoming serial data
  while (Serial.available()) {
    uint8_t c = Serial.read();
    
    if (c == SLIP_END) {
      if (inPacket && packetIndex > 0) {
        // Decode and process packet
        processSlipPacket(packetBuffer, packetIndex);
        packetIndex = 0;
        inPacket = false;
      } else {
        inPacket = true;
        packetIndex = 0;
      }
    } else if (inPacket) {
      if (packetIndex < MAX_PACKET_SIZE - 1) {
        packetBuffer[packetIndex++] = c;
      }
    }
  }
  
  // Update scrolling text
  updateScrollingText();
  
  // Update progress bar
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 100 && currentDuration > 0) {
    updateProgressDisplay();
    lastUpdate = millis();
  }
  
  delay(10);
}

void processSlipPacket(uint8_t* data, int len) {
  // Decode SLIP data
  uint8_t decoded[MAX_PACKET_SIZE];
  int decodedLen = 0;
  
  for (int i = 0; i < len; i++) {
    if (data[i] == SLIP_ESC) {
      if (i + 1 < len) {
        if (data[i + 1] == SLIP_ESC_END) {
          decoded[decodedLen++] = SLIP_END;
          i++;
        } else if (data[i + 1] == SLIP_ESC_ESC) {
          decoded[decodedLen++] = SLIP_ESC;
          i++;
        }
      }
    } else {
      decoded[decodedLen++] = data[i];
    }
  }
  
  // Parse packet - find null separator between header and thumbnail
  int separator = -1;
  for (int i = 0; i < decodedLen; i++) {
    if (decoded[i] == 0x00) {
      separator = i;
      break;
    }
  }
  
  if (separator > 0) {
    // Extract header (before null)
    String header = "";
    for (int i = 0; i < separator; i++) {
      header += (char)decoded[i];
    }
    
    // Parse header: TITLE|ARTIST|DURATION|POSITION
    int firstPipe = header.indexOf('|');
    int secondPipe = header.indexOf('|', firstPipe + 1);
    int thirdPipe = header.indexOf('|', secondPipe + 1);
    
    if (firstPipe > 0 && secondPipe > 0 && thirdPipe > 0) {
      currentTitle = header.substring(0, firstPipe);
      currentArtist = header.substring(firstPipe + 1, secondPipe);
      currentDuration = header.substring(secondPipe + 1, thirdPipe).toInt();
      currentPosition = header.substring(thirdPipe + 1).toInt();
      
      fullTitle = currentTitle;
      scrollOffset = 0;
      
      // Extract thumbnail (after null)
      int thumbnailOffset = separator + 1;
      int thumbnailLen = decodedLen - thumbnailOffset;
      
      if (thumbnailLen == THUMBNAIL_SIZE) {
        memcpy(thumbnailBuffer, decoded + thumbnailOffset, THUMBNAIL_SIZE);
        
        // Verify thumbnail has data
        int nonZeroCount = 0;
        for (int i = 0; i < 100; i++) {
          if (thumbnailBuffer[i] != 0) nonZeroCount++;
        }
        
        hasThumbnail = (nonZeroCount > 10);
        
        // Send acknowledgment
        Serial.write(SLIP_END);
        String ack = "THUMB_OK";
        for (int i = 0; i < ack.length(); i++) {
          sendSlipByte(ack[i]);
        }
        Serial.write(SLIP_END);
      } else {
        hasThumbnail = false;
        
        // Send error
        Serial.write(SLIP_END);
        String err = "THUMB_FAIL";
        for (int i = 0; i < err.length(); i++) {
          sendSlipByte(err[i]);
        }
        Serial.write(SLIP_END);
      }
      
      drawMainScreen();
      
      // Flash LED for activity
      digitalWrite(4, HIGH);
      delay(100);
      digitalWrite(4, LOW);
      
      // Send success status
      Serial.write(SLIP_END);
      String status = "OK";
      for (int i = 0; i < status.length(); i++) {
        sendSlipByte(status[i]);
      }
      Serial.write(SLIP_END);
    }
  }
}

void sendSlipByte(uint8_t b) {
  if (b == SLIP_END) {
    Serial.write(SLIP_ESC);
    Serial.write(SLIP_ESC_END);
  } else if (b == SLIP_ESC) {
    Serial.write(SLIP_ESC);
    Serial.write(SLIP_ESC_ESC);
  } else {
    Serial.write(b);
  }
}

void drawMainScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Draw thumbnail (centered, 80x80)
  int thumbX = (tft.width() - 80) / 2;
  int thumbY = 10;
  
  if (hasThumbnail) {
    // Draw RGB565 image
    for (int y = 0; y < 80; y++) {
      for (int x = 0; x < 80; x++) {
        int idx = (y * 80 + x) * 2;
        if (idx + 1 < THUMBNAIL_SIZE) {
          uint16_t color = thumbnailBuffer[idx] | (thumbnailBuffer[idx + 1] << 8);
          tft.drawPixel(thumbX + x, thumbY + y, color);
        }
      }
    }
  } else {
    // Draw placeholder
    tft.fillRect(thumbX, thumbY, 80, 80, TFT_DARKGREY);
    tft.drawRect(thumbX, thumbY, 80, 80, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(1);
    tft.setCursor(thumbX + 20, thumbY + 35);
    tft.println("NO");
    tft.setCursor(thumbX + 15, thumbY + 50);
    tft.println("ART");
  }
  
  // Title
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 110);
  
  String displayTitle = fullTitle;
  if (displayTitle.length() > 24) {
    displayTitle = displayTitle.substring(0, 22) + "...";
  }
  tft.println(displayTitle);
  
  // Artist
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 145);
  
  String displayArtist = currentArtist;
  if (displayArtist.length() > 35) {
    displayArtist = displayArtist.substring(0, 32) + "...";
  }
  tft.println(displayArtist);
  
  // Progress bar background
  int barWidth = tft.width() - 20;
  int barX = 10;
  int barY = 175;
  int barHeight = 8;
  tft.fillRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
  
  // Draw progress
  if (currentDuration > 0) {
    int progressWidth = (currentPosition * barWidth) / currentDuration;
    if (progressWidth > 0) {
      tft.fillRect(barX, barY, progressWidth, barHeight, TFT_GREEN);
    }
  }
  
  // Time text
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 195);
  tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
  
  // Footer
  tft.fillRect(0, tft.height() - 25, tft.width(), 25, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setCursor(10, tft.height() - 18);
  tft.println("♪ NOW PLAYING ♪");
}

void updateProgressDisplay() {
  static int lastPosition = -1;
  
  if (currentPosition != lastPosition && currentDuration > 0) {
    int barWidth = tft.width() - 20;
    int barX = 10;
    int barY = 175;
    int barHeight = 8;
    
    int progressWidth = (currentPosition * barWidth) / currentDuration;
    
    tft.fillRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
    if (progressWidth > 0) {
      tft.fillRect(barX, barY, progressWidth, barHeight, TFT_GREEN);
    }
    
    tft.fillRect(10, 195, 150, 15, TFT_BLACK);
    tft.setCursor(10, 195);
    tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
    
    lastPosition = currentPosition;
  }
}

void updateScrollingText() {
  if (fullTitle.length() > 24) {
    static unsigned long lastScroll = 0;
    if (millis() - lastScroll > 200) {
      scrollOffset++;
      if (scrollOffset > fullTitle.length() - 24) {
        scrollOffset = 0;
      }
      lastScroll = millis();
      
      String displayTitle = fullTitle.substring(scrollOffset, scrollOffset + 24);
      tft.fillRect(10, 105, tft.width() - 20, 25, TFT_BLACK);
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setTextSize(2);
      tft.setCursor(10, 110);
      tft.println(displayTitle);
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
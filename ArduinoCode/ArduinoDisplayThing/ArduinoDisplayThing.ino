#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

String currentTitle = "Waiting...";
String currentArtist = "";
int currentDuration = 0;
int currentPosition = 0;

#define THUMBNAIL_SIZE 12800
uint8_t thumbnailBuffer[THUMBNAIL_SIZE];
bool hasThumbnail = false;

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Starting...");
  
  // Backlight ON
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
  
  // Update progress bar if we have a song
  if (currentDuration > 0) {
    updateProgressDisplay();
  }
  
  delay(50);
}

void readSerialData() {
  Serial.println("\n=== Reading Serial Data ===");
  
  // Clear buffer
  String header = "";
  unsigned long timeout = millis() + 5000;
  bool foundNewline = false;
  
  // Read header until newline
  while (millis() < timeout) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        foundNewline = true;
        Serial.println("Found newline, header complete");
        break;
      }
      header += c;
    }
    delay(1);
  }
  
  if (!foundNewline) {
    Serial.println("Timeout waiting for newline");
    return;
  }
  
  Serial.print("Header: ");
  Serial.println(header);
  
  // Parse header
  int firstPipe = header.indexOf('|');
  int secondPipe = header.indexOf('|', firstPipe + 1);
  int thirdPipe = header.indexOf('|', secondPipe + 1);
  int fourthPipe = header.indexOf('|', thirdPipe + 1);
  
  if (firstPipe > 0 && secondPipe > 0 && thirdPipe > 0 && fourthPipe > 0) {
    currentTitle = header.substring(0, firstPipe);
    currentArtist = header.substring(firstPipe + 1, secondPipe);
    currentDuration = header.substring(secondPipe + 1, thirdPipe).toInt();
    currentPosition = header.substring(thirdPipe + 1, fourthPipe).toInt();
    
    Serial.println("Parsed values:");
    Serial.print("  Title: ");
    Serial.println(currentTitle);
    Serial.print("  Artist: ");
    Serial.println(currentArtist);
    Serial.print("  Duration: ");
    Serial.println(currentDuration);
    Serial.print("  Position: ");
    Serial.println(currentPosition);
    
    // Read thumbnail data
    Serial.print("Reading thumbnail (expecting ");
    Serial.print(THUMBNAIL_SIZE);
    Serial.println(" bytes)...");
    
    int bytesRead = 0;
    timeout = millis() + 3000;
    
    while (bytesRead < THUMBNAIL_SIZE && millis() < timeout) {
      if (Serial.available()) {
        thumbnailBuffer[bytesRead++] = Serial.read();
      }
    }
    
    Serial.print("Bytes read: ");
    Serial.println(bytesRead);
    
    if (bytesRead == THUMBNAIL_SIZE) {
      hasThumbnail = true;
      
      // Verify the thumbnail data isn't all zeros
      int nonZeroCount = 0;
      for (int i = 0; i < 100; i++) {
        if (thumbnailBuffer[i] != 0) nonZeroCount++;
      }
      
      Serial.print("Non-zero bytes in first 100: ");
      Serial.println(nonZeroCount);
      
      // Print first 20 bytes for debugging
      Serial.print("First 20 bytes: ");
      for (int i = 0; i < 20; i++) {
        Serial.print(thumbnailBuffer[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
      
      if (nonZeroCount == 0) {
        Serial.println("WARNING: Thumbnail data is all zeros!");
        hasThumbnail = false;
      } else {
        Serial.println("Thumbnail data looks valid!");
      }
    } else {
      Serial.println("ERROR: Didn't receive enough thumbnail bytes!");
      hasThumbnail = false;
    }
    
    // Draw the main screen
    drawMainScreen();
    
    // Flash LED to show we got data
    digitalWrite(4, HIGH);
    delay(200);
    digitalWrite(4, LOW);
  } else {
    Serial.println("Failed to parse header");
  }
}

void drawMainScreen() {
  Serial.println("Drawing main screen...");
  tft.fillScreen(TFT_BLACK);
  
  // Draw thumbnail
  int thumbX = (tft.width() - 80) / 2;
  int thumbY = 10;
  
  if (hasThumbnail) {
    Serial.println("Drawing thumbnail from buffer...");
    
    // Draw a colored border around thumbnail area to confirm positioning
    tft.drawRect(thumbX - 1, thumbY - 1, 82, 82, TFT_RED);
    
    // Draw the RGB565 image
    for (int y = 0; y < 80; y++) {
      for (int x = 0; x < 80; x++) {
        int idx = (y * 80 + x) * 2;
        if (idx + 1 < THUMBNAIL_SIZE) {
          uint16_t color = thumbnailBuffer[idx] | (thumbnailBuffer[idx + 1] << 8);
          tft.drawPixel(thumbX + x, thumbY + y, color);
        }
      }
    }
    Serial.println("Thumbnail drawing complete");
    
    // Draw a test pattern to verify display working
    tft.fillRect(thumbX + 30, thumbY + 30, 20, 20, TFT_RED);
  } else {
    Serial.println("No thumbnail, drawing placeholder");
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
  
  String displayTitle = currentTitle;
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
  
  Serial.println("Screen draw complete");
}

void updateProgressDisplay() {
  static int lastPosition = -1;
  static unsigned long lastUpdate = 0;
  
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();
  
  if (currentPosition != lastPosition && currentDuration > 0) {
    int barWidth = tft.width() - 20;
    int barX = 10;
    int barY = 175;
    int barHeight = 8;
    
    int progressWidth = (currentPosition * barWidth) / currentDuration;
    
    // Update progress bar
    tft.fillRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
    if (progressWidth > 0) {
      tft.fillRect(barX, barY, progressWidth, barHeight, TFT_GREEN);
    }
    
    // Update time
    tft.fillRect(10, 195, 150, 15, TFT_BLACK);
    tft.setCursor(10, 195);
    tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
    
    lastPosition = currentPosition;
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
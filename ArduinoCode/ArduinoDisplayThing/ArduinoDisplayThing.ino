#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

String currentTitle = "Waiting...";
String currentArtist = "";
int currentDuration = 0;
int currentPosition = 0;

#define THUMBNAIL_SIZE 12800
uint8_t thumbnailBuffer[THUMBNAIL_SIZE];
bool hasThumbnail = false;

// Simple state machine
enum State {
  WAITING_FOR_START,
  READING_METADATA,
  READING_THUMBNAIL
};

State currentState = WAITING_FOR_START;
String metadataBuffer = "";
int thumbnailBytesRead = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Starting...");
  
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  drawWaitingScreen();
  
  pinMode(4, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(4, HIGH);
    delay(100);
    digitalWrite(4, LOW);
    delay(100);
  }
  
  Serial.println("Ready to receive data");
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
    processIncomingData();
  }
  
  if (currentDuration > 0) {
    updateProgressDisplay();
  }
  
  delay(10);
}

void processIncomingData() {
  while (Serial.available()) {
    char c = Serial.read();
    
    switch (currentState) {
      case WAITING_FOR_START:
        // Wait for 'S' character as start marker
        if (c == 'S') {
          currentState = READING_METADATA;
          metadataBuffer = "";
          thumbnailBytesRead = 0;
          Serial.println("Start marker received");
        }
        break;
        
      case READING_METADATA:
        if (c == '\n') {
          // End of metadata, switch to thumbnail reading
          parseMetadata(metadataBuffer);
          currentState = READING_THUMBNAIL;
          Serial.println("Metadata received, waiting for thumbnail...");
        } else {
          metadataBuffer += c;
        }
        break;
        
      case READING_THUMBNAIL:
        // Read thumbnail data directly into buffer
        if (thumbnailBytesRead < THUMBNAIL_SIZE) {
          thumbnailBuffer[thumbnailBytesRead++] = c;
          
          if (thumbnailBytesRead == THUMBNAIL_SIZE) {
            hasThumbnail = true;
            currentState = WAITING_FOR_START;
            Serial.println("Thumbnail received complete!");
            
            // Draw the screen with new data
            drawMainScreen();
            
            // Flash LED to show we got data
            digitalWrite(4, HIGH);
            delay(200);
            digitalWrite(4, LOW);
            
            Serial.println("Ready for next song");
          }
        }
        break;
    }
  }
}

void parseMetadata(String metadata) {
  Serial.print("Metadata: ");
  Serial.println(metadata);
  
  int firstPipe = metadata.indexOf('|');
  int secondPipe = metadata.indexOf('|', firstPipe + 1);
  int thirdPipe = metadata.indexOf('|', secondPipe + 1);
  
  if (firstPipe > 0 && secondPipe > 0 && thirdPipe > 0) {
    currentTitle = metadata.substring(0, firstPipe);
    currentArtist = metadata.substring(firstPipe + 1, secondPipe);
    currentDuration = metadata.substring(secondPipe + 1, thirdPipe).toInt();
    currentPosition = metadata.substring(thirdPipe + 1).toInt();
    
    Serial.println("Parsed values:");
    Serial.print("  Title: ");
    Serial.println(currentTitle);
    Serial.print("  Artist: ");
    Serial.println(currentArtist);
    Serial.print("  Duration: ");
    Serial.println(currentDuration);
    Serial.print("  Position: ");
    Serial.println(currentPosition);
  } else {
    Serial.println("Failed to parse metadata!");
  }
}

void drawMainScreen() {
  Serial.println("Drawing main screen...");
  tft.fillScreen(TFT_BLACK);
  
  int thumbX = (tft.width() - 80) / 2;
  int thumbY = 10;
  
  if (hasThumbnail) {
    Serial.println("Drawing thumbnail...");
    tft.drawRect(thumbX - 1, thumbY - 1, 82, 82, TFT_RED);
    
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
  } else {
    Serial.println("No thumbnail available");
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
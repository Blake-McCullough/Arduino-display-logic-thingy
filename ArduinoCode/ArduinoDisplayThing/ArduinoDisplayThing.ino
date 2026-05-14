#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

String currentTitle = "Waiting...";
String currentArtist = "";
String currentSource = "";
int currentDuration = 0;
int currentPosition = 0;
unsigned long lastDataReceived = 0;
unsigned long lastPositionUpdate = 0;
bool isPlaying = false;

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

// Timeout settings
#define DATA_TIMEOUT_MS 30000  // 30 seconds timeout
#define POSITION_UPDATE_INTERVAL_MS 1000  // Update position every second

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
  lastDataReceived = millis();
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
  // Check for timeout - if no data received for 30 seconds, reset to waiting state
  if (millis() - lastDataReceived > DATA_TIMEOUT_MS) {
    if (currentState != WAITING_FOR_START) {
      Serial.println("Data timeout - resetting to waiting state");
      resetToWaitingState();
    }
  }
  
  if (Serial.available()) {
    processIncomingData();
    lastDataReceived = millis(); // Reset timeout on any data received
  }
  
  // Update position if we have a song
  if (currentDuration > 0 && isPlaying) {
    updatePosition();
  }
  
  delay(10);
}

void resetToWaitingState() {
  currentState = WAITING_FOR_START;
  metadataBuffer = "";
  thumbnailBytesRead = 0;
  hasThumbnail = false;
  isPlaying = false;
  currentDuration = 0;
  currentPosition = 0;
  currentTitle = "Waiting...";
  currentArtist = "";
  currentSource = "";
  drawWaitingScreen();
  Serial.println("Reset complete - waiting for new song");
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
            isPlaying = true;
            lastPositionUpdate = millis();
            lastDataReceived = millis();
            
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

void updatePosition() {
  unsigned long currentTime = millis();
  
  // Update position every second
  if (currentTime - lastPositionUpdate >= POSITION_UPDATE_INTERVAL_MS) {
    // Increment position by 1 second
    if (currentPosition < currentDuration) {
      currentPosition++;
      lastPositionUpdate = currentTime;
      
      // Update the display
      updateProgressDisplay();
      
      // Check if song has ended
      if (currentPosition >= currentDuration) {
        Serial.println("Song ended");
        isPlaying = false;
      }
    }
  }
}

void parseMetadata(String metadata) {
  Serial.print("Metadata: ");
  Serial.println(metadata);
  
  // Parse 5 fields now (including source)
  int firstPipe = metadata.indexOf('|');
  int secondPipe = metadata.indexOf('|', firstPipe + 1);
  int thirdPipe = metadata.indexOf('|', secondPipe + 1);
  int fourthPipe = metadata.indexOf('|', thirdPipe + 1);
  
  if (firstPipe > 0 && secondPipe > 0 && thirdPipe > 0 && fourthPipe > 0) {
    currentTitle = metadata.substring(0, firstPipe);
    currentArtist = metadata.substring(firstPipe + 1, secondPipe);
    currentDuration = metadata.substring(secondPipe + 1, thirdPipe).toInt();
    currentPosition = metadata.substring(thirdPipe + 1, fourthPipe).toInt();
    currentSource = metadata.substring(fourthPipe + 1);
    
    // Clean up source string (remove common prefixes)
    currentSource.replace("Microsoft.", "");
    currentSource.replace("AppleInc.", "Apple ");
    if (currentSource.length() > 15) {
      currentSource = currentSource.substring(0, 12) + "...";
    }
    
    // Reset position tracking
    lastPositionUpdate = millis();
    isPlaying = true;
    
    Serial.println("Parsed values:");
    Serial.print("  Title: ");
    Serial.println(currentTitle);
    Serial.print("  Artist: ");
    Serial.println(currentArtist);
    Serial.print("  Duration: ");
    Serial.println(currentDuration);
    Serial.print("  Position: ");
    Serial.println(currentPosition);
    Serial.print("  Source: ");
    Serial.println(currentSource);
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
  
  // Initial progress bar
  if (currentDuration > 0) {
    int progressWidth = (currentPosition * barWidth) / currentDuration;
    if (progressWidth > 0) {
      tft.fillRect(barX, barY, progressWidth, barHeight, TFT_GREEN);
    }
  }
  
  // Time text (left side)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 195);
  tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
  
  // Source text (right side)
  if (currentSource.length() > 0) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextSize(1);
    int sourceX = tft.width() - 10 - (currentSource.length() * 6); // Approximate width
    if (sourceX < 120) sourceX = 120; // Don't overlap with time
    tft.setCursor(sourceX, 195);
    tft.println(currentSource);
  }
  
  Serial.println("Screen draw complete");
}

void updateProgressDisplay() {
  static int lastDisplayedPosition = -1;
  
  // Only update if position has changed
  if (currentPosition != lastDisplayedPosition && currentDuration > 0) {
    int barWidth = tft.width() - 20;
    int barX = 10;
    int barY = 175;
    int barHeight = 8;
    
    // Calculate progress
    int progressWidth = (currentPosition * barWidth) / currentDuration;
    
    // Update progress bar
    tft.fillRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
    if (progressWidth > 0) {
      tft.fillRect(barX, barY, progressWidth, barHeight, TFT_GREEN);
    }
    
    // Update time text
    tft.fillRect(10, 195, 100, 15, TFT_BLACK);
    tft.setCursor(10, 195);
    tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
    
    lastDisplayedPosition = currentPosition;
    
    // Optional: Print debug info occasionally
    static int debugCounter = 0;
    if (debugCounter++ % 10 == 0) {
      Serial.print("Position updated: ");
      Serial.print(currentPosition);
      Serial.print("/");
      Serial.println(currentDuration);
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
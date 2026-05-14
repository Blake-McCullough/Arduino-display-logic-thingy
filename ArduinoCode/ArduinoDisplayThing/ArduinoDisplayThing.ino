#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

// Current values
String currentTitle = "";
String currentArtist = "";
String currentSource = "";
int currentDuration = 0;
int currentPosition = 0;
int initialPosition = 0;
bool isPlaying = false;
bool hasValidSong = false;
bool songEnded = false;  // New flag to track if song has ended

// Previous values for change detection
String lastTitle = "";
String lastArtist = "";
String lastSource = "";
int lastDuration = -1;
int lastPosition = -1;
bool lastIsPlaying = false;
bool lastHasThumbnail = false;
String currentSongKey = "";
String lastSongKey = "";

// Scrolling text variables
String scrollingTitle = "";
String scrollingArtist = "";
int titleScrollPos = 0;
int artistScrollPos = 0;
unsigned long lastScrollTime = 0;
unsigned long lastScrollUpdate = 0;
#define SCROLL_DELAY_MS 500  // Speed of scrolling (lower = faster)
#define SCROLL_PADDING 5     // Extra spaces for smooth scrolling

// Time tracking
unsigned long lastDataReceived = 0;
unsigned long lastPositionUpdate = 0;
unsigned long songStartTime = 0;
unsigned long pausedTime = 0;
unsigned long lastPauseTime = 0;

#define THUMBNAIL_SIZE 12800
uint8_t thumbnailBuffer[THUMBNAIL_SIZE];
uint8_t lastThumbnailBuffer[THUMBNAIL_SIZE];
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
#define DATA_TIMEOUT_MS 30000
#define POSITION_UPDATE_INTERVAL_MS 500

// Display dimensions
const int SCREEN_WIDTH = 320;
const int SCREEN_HEIGHT = 240;
const int THUMB_X = (SCREEN_WIDTH - 80) / 2;
const int THUMB_Y = 10;
const int TITLE_X = 10;
const int TITLE_Y = 110;
const int ARTIST_X = 10;
const int ARTIST_Y = 145;
const int BAR_X = 10;
const int BAR_Y = 175;
const int BAR_WIDTH = 300;
const int BAR_HEIGHT = 8;
const int TIME_X = 10;
const int TIME_Y = 195;

// Max characters that fit on screen (adjust based on your font)
const int MAX_TITLE_CHARS = 24;  // Characters that fit on screen at text size 2
const int MAX_ARTIST_CHARS = 35;  // Characters that fit on screen at text size 1

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Starting...");
  
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.invertDisplay(true);
  
  showWaitingScreen();
  
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

void showWaitingScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 120);
  tft.println("Waiting for");
  tft.setCursor(10, 150);
  tft.println("music...");
  
  hasValidSong = false;
  currentSongKey = "";
  lastSongKey = "";
  songEnded = false;
}

void renderFullScreen() {
  Serial.println("Rendering full screen for new song");
  tft.fillScreen(TFT_BLACK);
  
  // Draw thumbnail border
  tft.drawRect(THUMB_X - 1, THUMB_Y - 1, 82, 82, TFT_RED);
  
  // Draw thumbnail
  if (hasThumbnail) {
    for (int y = 0; y < 80; y++) {
      for (int x = 0; x < 80; x++) {
        int idx = (y * 80 + x) * 2;
        if (idx + 1 < THUMBNAIL_SIZE) {
          uint16_t color = thumbnailBuffer[idx] | (thumbnailBuffer[idx + 1] << 8);
          tft.drawPixel(THUMB_X + x, THUMB_Y + y, color);
        }
      }
    }
  } else {
    tft.fillRect(THUMB_X, THUMB_Y, 80, 80, TFT_DARKGREY);
    tft.drawRect(THUMB_X, THUMB_Y, 80, 80, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(1);
    tft.setCursor(THUMB_X + 20, THUMB_Y + 35);
    tft.println("NO");
    tft.setCursor(THUMB_X + 15, THUMB_Y + 50);
    tft.println("ART");
  }
  
  // Initialize scrolling for title and artist
  initScrollingText();
  
  // Draw title (first frame)
  drawScrollingTitle();
  
  // Draw artist (first frame)
  drawScrollingArtist();
  
  // Draw progress bar background
  tft.fillRect(BAR_X, BAR_Y, BAR_WIDTH, BAR_HEIGHT, TFT_DARKGREY);
  
  // Draw source
  if (currentSource.length() > 0) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextSize(1);
    int sourceX = SCREEN_WIDTH - 10 - (currentSource.length() * 6);
    if (sourceX < 120) sourceX = 120;
    tft.setCursor(sourceX, TIME_Y);
    tft.println(currentSource);
  }
  
  // Draw progress bar and time FIRST
  updateProgressAndTime();
  
  // Draw play/pause state LAST so it appears on top
  updatePlayPauseState();
  
  hasValidSong = true;
  songEnded = false;
}

void initScrollingText() {
  // Prepare scrolling title with padding
  scrollingTitle = currentTitle;
  if (scrollingTitle.length() > MAX_TITLE_CHARS) {
    // Add spaces for smooth scrolling
    scrollingTitle = currentTitle + "   " + currentTitle + "   ";
  }
  
  // Prepare scrolling artist with padding
  scrollingArtist = currentArtist;
  if (scrollingArtist.length() > MAX_ARTIST_CHARS) {
    // Add spaces for smooth scrolling
    scrollingArtist = currentArtist + "   " + currentArtist + "   ";
  }
  
  titleScrollPos = 0;
  artistScrollPos = 0;
  lastScrollUpdate = millis();
}

void drawScrollingTitle() {
  // Clear title area
  tft.fillRect(TITLE_X, TITLE_Y - 20, SCREEN_WIDTH - 20, 30, TFT_BLACK);
  
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  
  if (currentTitle.length() <= MAX_TITLE_CHARS) {
    // No scrolling needed, just center it
    int xPos = TITLE_X;
    tft.setCursor(xPos, TITLE_Y);
    tft.println(currentTitle);
  } else {
    // Draw scrolling text
    tft.setCursor(TITLE_X, TITLE_Y);
    String displayText = scrollingTitle.substring(titleScrollPos, titleScrollPos + MAX_TITLE_CHARS);
    tft.println(displayText);
  }
}

void drawScrollingArtist() {
  // Clear artist area
  tft.fillRect(ARTIST_X, ARTIST_Y - 10, SCREEN_WIDTH - 20, 20, TFT_BLACK);
  
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  
  if (currentArtist.length() <= MAX_ARTIST_CHARS) {
    // No scrolling needed
    tft.setCursor(ARTIST_X, ARTIST_Y);
    tft.println(currentArtist);
  } else {
    // Draw scrolling text
    tft.setCursor(ARTIST_X, ARTIST_Y);
    String displayText = scrollingArtist.substring(artistScrollPos, artistScrollPos + MAX_ARTIST_CHARS);
    tft.println(displayText);
  }
}

void updateScrolling() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastScrollUpdate >= SCROLL_DELAY_MS) {
    bool needsUpdate = false;
    
    // Update title scroll position if needed
    if (currentTitle.length() > MAX_TITLE_CHARS) {
      titleScrollPos++;
      if (titleScrollPos > (currentTitle.length() + 3)) {
        titleScrollPos = 0;
      }
      needsUpdate = true;
    }
    
    // Update artist scroll position if needed
    if (currentArtist.length() > MAX_ARTIST_CHARS) {
      artistScrollPos++;
      if (artistScrollPos > (currentArtist.length() + 3)) {
        artistScrollPos = 0;
      }
      needsUpdate = true;
    }
    
    if (needsUpdate) {
      drawScrollingTitle();
      drawScrollingArtist();
    }
    
    lastScrollUpdate = currentTime;
  }
}

void loop() {
  // Check for timeout
  if (millis() - lastDataReceived > DATA_TIMEOUT_MS) {
    if (hasValidSong) {
      Serial.println("Data timeout - showing waiting screen");
      showWaitingScreen();
      resetToWaitingState();
    }
  }
  
  if (Serial.available()) {
    processIncomingData();
    lastDataReceived = millis();
  }
  
  // Update scrolling text
  if (hasValidSong) {
    updateScrolling();
  }
  
  // Update position based on actual elapsed time (only if song hasn't ended)
  if (hasValidSong && currentDuration > 0 && !songEnded) {
    updatePositionFromTime();
  }
  
  delay(10);
}
void updatePositionFromTime() {
  if (!isPlaying) {
    // Not playing, just update display if needed
    if (millis() - lastPositionUpdate > POSITION_UPDATE_INTERVAL_MS) {
      updateProgressAndTime();     // Update progress bar first
      updatePlayPauseState();      // Then update PAUSED text on top
      lastPositionUpdate = millis();
    }
    return;
  }
  
  // Calculate elapsed time since song started/resumed
  unsigned long currentMillis = millis();
  unsigned long elapsedMillis = currentMillis - songStartTime;
  int elapsedSeconds = elapsedMillis / 1000;
  
  // Calculate current position (initial position + elapsed seconds)
  int calculatedPosition = initialPosition + elapsedSeconds;
  
  // Clamp to duration
  if (calculatedPosition >= currentDuration) {
    calculatedPosition = currentDuration;
    
    // Song has ended - stop counting time but don't change play state
    if (!songEnded) {
      songEnded = true;
      Serial.println("Song reached end - stopping time tracking");
      updateProgressAndTime();     // Final update to show full progress bar
      updatePlayPauseState();      // Update to show ended state (no PAUSED text)
    }
  }
  
  // Update position if changed and song hasn't ended
  if (calculatedPosition != currentPosition && !songEnded) {
    currentPosition = calculatedPosition;
    lastPositionUpdate = currentMillis;
    updateProgressAndTime();       // Update progress bar first
    updatePlayPauseState();        // Then update PAUSED text on top
  } else if (millis() - lastPositionUpdate > POSITION_UPDATE_INTERVAL_MS && !songEnded) {
    // Update display periodically even if position hasn't changed (for smoother progress bar)
    updateProgressAndTime();       // Update progress bar first
    updatePlayPauseState();        // Then update PAUSED text on top
    lastPositionUpdate = currentMillis;
  }
}

void handlePlayStateChange(bool newPlayingState) {
  if (newPlayingState == isPlaying) return;
  
  // Don't allow play/pause changes if song has ended
  if (songEnded) {
    Serial.println("Song has ended - ignoring play/pause change");
    return;
  }
  
  isPlaying = newPlayingState;
  
  if (isPlaying) {
    // Resuming playback
    if (lastPauseTime > 0) {
      // Adjust start time to account for pause duration
      unsigned long pauseDuration = millis() - lastPauseTime;
      songStartTime += pauseDuration;
      lastPauseTime = 0;
    }
    Serial.println("Playback resumed");
  } else {
    // Pausing playback
    lastPauseTime = millis();
    Serial.println("Playback paused");
  }
  
  updatePlayPauseState();
}

void resetToWaitingState() {
  currentState = WAITING_FOR_START;
  metadataBuffer = "";
  thumbnailBytesRead = 0;
  hasThumbnail = false;
  isPlaying = false;
  currentDuration = 0;
  currentPosition = 0;
  initialPosition = 0;
  currentTitle = "";
  currentArtist = "";
  currentSource = "";
  hasValidSong = false;
  currentSongKey = "";
  lastSongKey = "";
  songStartTime = 0;
  pausedTime = 0;
  lastPauseTime = 0;
  titleScrollPos = 0;
  artistScrollPos = 0;
  songEnded = false;
}

void processIncomingData() {
  while (Serial.available()) {
    char c = Serial.read();
    
    switch (currentState) {
      case WAITING_FOR_START:
        if (c == 'S') {
          currentState = READING_METADATA;
          metadataBuffer = "";
          thumbnailBytesRead = 0;
          Serial.println("Start marker received");
        }
        break;
        
      case READING_METADATA:
        if (c == '\n') {
          parseMetadata(metadataBuffer);
          currentState = READING_THUMBNAIL;
          Serial.println("Metadata received, waiting for thumbnail...");
        } else {
          metadataBuffer += c;
        }
        break;
        
      case READING_THUMBNAIL:
        if (thumbnailBytesRead < THUMBNAIL_SIZE) {
          thumbnailBuffer[thumbnailBytesRead++] = c;
          
          if (thumbnailBytesRead == THUMBNAIL_SIZE) {
            hasThumbnail = true;
            currentState = WAITING_FOR_START;
            lastDataReceived = millis();
            
            Serial.println("Thumbnail received complete!");
            
            // Check if this is a new song (different title + artist)
            String newSongKey = currentTitle + "|" + currentArtist;
            
            if (newSongKey != lastSongKey) {
              // New song - render everything
              Serial.println("New song detected - full refresh");
              lastSongKey = newSongKey;
              
              // Initialize timing for new song
              songStartTime = millis();
              initialPosition = currentPosition;
              lastPauseTime = 0;
              songEnded = false;
              
              renderFullScreen();
            } else {
              // Same song - just update timing and progress
              Serial.println("Same song - updating timing");
              
              // Reset song ended flag if we're getting new data for the same song
              if (songEnded) {
                songEnded = false;
                Serial.println("Song reset - new data received");
              }
              
              // Update timing for existing song
              if (isPlaying) {
                // If we were playing, adjust timing
                songStartTime = millis();
                initialPosition = currentPosition;
              }
              
              // Check if title or artist changed (for scrolling)
              if (currentTitle != lastTitle || currentArtist != lastArtist) {
                initScrollingText();
                drawScrollingTitle();
                drawScrollingArtist();
                lastTitle = currentTitle;
                lastArtist = currentArtist;
              }
              
              updateProgressAndTime();
              updatePlayPauseState();
            }
            
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
  int fourthPipe = metadata.indexOf('|', thirdPipe + 1);
  int fifthPipe = metadata.indexOf('|', fourthPipe + 1);
  
  if (firstPipe > 0 && secondPipe > 0 && thirdPipe > 0 && fourthPipe > 0 && fifthPipe > 0) {
    String newTitle = metadata.substring(0, firstPipe);
    String newArtist = metadata.substring(firstPipe + 1, secondPipe);
    int newDuration = metadata.substring(secondPipe + 1, thirdPipe).toInt();
    int newPosition = metadata.substring(thirdPipe + 1, fourthPipe).toInt();
    String newSource = metadata.substring(fourthPipe + 1, fifthPipe);
    bool newPlayingState = (metadata.substring(fifthPipe + 1) == "True" || metadata.substring(fifthPipe + 1) == "true");
    
    // Check if this is a new song
    String newSongKey = newTitle + "|" + newArtist;
    bool isNewSong = (newSongKey != lastSongKey);
    
    // Update values
    currentTitle = newTitle;
    currentArtist = newArtist;
    currentDuration = newDuration;
    currentSource = newSource;
         // For new song, use the position from metadata
      currentPosition = newPosition;
      initialPosition = newPosition;
      
    // Clean up source string
    currentSource.replace("Microsoft.", "");
    currentSource.replace("AppleInc.", "Apple ");
    if (currentSource.length() > 15) {
      currentSource = currentSource.substring(0, 12) + "...";
    }
    
    if (isNewSong) {
 
      songEnded = false;
      handlePlayStateChange(newPlayingState);
    } else {
      // For same song, just update play state and adjust timing
      if (newPlayingState != isPlaying && !songEnded) {
        handlePlayStateChange(newPlayingState);
      }
      // Don't update position from PC for same song - let millis() handle it
    }
    
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
    Serial.print("  IsPlaying: ");
    Serial.println(isPlaying ? "Yes" : "No");
    Serial.print("  IsNewSong: ");
    Serial.println(isNewSong ? "Yes" : "No");
    Serial.print("  SongEnded: ");
    Serial.println(songEnded ? "Yes" : "No");
  } else {
    Serial.println("Failed to parse metadata!");
  }
}

void updateProgressAndTime() {
  if (!hasValidSong) return;
  
  if (currentDuration > 0) {
    int progressWidth = (currentPosition * BAR_WIDTH) / currentDuration;
    
    // Update progress bar (always update the bar)
    tft.fillRect(BAR_X, BAR_Y, BAR_WIDTH, BAR_HEIGHT, TFT_DARKGREY);
    if (progressWidth > 0) {
      tft.fillRect(BAR_X, BAR_Y, progressWidth, BAR_HEIGHT, TFT_GREEN);
    }
    
    // Update time text
    tft.fillRect(TIME_X, TIME_Y - 8, 100, 16, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(TIME_X, TIME_Y);
    tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
  }
}


void updatePlayPauseState() {
  if (!hasValidSong) return;
  
  // Clear the area where PAUSED text appears (but don't clear progress bar)
  tft.fillRect(BAR_X, BAR_Y - 12, BAR_WIDTH, 12, TFT_BLACK);
  
  // Only show PAUSED if:
  // 1. Song hasn't ended
  // 2. Not playing
  // 3. Position hasn't reached the end
  if (!songEnded && !isPlaying && currentPosition < currentDuration) {
    // Show PAUSED text - draw it AFTER progress bar so it's on top
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(BAR_X + (BAR_WIDTH / 2) - 30, BAR_Y - 2);
    tft.println("PAUSED");
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
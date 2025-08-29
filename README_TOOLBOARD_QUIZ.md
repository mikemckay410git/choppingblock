# ToolBoard Quiz System

This system combines the existing toolboard impact detection with a quiz interface, allowing you to navigate through quiz questions by hitting different areas of the toolboard.

## Features

- **Dual Mode Interface**: Switch between Quiz Mode and Game Mode
- **Toolboard Controls**: Navigate quiz using physical impacts on the toolboard
- **Web Interface**: Modern, responsive web UI accessible via WiFi
- **Sample Questions**: Built-in sample questions plus support for CSV file upload
- **Keyboard Shortcuts**: Arrow keys and spacebar for navigation

## Setup Instructions

### 1. Hardware Setup
- Connect your ESP32 toolboard to your computer
- Ensure all sensors are properly connected (GPIO 35, 33, 34, 32)
- Upload the modified `player1_host_sync.ino` to your toolboard

### 2. WiFi Connection
- The toolboard creates a WiFi access point named "ToolBoard" with password "12345678"
- Connect your device (phone, tablet, computer) to this WiFi network
- Navigate to `192.168.4.1` in your web browser

### 3. Using the Quiz System

#### Quiz Mode Controls
The toolboard is divided into 4 quadrants for quiz navigation:

- **Top-Left (0-0.2m, 0-0.2m)**: Previous question
- **Top-Right (0.2-0.4m, 0-0.2m)**: Next question  
- **Bottom-Left (0-0.2m, 0.2-0.4m)**: Toggle answer show/hide
- **Bottom-Right (0.2-0.4m, 0.2-0.4m)**: Toggle answer show/hide

#### Web Interface Controls
- **Mode Toggle**: Switch between Quiz and Game modes
- **Navigation Buttons**: Previous, Show/Hide Answer, Next
- **Keyboard Shortcuts**: 
  - `←` / `→`: Previous/Next question
  - `Space`: Toggle answer visibility
  - Click on question card to toggle answer

### 4. Adding Custom Questions

#### Option 1: CSV File Upload
1. Prepare a CSV file with columns: `Question,Answer,Category`
2. Use the file upload feature in the web interface
3. Select your CSV file(s) to load custom questions

#### Option 2: Modify the Code
Edit the `sampleQuestions` array in the HTML to add your own questions:

```javascript
const sampleQuestions = [
  { q: "Your question here?", a: "Your answer here", category: "Your category" },
  // Add more questions...
];
```

### 5. Game Mode
- Switch to Game Mode for the original two-player impact game
- Requires a second toolboard running `player2_client_sync.ino`
- First player to hit their board wins the round

## Technical Details

### Sensor Layout
```
Top (GPIO35)     Right (GPIO34)
     |                 |
     |                 |
Left (GPIO32)   Bottom (GPIO33)
```

### Board Coordinates
- Board size: 0.4m x 0.4m
- Origin: Top-left corner (0,0)
- Sensor positions are calibrated for accurate hit detection

### WebSocket Communication
- Quiz actions are sent via WebSocket from toolboard to web interface
- Real-time updates for quiz navigation and game state
- Single client connection supported for resource efficiency

## Troubleshooting

### Common Issues

1. **Web interface not loading**
   - Ensure you're connected to the "ToolBoard" WiFi network
   - Try accessing `192.168.4.1` in your browser
   - Check that the toolboard is powered and running

2. **Hit detection not working**
   - Verify sensor connections (GPIO 35, 33, 34, 32)
   - Check serial monitor for sensor readings
   - Ensure sensors are properly mounted to the board

3. **Quiz navigation not responding**
   - Check that you're in Quiz Mode (not Game Mode)
   - Verify hit locations are within the correct quadrants
   - Check serial monitor for quiz action messages

### Serial Monitor Output
The toolboard provides detailed serial output including:
- Sensor readings and hit detection
- Quiz action triggers
- WebSocket connection status
- Game state changes

## File Structure

```
├── player1_host_sync.ino    # Modified toolboard code with quiz support
├── player2_client_sync.ino  # Original client code for game mode
├── toolboard_quiz.csv       # Sample quiz questions
├── quiz.html               # Original standalone quiz interface
└── README_TOOLBOARD_QUIZ.md # This file
```

## Future Enhancements

- Support for multiple quiz categories
- Score tracking and statistics
- Sound effects for hits and navigation
- Multiplayer quiz mode
- Integration with external quiz APIs
- Customizable hit zones and controls

## Credits

This system builds upon the original toolboard impact detection system and combines it with a modern web-based quiz interface for an interactive learning experience.

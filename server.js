import express from "express";
import http from "http";
import { Server } from "socket.io";
import cors from "cors";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";
import fs from "fs";
import path from "path";
import multer from "multer";
import { lightboardState } from "./lightboard.js";

const app = express();
const server = http.createServer(app);
const io = new Server(server, {
  cors: {
    origin: "*",
    methods: ["GET", "POST"]
  }
});

// Helper function to sanitize filenames - removes problematic characters
function sanitizeFilename(filename) {
  if (!filename) return 'file';
  
  // Get the extension first
  const lastDot = filename.lastIndexOf('.');
  let name = filename;
  let ext = '';
  
  if (lastDot > 0 && lastDot < filename.length - 1) {
    name = filename.substring(0, lastDot);
    ext = filename.substring(lastDot);
  }
  
  // Remove or replace problematic characters
  // Windows/Linux forbidden: / \ : * ? " < > |
  // Also remove other problematic characters: multiple dots, control characters
  let sanitized = name
    .replace(/[/\\:*?"<>|]/g, '_')  // Replace forbidden characters with underscore
    .replace(/[\x00-\x1F\x7F]/g, '') // Remove control characters
    .replace(/[^\x20-\x7E\u00A0-\uFFFF]/g, '_') // Replace non-printable unicode with underscore
    .replace(/\.{2,}/g, '.') // Replace multiple dots with single dot
    .replace(/^[\s.]+|[\s.]+$/g, ''); // Remove leading/trailing spaces and dots (but keep spaces in the middle)
  
  // Ensure we have something left
  if (!sanitized || sanitized.length === 0) {
    sanitized = 'file';
  }
  
  // Limit total length (including extension) to 255 characters (common filesystem limit)
  const maxLength = 255;
  const extLength = ext.length;
  const maxNameLength = maxLength - extLength;
  
  if (sanitized.length > maxNameLength) {
    sanitized = sanitized.substring(0, maxNameLength);
  }
  
  // Validate extension - only allow alphanumeric and common safe characters
  const safeExt = ext.replace(/[^a-zA-Z0-9._-]/g, '').toLowerCase();
  
  return sanitized + safeExt;
}

// Helper function to validate file extension
function isValidFileExtension(filename, allowedExtensions) {
  const ext = filename.toLowerCase().substring(filename.lastIndexOf('.'));
  return allowedExtensions.includes(ext);
}

// Helper function to set file permissions (readable/writable by owner, readable/writable by group and others)
// Using more permissive permissions to allow SFTP deletion
function setFilePermissions(filePath, isDirectory = false) {
  try {
    if (fs.existsSync(filePath)) {
      // 0666 for files (rw-rw-rw-), 0777 for directories (rwxrwxrwx)
      // This allows any user to read, write, and delete files
      const mode = isDirectory ? 0o777 : 0o666;
      fs.chmodSync(filePath, mode);
      console.log(`Set permissions ${mode.toString(8)} on ${filePath}`);
    }
  } catch (error) {
    console.warn(`Warning: Could not set permissions for ${filePath}:`, error.message);
  }
}

// Helper function to recursively set permissions on a directory and all its contents
function setDirectoryPermissionsRecursive(dirPath) {
  try {
    if (!fs.existsSync(dirPath)) {
      return;
    }
    
    // Set permissions on the directory itself first
    setFilePermissions(dirPath, true);
    
    // Read all items in the directory
    const items = fs.readdirSync(dirPath, { withFileTypes: true });
    
    for (const item of items) {
      const itemPath = path.join(dirPath, item.name);
      
      if (item.isDirectory()) {
        // Recursively set permissions on subdirectories
        setDirectoryPermissionsRecursive(itemPath);
      } else {
        // Set permissions on files
        setFilePermissions(itemPath, false);
      }
    }
    
    // Set permissions on directory again after processing contents
    // This ensures the directory itself has correct permissions
    setFilePermissions(dirPath, true);
    console.log(`Completed recursive permission setting for ${dirPath}`);
  } catch (error) {
    console.warn(`Warning: Could not set recursive permissions for ${dirPath}:`, error.message);
  }
}

app.use(cors());
app.use(express.json()); // Parse JSON request bodies

// Configure multer for file uploads
const storage = multer.diskStorage({
  destination: function (req, file, cb) {
    // Determine destination based on file type
    if (file.fieldname.startsWith('audio_')) {
      const quizName = req.body.quizName || 'default';
      const quizDir = path.join(process.cwd(), 'Quizes', quizName);
      if (!fs.existsSync(quizDir)) {
        fs.mkdirSync(quizDir, { recursive: true, mode: 0o777 });
        setFilePermissions(quizDir, true);
      } else {
        setFilePermissions(quizDir, true);
      }
      cb(null, quizDir);
    } else if (file.fieldname.startsWith('image_')) {
      // Images can go to Images folder or quiz folder
      const imagesDir = path.join(process.cwd(), 'Images');
      if (!fs.existsSync(imagesDir)) {
        fs.mkdirSync(imagesDir, { recursive: true, mode: 0o777 });
        setFilePermissions(imagesDir, true);
      } else {
        setFilePermissions(imagesDir, true);
      }
      cb(null, imagesDir);
    } else {
      // CSV files go to Quizes folder
      const quizesDir = path.join(process.cwd(), 'Quizes');
      if (!fs.existsSync(quizesDir)) {
        fs.mkdirSync(quizesDir, { recursive: true, mode: 0o777 });
        setFilePermissions(quizesDir, true);
      } else {
        setFilePermissions(quizesDir, true);
      }
      cb(null, quizesDir);
    }
  },
  filename: function (req, file, cb) {
    // Use original filename or the name provided in the request
    if (file.fieldname === 'csvContent') {
      // Sanitize CSV filename
      const sanitized = sanitizeFilename(file.originalname);
      cb(null, sanitized);
    } else {
      // For audio and image files, use the name from the request
      const index = file.fieldname.split('_')[1];
      const nameField = file.fieldname.startsWith('audio_') 
        ? `audio_name_${index}` 
        : `image_name_${index}`;
      let fileName = req.body[nameField] || file.originalname;
      
      // Validate and sanitize filename
      if (file.fieldname.startsWith('audio_')) {
        // Validate audio file extension
        const allowedAudioExts = ['.mp3', '.wav', '.ogg', '.m4a', '.aac', '.flac'];
        if (!isValidFileExtension(fileName, allowedAudioExts)) {
          // If invalid extension, try to preserve original extension or default to .mp3
          const originalExt = file.originalname.substring(file.originalname.lastIndexOf('.'));
          if (isValidFileExtension(originalExt, allowedAudioExts)) {
            fileName = sanitizeFilename(fileName.replace(/\.[^.]+$/, '') + originalExt);
          } else {
            fileName = sanitizeFilename(fileName.replace(/\.[^.]+$/, '')) + '.mp3';
          }
        } else {
          fileName = sanitizeFilename(fileName);
        }
      } else if (file.fieldname.startsWith('image_')) {
        // Validate image file extension
        const allowedImageExts = ['.jpg', '.jpeg', '.png', '.gif', '.webp', '.svg', '.bmp'];
        if (!isValidFileExtension(fileName, allowedImageExts)) {
          // If invalid extension, try to preserve original extension or default to .jpg
          const originalExt = file.originalname.substring(file.originalname.lastIndexOf('.'));
          if (isValidFileExtension(originalExt, allowedImageExts)) {
            fileName = sanitizeFilename(fileName.replace(/\.[^.]+$/, '') + originalExt);
          } else {
            fileName = sanitizeFilename(fileName.replace(/\.[^.]+$/, '')) + '.jpg';
          }
        } else {
          fileName = sanitizeFilename(fileName);
        }
      } else {
        fileName = sanitizeFilename(fileName);
      }
      
      cb(null, fileName);
    }
  }
});

const upload = multer({ 
  storage: storage,
  limits: { fileSize: 50 * 1024 * 1024 } // 50MB limit
});

// IMPORTANT: API routes must be defined BEFORE express.static
// to prevent static file serving from interfering with API endpoints

// Debug endpoint to test if save-quiz route exists
app.get("/api/save-quiz-test", (req, res) => {
  console.log('GET /api/save-quiz-test called');
  res.json({ 
    message: 'Save quiz route is registered',
    timestamp: new Date().toISOString(),
    multerInstalled: typeof upload !== 'undefined',
    uploadType: typeof upload
  });
});

// Save quiz endpoint
app.post("/api/save-quiz", upload.any(), (req, res) => {
  console.log('=== SAVE QUIZ REQUEST RECEIVED ===');
  console.log('Method:', req.method);
  console.log('URL:', req.url);
  console.log('Body keys:', Object.keys(req.body));
  console.log('Files:', req.files ? req.files.map(f => ({ fieldname: f.fieldname, filename: f.filename })) : 'No files');
  
  // Set permissions on all uploaded files immediately after multer processes them
  if (req.files) {
    req.files.forEach(file => {
      if (fs.existsSync(file.path)) {
        setFilePermissions(file.path, false);
      }
    });
  }
  
  try {
    const quizName = req.body.quizName;
    const mode = req.body.mode;
    const existingPath = req.body.existingPath;
    
    console.log('Quiz name:', quizName);
    console.log('Mode:', mode);
    console.log('Existing path:', existingPath);
    
    if (!quizName) {
      console.log('ERROR: Quiz name is required');
      return res.status(400).json({ error: 'Quiz name is required' });
    }
    
    // Sanitize quiz name to prevent problematic folder names
    const sanitizedQuizName = sanitizeFilename(quizName).replace(/\.[^.]+$/, ''); // Remove extension if present
    if (sanitizedQuizName !== quizName) {
      console.log(`WARNING: Quiz name sanitized from "${quizName}" to "${sanitizedQuizName}"`);
    }
    
    // Find the CSV file in the uploaded files
    const csvFile = req.files ? req.files.find(f => f.fieldname === 'csvContent') : null;
    if (!csvFile) {
      console.log('ERROR: CSV file is required. Files received:', req.files);
      return res.status(400).json({ error: 'CSV file is required' });
    }
    
    // Check if there are audio files - if so, quiz needs to be in a folder structure
    const audioFiles = req.files ? req.files.filter(f => f.fieldname.startsWith('audio_')) : [];
    const hasAudioFiles = audioFiles.length > 0;
    
    const quizesDir = path.join(process.cwd(), 'Quizes');
    if (!fs.existsSync(quizesDir)) {
      fs.mkdirSync(quizesDir, { recursive: true, mode: 0o777 });
      setFilePermissions(quizesDir, true);
    } else {
      setFilePermissions(quizesDir, true);
    }
    
    // Determine the final path based on the NEW quiz name (not the old path)
    let finalPath;
    let quizDir;
    let oldPath = null;
    let oldDir = null;
    
    // Get old path info if editing
    if (mode === 'edit' && existingPath) {
      oldPath = path.join(process.cwd(), existingPath);
      oldDir = path.dirname(oldPath);
    }
    
    if (hasAudioFiles) {
      // Quiz has audio files - must be in folder structure: Quizes/${sanitizedQuizName}/${sanitizedQuizName}.csv
      quizDir = path.join(quizesDir, sanitizedQuizName);
      if (!fs.existsSync(quizDir)) {
        fs.mkdirSync(quizDir, { recursive: true, mode: 0o777 });
        setFilePermissions(quizDir, true);
      } else {
        setFilePermissions(quizDir, true);
      }
      finalPath = path.join(quizDir, `${sanitizedQuizName}.csv`);
    } else {
      // No audio files - flat file: Quizes/${sanitizedQuizName}.csv
      finalPath = path.join(quizesDir, `${sanitizedQuizName}.csv`);
      quizDir = quizesDir;
    }
    
    // Ensure directory exists
    if (!fs.existsSync(quizDir)) {
      fs.mkdirSync(quizDir, { recursive: true, mode: 0o777 });
      setFilePermissions(quizDir, true);
    } else {
      // Directory exists, ensure it has correct permissions
      setFilePermissions(quizDir, true);
    }
    
    // Ensure parent directory of finalPath has correct permissions
    const finalDir = path.dirname(finalPath);
    if (fs.existsSync(finalDir)) {
      setFilePermissions(finalDir, true);
    }
    
    // Also ensure Quizes directory has correct permissions
    if (fs.existsSync(quizesDir)) {
      setFilePermissions(quizesDir, true);
    }
    
    // Move the CSV file to the final location
    fs.renameSync(csvFile.path, finalPath);
    setFilePermissions(finalPath, false);
    console.log(`CSV saved to: ${finalPath}`);
    
    // Handle audio files - they should be in the quiz folder
    if (hasAudioFiles) {
      audioFiles.forEach(audioFile => {
        // Audio files are already saved in the quiz folder by multer (quizDir)
        // Just ensure they're in the right place
        const audioPath = path.join(quizDir, audioFile.filename);
        if (audioFile.path !== audioPath) {
          // Move to correct location if needed
          if (fs.existsSync(audioFile.path)) {
            fs.renameSync(audioFile.path, audioPath);
            setFilePermissions(audioPath, false);
            console.log(`Audio file moved to: ${audioPath}`);
          }
        } else {
          // File already in correct location, just set permissions
          setFilePermissions(audioPath, false);
          console.log(`Audio file already in correct location: ${audioPath}`);
        }
      });
    }
    
    // Handle image files - they should be in the Images folder
    const imageFiles = req.files.filter(f => f.fieldname.startsWith('image_'));
    imageFiles.forEach(imageFile => {
      // Image files are already saved in the Images folder by multer
      console.log(`Image file saved: ${imageFile.filename} at ${imageFile.path}`);
      // Verify the file exists and set permissions
      if (fs.existsSync(imageFile.path)) {
        setFilePermissions(imageFile.path, false);
        console.log(`✓ Image file confirmed at: ${imageFile.path}`);
      } else {
        console.error(`✗ Image file NOT FOUND at: ${imageFile.path}`);
      }
    });
    
    // If editing and the name/location changed, clean up the old file/folder
    if (mode === 'edit' && oldPath && oldPath !== finalPath && fs.existsSync(oldPath)) {
      console.log(`Quiz name/location changed. Cleaning up old location: ${oldPath}`);
      
      try {
        // Check if old path is a file or directory
        const oldStats = fs.statSync(oldPath);
        
        if (oldStats.isDirectory()) {
          // Old location is a directory (music quiz folder)
          // Check if we need to move audio files from old folder to new folder
          if (hasAudioFiles && oldDir && fs.existsSync(oldDir)) {
            const oldAudioFiles = fs.readdirSync(oldDir).filter(file => 
              file.toLowerCase().endsWith('.mp3') || 
              file.toLowerCase().endsWith('.wav') || 
              file.toLowerCase().endsWith('.ogg') ||
              file.toLowerCase().endsWith('.m4a') ||
              file.toLowerCase().endsWith('.aac') ||
              file.toLowerCase().endsWith('.flac')
            );
            
            // Move audio files to new location if they're not already there
            oldAudioFiles.forEach(audioFileName => {
              const oldAudioPath = path.join(oldDir, audioFileName);
              const newAudioPath = path.join(quizDir, audioFileName);
              if (fs.existsSync(oldAudioPath) && !fs.existsSync(newAudioPath)) {
                fs.renameSync(oldAudioPath, newAudioPath);
                setFilePermissions(newAudioPath, false);
                console.log(`Moved audio file: ${oldAudioPath} -> ${newAudioPath}`);
              }
            });
          }
          
          // Remove old directory if it's empty or only contains the CSV
          const oldDirContents = fs.readdirSync(oldDir);
          // Only delete if directory is empty or only contains the CSV file
          if (oldDirContents.length === 0 || (oldDirContents.length === 1 && oldDirContents[0].endsWith('.csv'))) {
            if (oldDirContents.length === 1) {
              // Remove the CSV file first
              const oldCsvPath = path.join(oldDir, oldDirContents[0]);
              fs.unlinkSync(oldCsvPath);
            }
            fs.rmdirSync(oldDir);
            console.log(`Removed old quiz directory: ${oldDir}`);
          } else {
            // Directory has other files, just remove the CSV
            const oldCsvInDir = path.join(oldDir, path.basename(oldPath));
            if (fs.existsSync(oldCsvInDir)) {
              fs.unlinkSync(oldCsvInDir);
              console.log(`Removed old CSV file: ${oldCsvInDir}`);
            }
          }
        } else {
          // Old location is a file (flat CSV)
          fs.unlinkSync(oldPath);
          console.log(`Removed old CSV file: ${oldPath}`);
        }
      } catch (error) {
        console.warn(`Could not remove old file/folder ${oldPath}:`, error.message);
      }
    }
    
    // For music quizzes, ensure the entire folder structure has correct permissions
    // This is critical for allowing deletion of the folder and its contents
    if (hasAudioFiles && quizDir && quizDir !== quizesDir) {
      console.log(`Setting recursive permissions on quiz folder: ${quizDir}`);
      setDirectoryPermissionsRecursive(quizDir);
    }
    
    // Also ensure the Quizes directory itself has correct permissions
    if (fs.existsSync(quizesDir)) {
      setFilePermissions(quizesDir, true);
    }
    
    console.log('SUCCESS: Quiz saved to', finalPath);
    res.json({ 
      success: true, 
      message: 'Quiz saved successfully',
      path: finalPath
    });
  } catch (error) {
    console.error('ERROR saving quiz:', error);
    console.error('Error stack:', error.stack);
    res.status(500).json({ error: 'Failed to save quiz: ' + error.message });
  }
});

// Quiz files listing endpoint
app.get("/api/quiz-files", (req, res) => {
  try {
    const quizesDir = path.join(process.cwd(), 'Quizes');
    
    // Check if Quizes directory exists
    if (!fs.existsSync(quizesDir)) {
      return res.json([]);
    }
    
    const quizItems = [];
    
    // Read directory contents
    const items = fs.readdirSync(quizesDir, { withFileTypes: true });
    
    for (const item of items) {
      if (item.isFile() && item.name.toLowerCase().endsWith('.csv')) {
        // Regular CSV quiz file
        quizItems.push({
          type: 'regular',
          name: item.name,
          path: `Quizes/${item.name}`
        });
      } else if (item.isDirectory()) {
        // Check if it's a music quiz folder
        const folderPath = path.join(quizesDir, item.name);
        const csvFile = path.join(folderPath, `${item.name}.csv`);
        
        if (fs.existsSync(csvFile)) {
          // Check if folder contains audio files
          const audioFiles = fs.readdirSync(folderPath)
            .filter(file => file.toLowerCase().endsWith('.mp3') || file.toLowerCase().endsWith('.wav'));
          
          if (audioFiles.length > 0) {
            quizItems.push({
              type: 'music',
              name: item.name,
              path: `Quizes/${item.name}/${item.name}.csv`,
              audioFiles: audioFiles.map(file => `Quizes/${item.name}/${file}`)
            });
          } else {
            // Folder with CSV but no audio files - treat as regular quiz
            quizItems.push({
              type: 'regular',
              name: `${item.name}.csv`,
              path: `Quizes/${item.name}/${item.name}.csv`
            });
          }
        }
      }
    }
    
    // Sort alphabetically by name
    quizItems.sort((a, b) => a.name.localeCompare(b.name));
    
    res.json(quizItems);
  } catch (error) {
    console.error('Error reading quiz files:', error);
    res.status(500).json({ error: 'Failed to read quiz files' });
  }
});

app.use(express.static(".")); // serve files from home directory (AFTER API routes)

// ESP32 Serial Communication
class ESP32Bridge {
  constructor(serialPort = '/dev/ttyUSB0', baudRate = 115200) {
    this.serialPort = serialPort;
    this.baudRate = baudRate;
    this.serialConnection = null;
    this.parser = null;
    this.io = io;
    this.enabled = false; // Track if serial is enabled
    this.lightboardConnected = false; // Track if physical lightboard is connected via ESP-NOW
    this.player1Connected = false; // Track if Player 1 is connected via ESP-NOW
    this.player2Connected = false; // Track if Player 2 is connected via ESP-NOW
    this.retryTimeout = null; // Track retry timeout for cleanup
    this.heartbeatInterval = null; // Track heartbeat interval timer
    // Removed debounce variables - ESP32 handles awarding internally
  }

  async startSerialCommunication() {
    // Clear any existing retry timeout
    if (this.retryTimeout) {
      clearTimeout(this.retryTimeout);
      this.retryTimeout = null;
    }

    // Clean up existing connection
    this.cleanup();

    try {
      // List available ports for debugging
      const ports = await SerialPort.list();
      console.log('Available serial ports:', ports.map(p => p.path));
      
      // Check if the specified port exists
      const portExists = ports.some(port => port.path === this.serialPort);
      if (!portExists) {
        console.log(`Serial port ${this.serialPort} not found. ESP32 communication disabled.`);
        console.log('Server will run without ESP32 connection.');
        this.lightboardConnected = false;
        this.player1Connected = false;
        this.player2Connected = false;
        this.io.emit('esp32_status', { 
          connected: false, 
          enabled: false,
          lightboardConnected: false,
          player1Connected: false,
          player2Connected: false
        });
        return;
      }
      
      this.enabled = true;
      this.serialConnection = new SerialPort({
        path: this.serialPort,
        baudRate: this.baudRate,
        autoOpen: false
      });

      this.parser = this.serialConnection.pipe(new ReadlineParser({ delimiter: '\n' }));
      
      this.serialConnection.on('open', () => {
        console.log(`Connected to ESP32 on ${this.serialPort}`);
        // Notify all clients that ESP32 is connected
        this.io.emit('esp32_status', { 
          connected: true, 
          enabled: true,
          lightboardConnected: this.lightboardConnected,
          player1Connected: this.player1Connected,
          player2Connected: this.player2Connected
        });
        
        // Start sending periodic heartbeats to Bridge (every 2 seconds)
        // Bridge responds with status messages that include lightboard connection info
        this.startHeartbeat();
      });

      this.serialConnection.on('error', (err) => {
        console.warn(`Serial connection error: ${err.message}`);
        this.cleanup();
        this.enabled = false;
        this.lightboardConnected = false; // Reset lightboard connection status
        this.player1Connected = false; // Reset player connections
        this.player2Connected = false;
        // Notify all clients that ESP32 is disconnected
        this.io.emit('esp32_status', { 
          connected: false, 
          enabled: false,
          lightboardConnected: false,
          player1Connected: false,
          player2Connected: false
        });
        // Retry connection after 5 seconds (longer delay for optional connection)
        this.retryTimeout = setTimeout(() => this.startSerialCommunication(), 5000);
      });

      this.parser.on('data', (data) => {
        this.handleSerialMessage(data.trim());
      });

      // Open the connection
      await new Promise((resolve, reject) => {
        this.serialConnection.open((err) => {
          if (err) reject(err);
          else resolve();
        });
      });

    } catch (error) {
      console.warn(`Failed to connect to serial port ${this.serialPort}:`, error.message);
      console.log('Server will continue running without ESP32 connection.');
      this.cleanup();
      this.enabled = false;
      this.lightboardConnected = false;
      this.player2Connected = false;
      this.player3Connected = false;
      // Notify all clients that ESP32 is disconnected
      this.io.emit('esp32_status', { 
        connected: false, 
        enabled: false,
        lightboardConnected: false,
        player2Connected: false,
        player3Connected: false
      });
      // Retry connection after 5 seconds (longer delay for optional connection)
      this.retryTimeout = setTimeout(() => this.startSerialCommunication(), 5000);
    }
  }

  handleSerialMessage(message) {
    try {
      // Parse JSON message from ESP32
      const data = JSON.parse(message);
      console.log('Received from ESP32:', data);
      
      // Check if this is a status message with connection info
      if (data.type === 'status') {
        // Update connection statuses from Bridge
        let statusChanged = false;
        
        if (data.lightboardConnected !== undefined) {
          const wasConnected = this.lightboardConnected;
          this.lightboardConnected = data.lightboardConnected;
          if (wasConnected !== this.lightboardConnected) {
            console.log(`Lightboard connection status changed: ${this.lightboardConnected ? 'connected' : 'disconnected'}`);
            statusChanged = true;
          }
        }
        
        if (data.player1Connected !== undefined) {
          const wasConnected = this.player1Connected;
          this.player1Connected = data.player1Connected;
          if (wasConnected !== this.player1Connected) {
            console.log(`Player 1 connection status changed: ${this.player1Connected ? 'connected' : 'disconnected'}`);
            statusChanged = true;
          }
        }
        
        if (data.player2Connected !== undefined) {
          const wasConnected = this.player2Connected;
          this.player2Connected = data.player2Connected;
          if (wasConnected !== this.player2Connected) {
            console.log(`Player 2 connection status changed: ${this.player2Connected ? 'connected' : 'disconnected'}`);
            statusChanged = true;
          }
        }
        
        // Always emit status update when we receive status from Bridge
        // This ensures initial state is set and clients are kept in sync
        this.io.emit('esp32_status', { 
          connected: this.serialConnection ? this.serialConnection.isOpen : false,
          enabled: this.enabled,
          lightboardConnected: this.lightboardConnected,
          player1Connected: this.player1Connected,
          player2Connected: this.player2Connected
        });
      }
      
      // Check if this is a request for lightboard state
      if (data.type === 'lightboardStateRequest') {
        // Get current state from state manager
        const gameState = lightboardState.getGameState();
        
        // Send state back to ESP32
        this.sendToESP32({
          cmd: 'lightboardState',
          gameState: gameState
        });
        console.log('Sent lightboard state to ESP32:', gameState);
        return; // Don't forward this request to clients
      }
      
      // Update state when receiving point awards from ESP32
      if (data.type === 'winner' && data.winner) {
        // Extract player number from winner string (e.g., "Player 1" -> 1)
        const playerMatch = data.winner.match(/Player (\d+)/);
        if (playerMatch) {
          const player = parseInt(playerMatch[1]);
          if (player === 1 || player === 2) {
            lightboardState.awardPoint(player, 1);
            console.log(`Updated lightboard state: Player ${player} scored`);
          }
        }
      }
      
      // Update state when receiving reset from ESP32
      if (data.type === 'reset') {
        lightboardState.resetGame();
        console.log('Reset lightboard state from ESP32');
      }
      
      // Forward to all Socket.IO clients
      this.io.emit('esp32_data', data);
      
    } catch (error) {
      // Handle non-JSON messages (like debug output)
      console.log('Non-JSON message from ESP32:', message);

      // Forward important status messages to frontend
      if (message.includes('heartbeat') || 
          message.includes('Clock sync') || 
          message.includes('lightboard') ||
          message.includes('Player') ||
          message.includes('Status')) {
        
        // Send status message to frontend
        this.io.emit('esp32_status_message', {
          type: 'status',
          message: message,
          timestamp: new Date().toISOString()
        });
      }
    }
  }

  sendToESP32(command) {
    if (!this.enabled || !this.serialConnection || !this.serialConnection.isOpen) {
      console.warn('ESP32 serial connection not available - command ignored:', command);
      return;
    }
    
    try {
      const message = JSON.stringify(command) + '\n';
      this.serialConnection.write(message);
      console.log('Sent to ESP32:', command);
      // Surface to clients for debugging
      this.io.emit('esp32_status_message', {
        type: 'status',
        message: `Sent to ESP32: ${JSON.stringify(command)}`,
        timestamp: new Date().toISOString()
      });
    } catch (error) {
      console.error('Error sending to ESP32:', error);
    }
  }

  startHeartbeat() {
    // Stop any existing heartbeat
    this.stopHeartbeat();
    
    // Send initial heartbeat immediately
    this.sendHeartbeat();
    
    // Then send heartbeats every 2 seconds
    this.heartbeatInterval = setInterval(() => {
      this.sendHeartbeat();
    }, 2000);
  }
  
  stopHeartbeat() {
    if (this.heartbeatInterval) {
      clearInterval(this.heartbeatInterval);
      this.heartbeatInterval = null;
    }
  }
  
  sendHeartbeat() {
    if (!this.enabled || !this.serialConnection || !this.serialConnection.isOpen) {
      return;
    }
    
    try {
      const message = JSON.stringify({ cmd: 'heartbeat' }) + '\n';
      this.serialConnection.write(message);
    } catch (error) {
      console.error('Error sending heartbeat to Bridge:', error);
    }
  }

  cleanup() {
    // Stop heartbeat
    this.stopHeartbeat();
    
    // Clear retry timeout
    if (this.retryTimeout) {
      clearTimeout(this.retryTimeout);
      this.retryTimeout = null;
    }

    // Remove parser event listeners
    if (this.parser) {
      this.parser.removeAllListeners();
      this.parser = null;
    }

    // Close and cleanup serial connection
    if (this.serialConnection) {
      if (this.serialConnection.isOpen) {
        this.serialConnection.close();
      }
      this.serialConnection.removeAllListeners();
      this.serialConnection = null;
    }
  }

  stop() {
    this.cleanup();
    console.log('ESP32 Bridge stopped');
  }
}

// Create ESP32 bridge instance
const esp32Bridge = new ESP32Bridge('/dev/ttyUSB0', 115200);

// Version check endpoint - helps verify which code is running
app.get("/api/version", (req, res) => {
  res.json({ 
    version: "2.0.0-with-quiz-editor",
    hasSaveQuizRoute: true,
    hasDebugging: true,
    multerInstalled: typeof upload !== 'undefined',
    timestamp: new Date().toISOString(),
    routes: {
      saveQuiz: "/api/save-quiz",
      saveQuizTest: "/api/save-quiz-test",
      quizFiles: "/api/quiz-files",
      version: "/api/version"
    }
  });
});

// Example endpoint
app.get("/api", (req, res) => {
  res.json({ 
    message: "Quizboard backend running",
    version: "2.0.0-with-quiz-editor",
    routes: {
      saveQuiz: "/api/save-quiz",
      saveQuizTest: "/api/save-quiz-test",
      quizFiles: "/api/quiz-files",
      version: "/api/version"
    },
    timestamp: new Date().toISOString()
  });
});

// Lightboard emulator endpoint
app.get("/lightboard", (req, res) => {
  res.sendFile(path.join(process.cwd(), 'lightboard.html'));
});

// Lightboard state endpoints
app.get("/api/lightboard-state", (req, res) => {
  try {
    const state = lightboardState.getState();
    res.json(state);
  } catch (error) {
    console.error('Error reading lightboard state:', error);
    res.status(500).json({ error: 'Failed to read lightboard state' });
  }
});

app.post("/api/lightboard-state", (req, res) => {
  try {
    lightboardState.updateState(req.body);
    res.json({ success: true });
  } catch (error) {
    console.error('Error saving lightboard state:', error);
    res.status(500).json({ error: 'Failed to save lightboard state' });
  }
});

// NOTE: The /api/save-quiz route is defined earlier in the file (around line 76)
// before express.static to ensure it's registered properly

// Quiz files listing endpoint
app.get("/api/quiz-files", (req, res) => {
  try {
    const quizesDir = path.join(process.cwd(), 'Quizes');
    
    // Check if Quizes directory exists
    if (!fs.existsSync(quizesDir)) {
      return res.json([]);
    }
    
    const quizItems = [];
    
    // Read directory contents
    const items = fs.readdirSync(quizesDir, { withFileTypes: true });
    
    for (const item of items) {
      if (item.isFile() && item.name.toLowerCase().endsWith('.csv')) {
        // Regular CSV quiz file
        quizItems.push({
          type: 'regular',
          name: item.name,
          path: `Quizes/${item.name}`
        });
      } else if (item.isDirectory()) {
        // Check if it's a music quiz folder
        const folderPath = path.join(quizesDir, item.name);
        const csvFile = path.join(folderPath, `${item.name}.csv`);
        
        if (fs.existsSync(csvFile)) {
          // Check if folder contains audio files
          const audioFiles = fs.readdirSync(folderPath)
            .filter(file => file.toLowerCase().endsWith('.mp3') || file.toLowerCase().endsWith('.wav'));
          
          if (audioFiles.length > 0) {
            quizItems.push({
              type: 'music',
              name: item.name,
              path: `Quizes/${item.name}/${item.name}.csv`,
              audioFiles: audioFiles.map(file => `Quizes/${item.name}/${file}`)
            });
          } else {
            // Folder with CSV but no audio files - treat as regular quiz
            quizItems.push({
              type: 'regular',
              name: `${item.name}.csv`,
              path: `Quizes/${item.name}/${item.name}.csv`
            });
          }
        }
      }
    }
    
    // Sort alphabetically by name
    quizItems.sort((a, b) => a.name.localeCompare(b.name));
    
    res.json(quizItems);
  } catch (error) {
    console.error('Error reading quiz files:', error);
    res.status(500).json({ error: 'Failed to read quiz files' });
  }
});

// Health check endpoint
app.get("/health", (req, res) => {
  const memUsage = process.memoryUsage();
  res.json({ 
    status: 'ok', 
    esp32Enabled: esp32Bridge.enabled,
    esp32Connected: esp32Bridge.serialConnection ? esp32Bridge.serialConnection.isOpen : false,
    socketClients: io.engine.clientsCount,
    memory: {
      rss: Math.round(memUsage.rss / 1024 / 1024) + ' MB',
      heapTotal: Math.round(memUsage.heapTotal / 1024 / 1024) + ' MB',
      heapUsed: Math.round(memUsage.heapUsed / 1024 / 1024) + ' MB',
      external: Math.round(memUsage.external / 1024 / 1024) + ' MB'
    },
    uptime: Math.round(process.uptime()) + ' seconds'
  });
});

// Socket.IO connection handling
io.on("connection", (socket) => {
  console.log("Client connected:", socket.id);
  
  // Send current ESP32 status to new client
  socket.emit('esp32_status', { 
    connected: esp32Bridge.serialConnection ? esp32Bridge.serialConnection.isOpen : false,
    enabled: esp32Bridge.enabled,
    lightboardConnected: esp32Bridge.lightboardConnected,
    player1Connected: esp32Bridge.player1Connected,
    player2Connected: esp32Bridge.player2Connected
  });
  
  // Handle commands from client to ESP32
  socket.on("esp32_command", (command) => {
    console.log('Received command from client:', command);
    
    // Update lightboard state based on command
    if (command.cmd === 'awardPoint' && command.player && command.multiplier) {
      lightboardState.awardPoint(command.player, command.multiplier);
      console.log(`Updated lightboard state: Player ${command.player} awarded ${command.multiplier} point(s)`);
    } else if (command.cmd === 'reset') {
      // Client-initiated reset (from "Reset All Data") - reset everything including settings
      lightboardState.resetAll();
      console.log('Reset all lightboard data including settings');
      
      // Get the reset state with default settings
      const resetState = lightboardState.getGameState();
      
      // Send the reset command to ESP32
      esp32Bridge.sendToESP32(command);
      
      // Also send the updated default settings to ESP32 and emulator
      const settingsCommand = {
        cmd: 'lightboardSettings',
        mode: resetState.mode,
        p1Color: resetState.p1ColorIndex,
        p2Color: resetState.p2ColorIndex
      };
      esp32Bridge.sendToESP32(settingsCommand);
      io.emit('esp32_command', settingsCommand);
      
      // Broadcast the reset command to all connected clients (including lightboard emulator)
      io.emit('esp32_command', command);
      return; // Don't send reset command again below
    } else if (command.cmd === 'lightboardSettings' && command.mode !== undefined) {
      lightboardState.updateSettings(command.mode, command.p1Color, command.p2Color);
      console.log(`Updated lightboard settings: mode=${command.mode}, p1Color=${command.p1Color}, p2Color=${command.p2Color}`);
    }
    
    // Send command to ESP32 and broadcast to all clients (unless already handled above)
    esp32Bridge.sendToESP32(command);
    io.emit('esp32_command', command);
  });
  
  socket.on("disconnect", () => {
    console.log("Client disconnected:", socket.id);
  });
});

// Start ESP32 communication
esp32Bridge.startSerialCommunication();

// Periodic cleanup routine to prevent memory leaks
setInterval(() => {
  // Force garbage collection if available
  if (global.gc) {
    global.gc();
  }
  
  // Log memory usage every 10 minutes
  const memUsage = process.memoryUsage();
  console.log(`Memory usage - RSS: ${Math.round(memUsage.rss / 1024 / 1024)}MB, Heap: ${Math.round(memUsage.heapUsed / 1024 / 1024)}MB`);
}, 10 * 60 * 1000); // Every 10 minutes

const PORT = 80;
server.listen(PORT, '0.0.0.0', () => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
  console.log('ESP32 Bridge server started!');
  console.log('Web interface: http://localhost:80');
  console.log('Network access: http://[PI_IP]:80');
  console.log('Socket.IO server: ws://localhost:80');
  console.log('ESP32 connection is optional - server will run with or without ESP32');
  console.log('Press Ctrl+C to stop');
  console.log('\n=== REGISTERED API ROUTES ===');
  console.log('GET  /api - API info');
  console.log('GET  /api/version - Version check (NEW)');
  console.log('GET  /api/quiz-files - List quiz files');
  console.log('GET  /api/save-quiz-test - Test save-quiz route');
  console.log('POST /api/save-quiz - Save quiz (with file upload)');
  console.log('================================');
  console.log('Server version: 2.0.0-with-quiz-editor');
  console.log('If you do not see this version message, the server is running old code!\n');
});

// Handle graceful shutdown
process.on('SIGINT', () => {
  console.log('\nShutting down...');
  esp32Bridge.stop();
  process.exit(0);
});

process.on('SIGTERM', () => {
  console.log('\nShutting down...');
  esp32Bridge.stop();
  process.exit(0);
});

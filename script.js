// Socket.IO client for ESP32 communication
// Automatically connect to the same host as the current page
const socket = io();

// Connection status indicator
const connDot = document.getElementById('connDot');

// ESP32 connection status
let esp32Connected = false;
let esp32Enabled = false;

// Socket.IO event handlers
socket.on('connect', () => {
  console.log('Connected to server');
  connDot.className = 'ok';
});

socket.on('disconnect', () => {
  console.log('Disconnected from server');
  connDot.className = 'bad';
});

socket.on('esp32_status', (status) => {
  esp32Connected = status.connected;
  esp32Enabled = status.enabled;
  // Removed frequent heartbeat log to reduce console noise
  
  // Update connection indicator
  if (esp32Enabled && esp32Connected) {
    connDot.className = 'ok';
  } else if (esp32Enabled && !esp32Connected) {
    connDot.className = 'warning';
  } else {
    connDot.className = 'bad';
  }
});

socket.on('esp32_data', (data) => {
  // Removed frequent heartbeat log to reduce console noise
  // Only log non-status messages (hits, winners, etc.)
  if (data.type !== 'status') {
    console.log('Received from ESP32:', data);
  }
  
  // Handle different types of ESP32 messages
  if (data.type === 'hit') {
    console.log(`Player ${data.player} hit detected! Time: ${data.time}, Strength: ${data.strength}`);
    
    // Ignore hits without timestamp (duplicate/debug-originating events)
    if (!data.time) {
      console.log(`Ignoring hit without timestamp from Player ${data.player}`);
      return;
    }
    
    // Deduplication: check if this is a duplicate hit
    if (data.time && data.time === lastProcessedHitTime && data.player === lastProcessedHitPlayer) {
      console.log(`Duplicate hit from Player ${data.player} ignored (time: ${data.time})`);
      return;
    }
    
    // Update deduplication tracking
    if (data.time) {
      lastProcessedHitTime = data.time;
      lastProcessedHitPlayer = data.player;
    }
    
    // Backend now uses Player 1 and Player 2 directly, no translation needed
    handlePlayerHit(data.player);
  } else if (data.type === 'winner') {
    console.log(`Winner declared: ${data.winner}`);
    
    // Check if quiz is actually in play - ignore winner messages when quiz is not open
    if (!currentCategory || QA.length === 0 || quizDisplay.classList.contains('hidden')) {
      console.log(`Winner message for ${data.winner} ignored - quiz not in play`);
      return;
    }
    
    // Backend now uses Player 1 and Player 2 directly, no translation needed
    // Only process winner if round is not already complete
    if (!roundComplete) {
      showWinner(data.winner);
    } else {
      console.log(`Winner message for ${data.winner} ignored - round already complete`);
    }
  }
});

socket.on('esp32_status_message', (data) => {
  console.log('ESP32 Status:', data.message);
  // Display status messages in the console for now
  // You could add a status display area to the UI if needed
});

// Function to send commands to ESP32
function sendToESP32(command) {
  if (esp32Enabled && esp32Connected) {
    // Convert frontend command format to ESP32 expected format
    let esp32Command;
    if (command.action === 'reset') {
      esp32Command = { cmd: 'reset' };
    } else if (command.action === 'lightboardSettings') {
      esp32Command = { 
        cmd: 'lightboardSettings', 
        mode: command.mode, 
        p1Color: command.p1Color, // Backend now uses Player 1 and Player 2 directly
        p2Color: command.p2Color
      };
    } else if (command.action === 'awardPoint') {
      esp32Command = { 
        cmd: 'awardPoint', 
        player: command.player, // Backend now uses Player 1 and Player 2 directly, no translation needed
        multiplier: command.multiplier 
      };
    } else {
      esp32Command = command; // Pass through as-is
    }
    
    socket.emit('esp32_command', esp32Command);
    console.log('Sent to ESP32:', esp32Command);
  } else {
    console.warn('ESP32 not available - command ignored:', command);
  }
}

// === CSV PARSER ===
function parseCSV(csv) {
  // Handle different line endings (Windows \r\n, Mac \r, Unix \n)
  const lines = csv.split(/\r?\n/).filter(line => line.trim() !== '');
  
  if (lines.length === 0) return [];
  
  // Parse header line properly, handling quoted values
  const headerLine = lines[0];
  const headers = [];
  let current = '';
  let inQuotes = false;
  
  for (let j = 0; j < headerLine.length; j++) {
    const char = headerLine[j];
    if (char === '"') {
      inQuotes = !inQuotes;
    } else if (char === ',' && !inQuotes) {
      headers.push(current.trim().replace(/^"|"$/g, ''));
      current = '';
    } else {
      current += char;
    }
  }
  headers.push(current.trim().replace(/^"|"$/g, ''));
  
  const data = [];
  
  for (let i = 1; i < lines.length; i++) {
    if (lines[i].trim() === '') continue;
    
    const values = [];
    let current = '';
    let inQuotes = false;
    
    for (let j = 0; j < lines[i].length; j++) {
      const char = lines[i][j];
      
      if (char === '"') {
        inQuotes = !inQuotes;
      } else if (char === ',' && !inQuotes) {
        values.push(current.trim());
        current = '';
      } else {
        current += char;
      }
    }
    // Push the last value after the loop
    values.push(current.trim());
    
    // Ensure we have the same number of values as headers (pad with empty strings if needed)
    while (values.length < headers.length) {
      values.push('');
    }

    const row = {};
    headers.forEach((header, index) => {
      const cleanHeader = header.trim();
      const value = values[index] !== undefined ? values[index].replace(/^"|"$/g, '').trim() : '';
      row[cleanHeader] = value;
    });
    // Don't set row.Level here as it conflicts with the actual column structure
    // The row object now has properties matching the CSV headers exactly
    data.push(row);
  }
  
  return data;
}

// === LEVEL TO ICON MAPPING ===
function getIconPath(levelText) {
  if (!levelText || levelText.trim() === '') return null;
  
  // Handle special cases
  if (levelText.includes('Paddling')) {
    // Paddling uses different naming: "Paddling_Level_1.svg"
    const levelMatch = levelText.match(/Level\s+(\d+)/i);
    if (levelMatch) {
      return `Images/Paddling_Level_${levelMatch[1]}.svg`;
    }
    return null;
  }
  
  // Standard format: "Category Level N" -> "AS_Category_CN.jpg"
  const match = levelText.match(/(.+?)\s+Level\s+(\d+)/i);
  if (!match) return null;
  
  const category = match[1].trim();
  const level = match[2];
  
  // Convert category name to image filename format
  // "Camping" -> "Camping", "Air Activities" -> "Air_Activities"
  const categoryFormatted = category.replace(/\s+/g, '_');
  const imagePath = `Images/AS_${categoryFormatted}_C${level}.jpg`;
  
  return imagePath;
}

// === CHECK IF THIRD COLUMN IS AUDIO FILE ===
function isAudioFile(value) {
  if (!value || value.trim() === '') return false;
  // Check if it contains "Level" - if so, it's not an audio file
  if (value.includes('Level')) return false;
  // Check if it looks like an audio file (has audio extension)
  const audioExtensions = ['.mp3', '.wav', '.ogg', '.m4a', '.aac', '.flac', '.wma'];
  const lowerValue = value.toLowerCase();
  return audioExtensions.some(ext => lowerValue.endsWith(ext));
}

// === APP STATE ===
let QA = [];
let currentCategory = '';
let availableCategories = [];
let player1Score = 0;
let player2Score = 0;

// Image preloading cache
let imageCache = new Map(); // Map of imagePath -> Image object

// Music quiz state
let currentQuizType = 'regular'; // 'regular' or 'music'
let currentAudio = null;
let isMusicPlaying = false;

// DOM Elements
const fileInput = document.getElementById('csvFile');
const fileInputSection = document.getElementById('fileInputSection');
const loadedFiles = document.getElementById('loadedFiles');
const fileList = document.getElementById('fileList');
const categorySelector = document.getElementById('categorySelector');
const categoryGrid = document.getElementById('categoryGrid');
const categorySelection = document.getElementById('categorySelection');
const categoryItems = document.getElementById('categoryItems');
const totalQuestions = document.getElementById('totalQuestions');
const totalNumber = document.getElementById('totalNumber');
const startCustomQuiz = document.getElementById('startCustomQuiz');
const selectAllCheckbox = document.getElementById('selectAllCheckbox');
const backToSimpleCategories = document.getElementById('backToSimpleCategories');
const quizInterface = document.getElementById('quizInterface');
const quizDisplay = document.getElementById('quizDisplay');
const quizTitle = document.getElementById('quizTitle');
const qEl = document.getElementById('q');
const aEl = document.getElementById('a');
const answerText = document.getElementById('answerText');
const categoryBadge = document.getElementById('categoryBadge');
const questionIcon = document.getElementById('questionIcon');
const counterEl = document.getElementById('counter');
const btnPrev = document.getElementById('prev');
const btnNext = document.getElementById('next');
const btnToggle = document.getElementById('toggle');
const btnExit = document.getElementById('exitBtn');
const card = document.getElementById('card');
const player1ScoreEl = document.getElementById('player1Score');
const player2ScoreEl = document.getElementById('player2Score');

// Music quiz elements
const musicControls = document.getElementById('musicControls');
const playMusicBtn = document.getElementById('playMusicBtn');
const musicStatus = document.getElementById('musicStatus');

// Reset button
const resetAllData = document.getElementById('resetAllData');
const resetModal = document.getElementById('resetModal');
const cancelReset = document.getElementById('cancelReset');
const confirmReset = document.getElementById('confirmReset');

// Lightboard settings
const lightboardSettingsBtn = document.getElementById('lightboardSettingsBtn');
const lightboardModal = document.getElementById('lightboardModal');
const cancelLightboard = document.getElementById('cancelLightboard');
const confirmLightboard = document.getElementById('confirmLightboard');
const lightboardMode = document.getElementById('lightboardMode');
const lightboardP1Color = document.getElementById('lightboardP1Color');
const lightboardP2Color = document.getElementById('lightboardP2Color');
const damageMultiplier = document.getElementById('damageMultiplier');

// Quiz Editor elements (will be initialized in DOMContentLoaded)
let quizEditorBtn;
let quizEditorDisplay;
let exitQuizEditorBtn;
let saveQuizBtn;
let newQuizMode;
let editQuizMode;
let newQuizSection;
let editQuizSection;
let quizName;
let existingQuizSelect;
let questionsList;
let addQuestionBtn;
let quizEditorExitModal;
let cancelQuizEditorExit;
let saveAndExitQuizEditor;
let exitWithoutSavingQuizEditor;
let quizQuestionsSection;
let quizEditorSaveSection;

// Game status elements
const gameStatus = document.getElementById('gameStatus');
const player1Tile = document.getElementById('player1Tile');
const player2Tile = document.getElementById('player2Tile');
const player1Name = document.querySelector('#player1Tile .player-name');
const player2Name = document.querySelector('#player2Tile .player-name');

let order = [];
let idx = 0;

// Lightboard settings
let lightboardGameMode = 1; // Default to Territory mode
let lightboardP1ColorIndex = 0; // Red
let lightboardP2ColorIndex = 1; // Blue
let damageMultiplierValue = 3; // Default to triple damage

// Debounce awarding to avoid duplicate points on multiple hit logs
let lastAwardAtMs = 0;
let lastAwardPlayer = null;

// Hit deduplication to prevent processing duplicate hit messages
let lastProcessedHitTime = 0;
let lastProcessedHitPlayer = null;

// Player names with persistence
let player1NameText = 'Player 1';
let player2NameText = 'Player 2';
let isEditingName = false;

// Quiz state persistence
let currentQuestionIndex = 0;
let savedOrder = null; // Store the shuffled order to restore exactly

// Scoring system
let roundComplete = false;

// Sample quiz data (you can replace this with your CSV data)
const sampleQuestions = [
  { q: "What is the capital of France?", a: "Paris", category: "Geography" },
  { q: "What is 2 + 2?", a: "4", category: "Math" },
  { q: "What is the largest planet in our solar system?", a: "Jupiter", category: "Science" },
  { q: "Who wrote Romeo and Juliet?", a: "William Shakespeare", category: "Literature" },
  { q: "What is the chemical symbol for gold?", a: "Au", category: "Science" },
  { q: "What year did World War II end?", a: "1945", category: "History" },
  { q: "What is the main component of the sun?", a: "Hydrogen", category: "Science" },
  { q: "What is the largest ocean on Earth?", a: "Pacific Ocean", category: "Geography" },
  { q: "What is the square root of 144?", a: "12", category: "Math" },
  { q: "Who painted the Mona Lisa?", a: "Leonardo da Vinci", category: "Art" }
];

// === FILE HANDLING ===
fileInput.addEventListener('change', handleFileSelect);

function handleFileSelect(event) {
  const files = event.target.files;
  if (files.length === 0) return;

  // Clear previous categories
  availableCategories = [];
  fileList.innerHTML = '';
  
  let loadedCount = 0;
  const totalFiles = files.length;

  Array.from(files).forEach((file, index) => {
    if (file.type === 'text/csv' || file.name.endsWith('.csv')) {
      const reader = new FileReader();
      
      reader.onload = function(e) {
        try {
          const csvText = e.target.result;
          const csvData = parseCSV(csvText);
          
          // Convert CSV data to the expected format
          const questions = csvData.map(row => {
            const question = row.Question || row.question || row.Q || row.q || Object.values(row)[0];
            const answer = row.Answer || row.answer || row.A || row.a || Object.values(row)[1];
            
            const questionObj = { q: question, a: answer };
            
            // Get column values - handle CSVs with 2, 3, or 4 columns
            const rowValues = Object.values(row);
            // Column 3 (index 2): always for audio files (if it exists)
            // Support multiple header names: Audio, Music, File, etc.
            const audioColumn = row.Audio || row.audio || row.AudioFile || row.audioFile || row.Music || row.music || row.File || row.file || (rowValues.length > 2 ? rowValues[2] : '') || '';
            // Column 4 (index 3): always for badge category/level (if it exists)
            // IMPORTANT: Don't use row.Level as it's set to the wrong column by parseCSV
            // Support multiple header names: Badge, Icon, Category, Level, etc.
            const badgeColumn = row.Badge || row.badge || row.Icon || row.icon || row.Category || row.category || (rowValues.length > 3 ? rowValues[3] : '') || '';
            
            // Set audio file from column 3 (only if it exists and is not empty)
            if (audioColumn && audioColumn.trim() !== '') {
              questionObj.audioFile = audioColumn.trim();
            }
            
            // Set badge/image from column 4 (only if it exists and is not empty)
            if (badgeColumn && badgeColumn.trim() !== '') {
              questionObj.level = badgeColumn.trim();
              // Check if column 4 is a direct image path (has image extension)
              const imageExtensions = ['.png', '.svg', '.webp', '.jpg', '.jpeg', '.gif'];
              const isImagePath = imageExtensions.some(ext => 
                badgeColumn.trim().toLowerCase().endsWith(ext)
              );
              
              if (isImagePath) {
                // Column 4 is a direct image path
                let imagePath = badgeColumn.trim();
                
                // If it doesn't have a path prefix, assume it's in Images folder
                if (!imagePath.startsWith('http') && !imagePath.startsWith('/') && !imagePath.startsWith('Images/') && !imagePath.includes('/')) {
                  // Just a filename - prepend Images/
                  imagePath = `Images/${imagePath}`;
                }
                questionObj.iconPath = imagePath;
              } else {
                // Column 4 is a badge name - try standard icon path
                questionObj.iconPath = getIconPath(badgeColumn.trim());
              }
            }
            
            return questionObj;
          }).filter(qa => qa.q && qa.a);
          
          if (questions.length > 0) {
            const categoryName = file.name.replace('.csv', '').replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
            availableCategories.push({
              filename: file.name,
              name: categoryName,
              questions: questions
            });
            
            // Add success message to file list
            addFileToList(file.name, `${questions.length} questions`, 'success');
          } else {
            addFileToList(file.name, 'No questions', 'error');
          }
        } catch (error) {
          console.error('Error parsing CSV:', error);
          addFileToList(file.name, 'Error', 'error');
        }
        
        loadedCount++;
        
        // If all files are processed, show category selector
        if (loadedCount === totalFiles) {
          if (availableCategories.length > 0) {
            showCategorySelector();
            createCategoryButtons(availableCategories);
          }
        }
      };
      
      reader.onerror = function() {
        addFileToList(file.name, 'Read failed', 'error');
        loadedCount++;
        
        if (loadedCount === totalFiles) {
          if (availableCategories.length > 0) {
            showCategorySelector();
            createCategoryButtons(availableCategories);
          }
        }
      };
      
      reader.readAsText(file);
    } else {
      addFileToList(file.name, 'Not CSV', 'error');
      loadedCount++;
      
      if (loadedCount === totalFiles) {
        if (availableCategories.length > 0) {
          showCategorySelector();
          createCategoryButtons(availableCategories);
        }
      }
    }
  });
  
  // Show the loaded files section
  loadedFiles.classList.remove('hidden');
}

function addFileToList(filename, message, status) {
  const li = document.createElement('li');
  li.className = status;
  li.innerHTML = `${filename.replace('.csv', '')}: ${message}`;
  fileList.appendChild(li);
}

// === CATEGORY SELECTION ===
function showCategorySelector() {
  categorySelector.classList.remove('hidden');
  categorySelection.classList.add('hidden');
  quizDisplay.classList.add('hidden');
  if (quizEditorDisplay) quizEditorDisplay.classList.add('hidden');
  fileInputSection.classList.remove('hidden');
  resetAllData.parentElement.classList.remove('hidden');
  lightboardSettingsBtn.parentElement.classList.remove('hidden');
  const quizEditorSection = document.getElementById('quizEditorSection');
  if (quizEditorSection) quizEditorSection.classList.remove('hidden');
  currentCategory = '';
  QA = [];
  console.log('Category selector shown, file input visible');
}

function showCustomMixBuilder(categories) {
  categorySelector.classList.add('hidden');
  categorySelection.classList.remove('hidden');
  quizDisplay.classList.add('hidden');
  if (quizEditorDisplay) quizEditorDisplay.classList.add('hidden');
  fileInputSection.classList.add('hidden');
  resetAllData.parentElement.classList.add('hidden');
  lightboardSettingsBtn.parentElement.classList.add('hidden');
  const quizEditorSection = document.getElementById('quizEditorSection');
  if (quizEditorSection) quizEditorSection.classList.add('hidden');
  
  createCategorySelectionItems(categories);
  console.log('Custom mix builder shown');
}

function showQuizDisplay() {
  categorySelector.classList.add('hidden');
  categorySelection.classList.add('hidden');
  quizDisplay.classList.remove('hidden');
  if (quizEditorDisplay) quizEditorDisplay.classList.add('hidden');
  fileInputSection.classList.add('hidden');
  resetAllData.parentElement.classList.add('hidden');
  lightboardSettingsBtn.parentElement.classList.add('hidden');
  const quizEditorSection = document.getElementById('quizEditorSection');
  if (quizEditorSection) quizEditorSection.classList.add('hidden');
  console.log('Quiz interface shown, file input hidden');
}

function createCategoryButtons(categories) {
  if (categories.length === 0) {
    categoryGrid.innerHTML = `
      <div class="category-btn" style="grid-column: 1 / -1; color: var(--muted);">
        No valid CSV files loaded. Please select CSV files with Question,Answer format.
      </div>
    `;
    return;
  }

  // Add "Custom Mix" button if there are multiple categories
  let buttonsHTML = '';
  if (categories.length > 1) {
    buttonsHTML += `
      <div class="category-btn custom-mix" style="grid-column: 1 / -1; background: linear-gradient(180deg, rgba(106,161,255,.25), rgba(106,161,255,.15)); border-color: var(--accent);">
        <div style="font-size: 18px; margin-bottom: 4px;">⚙️ Custom Mix Builder</div>
        <div style="font-size: 12px; color: var(--muted);">Select categories and set question ratios</div>
      </div>
    `;
  }

  // Add individual category buttons
  buttonsHTML += categories.map(category => {
    const musicEmoji = category.type === 'music' ? '<div class="category-music-icon">🎵</div>' : '';
    return `
      <div class="category-btn" data-filename="${category.filename}">
        ${musicEmoji}
        <div class="category-name-container">
          <span class="category-name-text">${category.name}</span>
        </div>
        <div style="font-size: 12px; color: var(--muted);">${category.questions.length} questions</div>
      </div>
    `;
  }).join('');

  categoryGrid.innerHTML = buttonsHTML;

  // Check for text overflow and enable scrolling after layout
  requestAnimationFrame(() => {
    // Create a single style element for all animations
    let styleElement = document.getElementById('scroll-animations-style');
    if (!styleElement) {
      styleElement = document.createElement('style');
      styleElement.id = 'scroll-animations-style';
      document.head.appendChild(styleElement);
    }
    
    let keyframesText = '';
    
    categoryGrid.querySelectorAll('.category-name-container').forEach((container, index) => {
      const textSpan = container.querySelector('.category-name-text');
      if (textSpan) {
        // Check if text overflows
        const containerWidth = container.offsetWidth;
        const textWidth = textSpan.scrollWidth;
        
        if (textWidth > containerWidth) {
          container.classList.add('needs-scroll');
          const scrollGap = 48;
          
          // Calculate distances for constant speed
          // First scroll: moves from 0 to -textWidth (distance = textWidth)
          const firstScrollDistance = textWidth;
          // Loop scroll: moves from containerWidth to -textWidth - gap (distance = containerWidth + textWidth + gap)
          const loopScrollDistance = containerWidth + textWidth + scrollGap;
          // Total distance for one complete cycle
          const totalDistance = firstScrollDistance + loopScrollDistance;
          
          // Calculate percentage for first scroll based on distance (for constant speed)
          const firstCyclePercentage = (firstScrollDistance / totalDistance) * 100;
          
          // Calculate animation duration based on total distance
          const pxPerSecond = 120; // Slower speed for smoother animation
          const durationSeconds = Math.max(2, totalDistance / pxPerSecond);
          
          container.style.setProperty('--text-width', `${textWidth}px`);
          container.style.setProperty('--container-width', `${containerWidth}px`);
          container.style.setProperty('--scroll-gap', `${scrollGap}px`);
          container.style.setProperty('--scroll-duration', `${durationSeconds.toFixed(2)}s`);
          container.style.setProperty('--first-cycle-end', `${firstCyclePercentage.toFixed(2)}%`);
          
          // Create unique animation name for this container
          const animationName = `scroll-text-hover-${index}`;
          container.style.setProperty('--animation-name', animationName);
          
          // Add keyframes to the style element
          // Structure: The animation loops from right to left continuously
          // We'll use a negative delay on first play to start from center
          // Calculate what percentage "center" would be in the loop
          // The loop goes: right -> left, so center is at the start of that journey
          // But we want first cycle to be: center -> left -> (then loop from right)
          
          // The loop portion: from right (containerWidth) to left (-textWidth - gap)
          // This takes: (100 - firstCyclePercentage - 0.01)% of the animation
          // We want the animation to loop seamlessly, so 0% and 100% should be the same state
          // But we also need the first cycle to start from center
          
          // Solution: Make the animation end at the loop start position
          // Then use animation-direction or a wrapper animation
          
          // Better: Create two animations - one for first cycle, one for loop
          // Or: Make the animation so 0% is loop start, and use a negative delay to start from center
          
          const loopDuration = durationSeconds * (1 - firstCyclePercentage / 100);
          const firstCycleDuration = durationSeconds * (firstCyclePercentage / 100);
          
          keyframesText += `
            @keyframes ${animationName}-first {
              /* First cycle: center to left */
              0% {
                transform: translateX(0);
              }
              100% {
                transform: translateX(calc(-1 * ${textWidth}px));
              }
            }
            @keyframes ${animationName}-loop {
              /* Loop: right to left continuously */
              0% {
                transform: translateX(${containerWidth}px);
              }
              100% {
                transform: translateX(calc(-1 * ${textWidth}px - ${scrollGap}px));
              }
            }
          `;
          
          // Set CSS variables for the animations
          container.style.setProperty('--first-anim', `${animationName}-first`);
          container.style.setProperty('--loop-anim', `${animationName}-loop`);
          container.style.setProperty('--first-duration', `${firstCycleDuration.toFixed(2)}s`);
          container.style.setProperty('--loop-duration', `${loopDuration.toFixed(2)}s`);
          
          // Store as attributes for potential JS use
          container.setAttribute('data-first-anim', `${animationName}-first`);
          container.setAttribute('data-loop-anim', `${animationName}-loop`);
          container.setAttribute('data-first-duration', `${firstCycleDuration.toFixed(2)}s`);
          container.setAttribute('data-loop-duration', `${loopDuration.toFixed(2)}s`);
        }
      }
    });
    
    // Set all keyframes at once
    if (keyframesText) {
      styleElement.textContent = keyframesText;
    }
  });

  // Add click handlers
  categoryGrid.querySelectorAll('.category-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      if (btn.classList.contains('custom-mix')) {
        showCustomMixBuilder(categories);
      } else {
        const filename = btn.dataset.filename;
        loadCategory(filename);
      }
    });
  });
}

// === IMAGE PRELOADING ===
function preloadImages(questions) {
  // Collect all unique icon paths
  const iconPaths = new Set();
  questions.forEach(qa => {
    if (qa.iconPath) {
      iconPaths.add(qa.iconPath);
    }
  });
  
  // Preload each image
  iconPaths.forEach(iconPath => {
    if (!imageCache.has(iconPath)) {
      const img = new Image();
      img.src = iconPath;
      imageCache.set(iconPath, img);
    }
  });
  
  console.log(`Preloaded ${iconPaths.size} images`);
}

// === LOAD CATEGORY ===
function loadCategory(filename) {
  const category = availableCategories.find(cat => cat.filename === filename);
  if (!category) {
    alert('Category not found');
    return;
  }

  QA = category.questions;
  currentCategory = category.name;
  currentQuizType = category.type || 'regular';
  quizTitle.textContent = currentCategory;
  
  // Preload all images for this category
  preloadImages(QA);
  
  // Music controls visibility will be handled per-question in render()
  // Initially hide them, they'll be shown if the first question has audio
  musicControls.classList.add('hidden');
  stopMusic();
  
  // Clear any winner state from previous quiz
  hideWinner();
  
  showQuizDisplay();
  setOrder(true);
  render(true);
  enableControls();
}

function createCategorySelectionItems(categories) {
  categoryItems.innerHTML = '';
  
  categories.forEach((category, index) => {
    const item = document.createElement('div');
    item.className = 'category-item';
    item.innerHTML = `
      <input type="checkbox" class="category-checkbox" id="cat-${index}" data-index="${index}">
      <div class="category-info">
        <div class="category-name">${category.name}</div>
        <div class="category-count">${category.questions.length} questions</div>
      </div>
      <div class="ratio-controls">
        <span class="ratio-label">Ratio:</span>
        <input type="range" class="ratio-slider" id="ratio-${index}" min="1" max="10" value="5" data-index="${index}">
        <span class="ratio-value" id="ratio-value-${index}">5</span>
      </div>
    `;
    
    categoryItems.appendChild(item);
    
    // Add event listeners
    const checkbox = item.querySelector('.category-checkbox');
    const slider = item.querySelector('.ratio-slider');
    const ratioValue = item.querySelector('.ratio-value');
    
    checkbox.addEventListener('change', () => {
      item.classList.toggle('selected', checkbox.checked);
      updateSelectAllCheckbox();
      updateTotalQuestions();
    });
    
    slider.addEventListener('input', () => {
      ratioValue.textContent = slider.value;
      updateTotalQuestions();
    });
  });
  
  updateSelectAllCheckbox();
  updateTotalQuestions();
}

function updateSelectAllCheckbox() {
  const allCheckboxes = categoryItems.querySelectorAll('.category-checkbox');
  const checkedCount = categoryItems.querySelectorAll('.category-checkbox:checked').length;
  
  if (allCheckboxes.length === 0) {
    selectAllCheckbox.checked = false;
    selectAllCheckbox.indeterminate = false;
    return;
  }
  
  if (checkedCount === 0) {
    selectAllCheckbox.checked = false;
    selectAllCheckbox.indeterminate = false;
  } else if (checkedCount === allCheckboxes.length) {
    selectAllCheckbox.checked = true;
    selectAllCheckbox.indeterminate = false;
  } else {
    selectAllCheckbox.checked = false;
    selectAllCheckbox.indeterminate = true;
  }
}

function updateTotalQuestions() {
  const selectedCategories = [];
  let totalRatio = 0;
  
  categoryItems.querySelectorAll('.category-checkbox:checked').forEach(checkbox => {
    const index = parseInt(checkbox.dataset.index);
    const slider = document.getElementById(`ratio-${index}`);
    const ratio = parseInt(slider.value);
    
    selectedCategories.push({
      index: index,
      ratio: ratio
    });
    totalRatio += ratio;
  });
  
  if (selectedCategories.length === 0) {
    totalQuestions.classList.add('hidden');
    startCustomQuiz.disabled = true;
    return;
  }
  
  // Calculate total questions based on ratios
  // The ratio determines the proportion of questions from each category
  // We scale the ratios to fit within available questions
  
  // First, calculate proportional amounts for each category
  const proportionalAmounts = selectedCategories.map(cat => {
    const category = availableCategories[cat.index];
    const ratio = cat.ratio / totalRatio;
    // Use a high base to calculate proportions, then scale down
    const proportional = ratio * 1000; // High base for precision
    return {
      index: cat.index,
      ratio: cat.ratio,
      proportional: proportional,
      available: category.questions.length
    };
  });
  
  // Find the scale factor needed so no category exceeds its available questions
  let scaleFactor = 1;
  proportionalAmounts.forEach(item => {
    if (item.proportional > item.available) {
      const neededScale = item.available / item.proportional;
      if (neededScale < scaleFactor) {
        scaleFactor = neededScale;
      }
    }
  });
  
  // Calculate final question counts
  let totalQuestionsCount = 0;
  proportionalAmounts.forEach(item => {
    const questionsForCategory = Math.round(item.proportional * scaleFactor);
    totalQuestionsCount += questionsForCategory;
  });
  
  totalNumber.textContent = totalQuestionsCount;
  totalQuestions.classList.remove('hidden');
  startCustomQuiz.disabled = false;
}

function loadCustomQuiz() {
  const selectedCategories = [];
  let totalRatio = 0;
  
  categoryItems.querySelectorAll('.category-checkbox:checked').forEach(checkbox => {
    const index = parseInt(checkbox.dataset.index);
    const slider = document.getElementById(`ratio-${index}`);
    const ratio = parseInt(slider.value);
    
    selectedCategories.push({
      index: index,
      ratio: ratio
    });
    totalRatio += ratio;
  });
  
  if (selectedCategories.length === 0) {
    alert('Please select at least one category');
    return;
  }
  
  // Build the custom quiz
  QA = [];
  const categoryNames = [];
  let hasMusicQuiz = false;
  
  // Calculate proportional amounts for each category (same logic as updateTotalQuestions)
  const proportionalAmounts = selectedCategories.map(cat => {
    const category = availableCategories[cat.index];
    const ratio = cat.ratio / totalRatio;
    const proportional = ratio * 1000; // High base for precision
    return {
      index: cat.index,
      ratio: cat.ratio,
      proportional: proportional,
      available: category.questions.length,
      category: category
    };
  });
  
  // Find the scale factor needed so no category exceeds its available questions
  let scaleFactor = 1;
  proportionalAmounts.forEach(item => {
    if (item.proportional > item.available) {
      const neededScale = item.available / item.proportional;
      if (neededScale < scaleFactor) {
        scaleFactor = neededScale;
      }
    }
  });
  
  // Calculate and load questions for each category
  proportionalAmounts.forEach(item => {
    const questionsForCategory = Math.round(item.proportional * scaleFactor);
    
    // Shuffle the category questions and take the required number
    const shuffledQuestions = shuffle([...item.category.questions]);
    const selectedQuestions = shuffledQuestions.slice(0, questionsForCategory);
    
    // Add category information to each question
    const questionsWithCategory = selectedQuestions.map(qa => ({
      ...qa,
      category: item.category.name
    }));
    
    QA = QA.concat(questionsWithCategory);
    categoryNames.push(item.category.name);
    
    // Check if any category is a music quiz
    if (item.category.type === 'music') {
      hasMusicQuiz = true;
    }
  });
  
  // Shuffle the final combined questions
  QA = shuffle(QA);
  
  // Format category title - limit to 3 category names, otherwise show count
  let categoryTitle;
  if (categoryNames.length <= 3) {
    categoryTitle = categoryNames.join(', ');
  } else {
    categoryTitle = `${categoryNames.length} Categories`;
  }
  
  currentCategory = `Custom Mix: ${categoryTitle}`;
  currentQuizType = hasMusicQuiz ? 'music' : 'regular';
  quizTitle.textContent = currentCategory;
  
  // Preload all images for custom quiz
  preloadImages(QA);
  
  // Music controls visibility will be handled per-question in render()
  // Initially hide them, they'll be shown if the first question has audio
  musicControls.classList.add('hidden');
  stopMusic();
  
  // Clear any winner state from previous quiz
  hideWinner();
  
  showQuizDisplay();
  setOrder(true);
  render(true);
  enableControls();
}

function enableControls() {
  btnNext.disabled = false;
  btnPrev.disabled = false;
  btnToggle.disabled = false;
}

function shuffle(arr){
  for (let i = arr.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [arr[i], arr[j]] = [arr[j], arr[i]];
  }
  return arr;
}

function setOrder(randomize) {
  order = [...Array(QA.length).keys()];
  if (randomize) shuffle(order);
  idx = 0;
}

function render(hideAnswer = true) {
  if (QA.length === 0) return;
  
  const qa = QA[ order[idx] ];
  qEl.textContent = qa.q;
  answerText.textContent = qa.a;
  
  // Show/hide category badge based on whether question has category info
  if (qa.category) {
    categoryBadge.textContent = qa.category;
    categoryBadge.classList.remove('hidden');
  } else {
    categoryBadge.classList.add('hidden');
  }
  
  // Show/hide music controls based on whether current question has audio file
  // Only show if audioFile exists AND is not undefined/null/empty
  if (qa.audioFile && qa.audioFile.trim && qa.audioFile.trim() !== '') {
    musicControls.classList.remove('hidden');
  } else {
    musicControls.classList.add('hidden');
    stopMusic(); // Stop any playing music when switching to non-music question
  }
  
  // Add class to quiz card when both category badge and music controls are visible
  // This helps position them correctly to avoid overlap
  const hasCategoryBadge = !categoryBadge.classList.contains('hidden');
  const hasMusicControls = !musicControls.classList.contains('hidden');
  if (hasCategoryBadge && hasMusicControls) {
    card.classList.add('has-both-controls');
  } else {
    card.classList.remove('has-both-controls');
  }
  
  // Show/hide question icon based on whether question has icon path
  // Use preloaded image from cache for instant display
  if (qa.iconPath) {
    // Preloading ensures images are in browser cache, so setting src should be instant
    questionIcon.src = qa.iconPath;
    questionIcon.alt = qa.level || 'Badge icon';
    questionIcon.classList.remove('hidden');
    // Handle image load errors gracefully
    questionIcon.onerror = function() {
      console.error(`Failed to load image: ${qa.iconPath}`);
      console.error(`Question: ${qa.q}`);
      console.error(`Badge value in CSV: ${qa.level}`);
      this.classList.add('hidden');
    };
    questionIcon.onload = function() {
      console.log(`Successfully loaded image: ${qa.iconPath}`);
    };
  } else {
    questionIcon.classList.add('hidden');
  }
  
  if (hideAnswer) {
    aEl.classList.remove('show');
    btnToggle.textContent = 'Show Answer';
  }
  counterEl.textContent = `${idx+1} / ${QA.length}`;
}

function next() {
  if (idx < order.length - 1) { 
    idx++; 
    render(); 
  } else {
    idx = 0; 
    render(); 
  }
  // Reset game state when navigating to new question
  hideWinner();
  removeScorableState();
  // Stop music when navigating to new question
  stopMusic();
}

function prev() {
  if (idx > 0) { 
    idx--; 
    render(); 
  } else {
    idx = order.length - 1; 
    render(); 
  }
  // Reset game state when navigating to new question
  hideWinner();
  removeScorableState();
  // Stop music when navigating to new question
  stopMusic();
}

function toggleAnswer() {
  aEl.classList.toggle('show');
  btnToggle.textContent = aEl.classList.contains('show') ? 'Hide Answer' : 'Show Answer';
}

// === MUSIC QUIZ FUNCTIONS ===
function playMusic() {
  if (QA.length === 0) return;
  
  const currentQuestion = QA[order[idx]];
  if (!currentQuestion.audioFile) {
    musicStatus.textContent = '';
    return;
  }
  
  // Stop any currently playing music
  stopMusic();
  
  // Find the current category to get the folder path
  // For custom mix quizzes, use the question's category property
  // For regular quizzes, find by matching the QA array or category name
  let currentCategoryData;
  if (currentQuestion.category && currentCategory && currentCategory.startsWith('Custom Mix:')) {
    // Custom mix quiz - find category by the question's category property
    currentCategoryData = availableCategories.find(cat => cat.name === currentQuestion.category);
  } else {
    // Regular quiz - find by matching QA array or category name
    currentCategoryData = availableCategories.find(cat => 
      cat.questions === QA || cat.name === currentCategory
    );
  }
  
  if (!currentCategoryData) {
    console.warn('Could not find category data for audio file:', currentQuestion.audioFile);
    musicStatus.textContent = '';
    return;
  }
  
  // Construct the full path to the audio file
  // The audio file should be in the same folder as the CSV
  const categoryFolder = currentCategoryData.filename.replace('.csv', '');
  const audioPath = `Quizes/${categoryFolder}/${currentQuestion.audioFile}`;
  
  console.log('Loading audio file:', audioPath);
  
  // Create new audio element
  currentAudio = new Audio(audioPath);
  
  currentAudio.addEventListener('loadstart', () => {
    musicStatus.textContent = '';
    playMusicBtn.textContent = '⏳ Loading...';
    playMusicBtn.disabled = true;
  });
  
  currentAudio.addEventListener('canplay', () => {
    musicStatus.textContent = '';
    playMusicBtn.disabled = false;
    // Only update button text if not already playing
    if (!isMusicPlaying) {
      playMusicBtn.textContent = '🎵 Play Song';
    }
  });
  
  currentAudio.addEventListener('play', () => {
    isMusicPlaying = true;
    playMusicBtn.textContent = '⏸️ Stop Song';
    playMusicBtn.classList.add('playing');
    musicStatus.textContent = '';
  });
  
  currentAudio.addEventListener('pause', () => {
    isMusicPlaying = false;
    playMusicBtn.textContent = '🎵 Play Song';
    playMusicBtn.classList.remove('playing');
    musicStatus.textContent = '';
  });
  
  currentAudio.addEventListener('ended', () => {
    isMusicPlaying = false;
    playMusicBtn.textContent = '🎵 Play Song';
    playMusicBtn.classList.remove('playing');
    musicStatus.textContent = '';
  });
  
  currentAudio.addEventListener('error', (e) => {
    console.error('Audio error:', e);
    musicStatus.textContent = '';
    playMusicBtn.textContent = '🎵 Play Song';
    playMusicBtn.disabled = false;
    playMusicBtn.classList.remove('playing');
  });
  
  // Start playing
  currentAudio.play().then(() => {
    // Audio started playing successfully
    isMusicPlaying = true;
    playMusicBtn.textContent = '⏸️ Stop Song';
    playMusicBtn.classList.add('playing');
    musicStatus.textContent = '';
  }).catch(error => {
    console.error('Error playing audio:', error);
    musicStatus.textContent = '';
    playMusicBtn.textContent = '🎵 Play Song';
    playMusicBtn.disabled = false;
  });
}

function stopMusic() {
  if (currentAudio) {
    currentAudio.pause();
    currentAudio.currentTime = 0;
    currentAudio = null;
  }
  isMusicPlaying = false;
  if (playMusicBtn) {
    playMusicBtn.textContent = '🎵 Play Song';
    playMusicBtn.classList.remove('playing');
    playMusicBtn.disabled = false;
  }
  if (musicStatus) {
    musicStatus.textContent = '';
  }
}

// === PERSISTENCE FUNCTIONS ===
function savePersistedData() {
  try {
    // Save player names
    localStorage.setItem('player1Name', player1NameText);
    localStorage.setItem('player2Name', player2NameText);
    
    // Save scores
    localStorage.setItem('player1Score', player1Score.toString());
    localStorage.setItem('player2Score', player2Score.toString());
    
    // Save lightboard settings
    localStorage.setItem('lightboardGameMode', lightboardGameMode.toString());
    localStorage.setItem('lightboardP1ColorIndex', lightboardP1ColorIndex.toString());
    localStorage.setItem('lightboardP2ColorIndex', lightboardP2ColorIndex.toString());
    localStorage.setItem('damageMultiplierValue', damageMultiplierValue.toString());
    
    // Save categories
    localStorage.setItem('quizCategories', JSON.stringify(availableCategories));
    
    // Save current quiz state
    if (currentCategory) {
      localStorage.setItem('currentCategory', currentCategory);
      localStorage.setItem('currentQuestionIndex', currentQuestionIndex.toString());
      if (order.length > 0) {
        localStorage.setItem('savedOrder', JSON.stringify(order));
      }
    }
  } catch (error) {
    console.error('Error saving persisted data:', error);
  }
}

function loadPersistedData() {
  try {
    // Load player names
    const savedPlayer1Name = localStorage.getItem('player1Name');
    const savedPlayer2Name = localStorage.getItem('player2Name');
    if (savedPlayer1Name) player1NameText = savedPlayer1Name;
    if (savedPlayer2Name) player2NameText = savedPlayer2Name;
    
    // Load scores
    const savedPlayer1Score = localStorage.getItem('player1Score');
    const savedPlayer2Score = localStorage.getItem('player2Score');
    if (savedPlayer1Score) player1Score = parseInt(savedPlayer1Score);
    if (savedPlayer2Score) player2Score = parseInt(savedPlayer2Score);
    
    // Load lightboard settings
    const savedMode = localStorage.getItem('lightboardGameMode');
    const savedP1Color = localStorage.getItem('lightboardP1ColorIndex');
    const savedP2Color = localStorage.getItem('lightboardP2ColorIndex');
    const savedMultiplier = localStorage.getItem('damageMultiplierValue');
    
    if (savedMode !== null) lightboardGameMode = parseInt(savedMode);
    if (savedP1Color !== null) lightboardP1ColorIndex = parseInt(savedP1Color);
    if (savedP2Color !== null) lightboardP2ColorIndex = parseInt(savedP2Color);
    if (savedMultiplier !== null) damageMultiplierValue = parseInt(savedMultiplier);
    
    // Load categories
    const savedCategories = localStorage.getItem('quizCategories');
    if (savedCategories) {
      availableCategories = JSON.parse(savedCategories);
    }
    
    // Update UI
    player1Name.textContent = player1NameText;
    player2Name.textContent = player2NameText;
    player1ScoreEl.textContent = player1Score;
    player2ScoreEl.textContent = player2Score;
    
    // Update lightboard settings UI
    lightboardMode.value = lightboardGameMode;
    lightboardP1Color.value = lightboardP1ColorIndex;
    lightboardP2Color.value = lightboardP2ColorIndex;
    damageMultiplier.value = damageMultiplierValue;
    
  } catch (error) {
    console.error('Error loading persisted data:', error);
  }
}

// === RESET FUNCTIONALITY ===
resetAllData.addEventListener('click', () => {
  resetModal.classList.remove('hidden');
});

cancelReset.addEventListener('click', () => {
  resetModal.classList.add('hidden');
});

confirmReset.addEventListener('click', () => {
  // Clear all localStorage data
  localStorage.clear();
  
  // Reset all variables to default state
  QA = [];
  currentCategory = '';
  availableCategories = [];
  player1Score = 0;
  player2Score = 0;
  player1NameText = 'Player 1';
  player2NameText = 'Player 2';
  currentQuestionIndex = 0;
  savedOrder = null;
  order = [];
  idx = 0;
  roundComplete = false;
  
  // Reset music quiz state
  currentQuizType = 'regular';
  stopMusic();
  
  // Reset lightboard settings to defaults
  lightboardGameMode = 1;
  lightboardP1ColorIndex = 0;
  lightboardP2ColorIndex = 1;
  damageMultiplierValue = 3;
  
  // Update UI
  player1Name.textContent = player1NameText;
  player2Name.textContent = player2NameText;
  player1ScoreEl.textContent = '0';
  player2ScoreEl.textContent = '0';
  
  // Update lightboard settings UI
  lightboardMode.value = lightboardGameMode;
  lightboardP1Color.value = lightboardP1ColorIndex;
  lightboardP2Color.value = lightboardP2ColorIndex;
  damageMultiplier.value = damageMultiplierValue;
  
  // Reset file input
  fileInput.value = '';
  fileList.innerHTML = '';
  loadedFiles.classList.add('hidden');
  
  // Reload all quiz files
  loadAllQuizzes();
  
  // Show file input section
  showCategorySelector();
  
  // Close modal
  resetModal.classList.add('hidden');
  
  // Send reset command to ESP32 (this will reset the lightboard)
  sendToESP32({ action: 'reset' });
  
  console.log('All data reset including player names and lightboard settings');
});

// === LIGHTBOARD SETTINGS ===
lightboardSettingsBtn.addEventListener('click', () => {
  // Load current settings
  lightboardMode.value = lightboardGameMode;
  lightboardP1Color.value = lightboardP1ColorIndex;
  lightboardP2Color.value = lightboardP2ColorIndex;
  damageMultiplier.value = damageMultiplierValue;
  
  lightboardModal.classList.remove('hidden');
});

cancelLightboard.addEventListener('click', () => {
  lightboardModal.classList.add('hidden');
});

confirmLightboard.addEventListener('click', () => {
  const newMode = parseInt(lightboardMode.value);
  const newP1Color = parseInt(lightboardP1Color.value);
  const newP2Color = parseInt(lightboardP2Color.value);
  const newMultiplier = parseInt(damageMultiplier.value);
  
  // Update local variables
  lightboardGameMode = newMode;
  lightboardP1ColorIndex = newP1Color;
  lightboardP2ColorIndex = newP2Color;
  damageMultiplierValue = newMultiplier;
  
  // Send settings to ESP32
  sendToESP32({
    action: 'lightboardSettings',
    mode: newMode,
    p1Color: newP1Color,
    p2Color: newP2Color
  });
  
  lightboardModal.classList.add('hidden');
  
  // Save to localStorage
  savePersistedData();
  
  console.log('Lightboard settings applied:', { newMode, newP1Color, newP2Color, newMultiplier });
});

// === SCORE MANAGEMENT ===
function awardPoint(player) {
  if (!roundComplete) return;
  
  // Award single point to player (damage multiplier only affects lightboard)
  if (player === 'Player 1') {
    player1Score += 1;
    player1ScoreEl.textContent = player1Score;
    // Send message to ESP32 to award points to Player 1
    sendToESP32({ action: 'awardPoint', player: 1, multiplier: damageMultiplierValue });
  } else if (player === 'Player 2') {
    player2Score += 1;
    player2ScoreEl.textContent = player2Score;
    // Send message to ESP32 to award points to Player 2
    sendToESP32({ action: 'awardPoint', player: 2, multiplier: damageMultiplierValue });
  }
  
  // Reset game and advance to next question
  removeScorableState();
  hideWinner();
  aEl.classList.remove('show');
  btnToggle.textContent = 'Show Answer';
  next();
}

// Handle player hits from ESP32
function handlePlayerHit(playerNumber) {
  const playerName = playerNumber === 1 ? 'Player 1' : 'Player 2';
  console.log(`ESP32 detected hit from ${playerName}`);
  
  // Check if quiz is actually in play
  if (!currentCategory || QA.length === 0 || quizDisplay.classList.contains('hidden')) {
    console.log(`Hit from ${playerName} ignored - quiz not in play`);
    return;
  }
  
  // Check if a round is already complete (winner already declared)
  if (roundComplete) {
    console.log(`Hit from ${playerName} ignored - round already complete`);
    return;
  }
  
  // Show winner immediately (ESP32 has already determined winner)
  showWinner(playerName);
  
  // Note: Points are only awarded when clicking the player tile, not on hit detection
  
  // Note: ESP32 handles reset internally when hit is detected, no need to send additional reset
}

// Add scorable state to player tiles
function addScorableState() {
  // Only add scorable class if the tile is not already a winner
  if (!player1Tile.classList.contains('winner')) {
    player1Tile.classList.add('scorable');
  }
  if (!player2Tile.classList.contains('winner')) {
    player2Tile.classList.add('scorable');
  }
  roundComplete = true;
}

// Remove scorable state from player tiles
function removeScorableState() {
  player1Tile.classList.remove('scorable');
  player2Tile.classList.remove('scorable');
  roundComplete = false;
}

function showWinner(player) {
  // Remove winner class from all tiles
  player1Tile.classList.remove('winner');
  player2Tile.classList.remove('winner');
  
  // Add winner class to the winning player's tile
  if (player === 'Player 1') {
    player1Tile.classList.add('winner');
  } else if (player === 'Player 2') {
    player2Tile.classList.add('winner');
  }
  
  // Automatically reveal answer when winner tile is shown
  aEl.classList.add('show');
  btnToggle.textContent = 'Hide Answer';
  
  // Stop music when answer is revealed (player hit)
  stopMusic();
  
  // Enable scoring immediately
  addScorableState();
}

function hideWinner() {
  player1Tile.classList.remove('winner');
  player2Tile.classList.remove('winner');
  removeScorableState();
  
  // Reset hit deduplication tracking for new round
  lastProcessedHitTime = 0;
  lastProcessedHitPlayer = null;
  
  // Hide answer when winner tile is hidden
  aEl.classList.remove('show');
  btnToggle.textContent = 'Show Answer';
}

// === EVENT LISTENERS ===
btnNext.addEventListener('click', () => {
  next();
});

btnPrev.addEventListener('click', () => {
  prev();
});

btnToggle.addEventListener('click', toggleAnswer);
btnExit.addEventListener('click', showExitConfirmation);
card.addEventListener('click', toggleAnswer);

// Music quiz event listener
playMusicBtn.addEventListener('click', (e) => {
  e.preventDefault();
  e.stopPropagation();
  if (isMusicPlaying) {
    stopMusic();
  } else {
    playMusic();
  }
});

// Consolidated scoring event listener
function handlePlayerClick(player) {
  return function(e) {
    if (roundComplete && !isEditingName) {
      e.preventDefault();
      e.stopPropagation();
      awardPoint(player);
    }
  };
}

player1Tile.addEventListener('click', handlePlayerClick('Player 1'));
player2Tile.addEventListener('click', handlePlayerClick('Player 2'));

// Player name editing functionality
function setupPlayerNameEditing() {
  // Use double-tap for mobile editing (more reliable than long press)
  let lastTap = 0;
  let tapTimer;
  
  // Player 1 name editing
  player1Name.addEventListener('touchend', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    
    const currentTime = new Date().getTime();
    const tapLength = currentTime - lastTap;
    
    if (tapLength < 500 && tapLength > 0) {
      // Double tap detected
      e.preventDefault();
      // Clear any pending timer
      if (tapTimer) {
        clearTimeout(tapTimer);
        tapTimer = null;
      }
      startEditingName(player1Name, 'Player 1');
    } else {
      // Single tap - wait for potential double tap
      if (tapTimer) {
        clearTimeout(tapTimer);
      }
      tapTimer = setTimeout(() => {
        // Single tap confirmed
        tapTimer = null;
      }, 500);
    }
    lastTap = currentTime;
  });

  // Player 2 name editing
  let lastTap2 = 0;
  let tapTimer2;
  
  player2Name.addEventListener('touchend', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    
    const currentTime = new Date().getTime();
    const tapLength = currentTime - lastTap2;
    
    if (tapLength < 500 && tapLength > 0) {
      // Double tap detected
      e.preventDefault();
      // Clear any pending timer
      if (tapTimer2) {
        clearTimeout(tapTimer2);
        tapTimer2 = null;
      }
      startEditingName(player2Name, 'Player 2');
    } else {
      // Single tap - wait for potential double tap
      if (tapTimer2) {
        clearTimeout(tapTimer2);
      }
      tapTimer2 = setTimeout(() => {
        // Single tap confirmed
        tapTimer2 = null;
      }, 500);
    }
    lastTap2 = currentTime;
  });

  // Desktop double-click for editing
  player1Name.addEventListener('dblclick', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    e.preventDefault();
    startEditingName(player1Name, 'Player 1');
  });

  player2Name.addEventListener('dblclick', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    e.preventDefault();
    startEditingName(player2Name, 'Player 2');
  });
}

function startEditingName(nameElement, defaultName) {
  if (isEditingName) return;
  
  isEditingName = true;
  
  const parentTile = nameElement.closest('.player-tile');
  const currentName = nameElement.textContent;
  
  // Lock the parent tile width BEFORE making any changes to prevent expansion
  if (parentTile) {
    const tileWidth = parentTile.offsetWidth;
    parentTile.style.width = tileWidth + 'px';
    parentTile.style.minWidth = tileWidth + 'px';
    parentTile.style.maxWidth = tileWidth + 'px';
    parentTile.style.flexShrink = '0';
    parentTile.style.flexGrow = '0';
  }
  
  nameElement.classList.add('editing');
  
  const input = document.createElement('input');
  input.type = 'text';
  input.value = currentName;
  input.style.cssText = `
    background: transparent;
    border: none;
    color: var(--ink);
    font-weight: 600;
    font-size: 16px;
    text-align: center;
    width: 100%;
    max-width: 100%;
    outline: none;
    font-family: inherit;
    -webkit-user-select: text;
    user-select: text;
    margin: 0;
    padding: 0;
    box-sizing: border-box;
    overflow: hidden;
    text-overflow: ellipsis;
  `;
  
  // Clear any existing content and add the input
  nameElement.innerHTML = '';
  nameElement.appendChild(input);
  
  // Force focus and selection on mobile
  const focusTimer = setTimeout(() => {
    input.focus();
    input.select();
    // Force keyboard to appear on mobile
    input.click();
  }, 100);
  
  function finishEditing() {
    // Clear the focus timer
    clearTimeout(focusTimer);
    
    const newName = input.value.trim() || defaultName;
    nameElement.textContent = newName;
    nameElement.classList.remove('editing');
    isEditingName = false;
    
    // Restore the parent tile's flexible sizing
    if (parentTile) {
      parentTile.style.removeProperty('width');
      parentTile.style.removeProperty('min-width');
      parentTile.style.removeProperty('max-width');
      parentTile.style.removeProperty('flex-shrink');
      parentTile.style.removeProperty('flex-grow');
    }
    
    // Update stored names
    if (nameElement === player1Name) {
      player1NameText = newName;
    } else if (nameElement === player2Name) {
      player2NameText = newName;
    }
    
    // Save to localStorage
    savePersistedData();
  }
  
  input.addEventListener('blur', finishEditing);
  input.addEventListener('keydown', function(e) {
    if (e.key === 'Enter') {
      finishEditing();
    } else if (e.key === 'Escape') {
      // Restore original name without saving
      nameElement.textContent = currentName;
      nameElement.classList.remove('editing');
      isEditingName = false;
      
      // Restore the parent tile's flexible sizing
      if (parentTile) {
        parentTile.style.removeProperty('width');
        parentTile.style.removeProperty('min-width');
        parentTile.style.removeProperty('max-width');
        parentTile.style.removeProperty('flex-shrink');
        parentTile.style.removeProperty('flex-grow');
      }
    }
  });
  
  // Handle mobile keyboard "Done" button
  input.addEventListener('input', function(e) {
    // This ensures the input is properly handled on mobile
  });
}

// Keyboard shortcuts
window.addEventListener('keydown', (e) => {
  // Don't process shortcuts if user is editing a name
  if (isEditingName) {
    return;
  }
  
  // Don't process shortcuts if user is typing in an input field
  const activeElement = document.activeElement;
  const isInputField = activeElement && (
    activeElement.tagName === 'INPUT' ||
    activeElement.tagName === 'TEXTAREA' ||
    activeElement.isContentEditable
  );
  
  if (isInputField) {
    return; // Let the input field handle the key
  }
  
  if (e.key === 'ArrowRight') { 
    e.preventDefault(); 
    next(); 
  }
  else if (e.key === 'ArrowLeft') { 
    e.preventDefault(); 
    prev(); 
  }
  else if (e.key === ' ' || e.code === 'Space') { 
    e.preventDefault(); 
    toggleAnswer(); 
  }
  else if (e.key === 'Escape') { 
    e.preventDefault(); 
    if (!categorySelection.classList.contains('hidden')) {
      showCategorySelector();
    } else if (!quizDisplay.classList.contains('hidden')) {
      showCategorySelector();
    }
  }
});

// Exit functionality
function showExitConfirmation() {
  confirmModal.classList.remove('hidden');
}

function hideExitConfirmation() {
  confirmModal.classList.add('hidden');
}

function exitToCategories(keepNames = false, keepScores = false, keepLightboardSettings = true) {
  // Reset game state
  hideWinner();
  aEl.classList.remove('show');
  btnToggle.textContent = 'Show Answer';
  
  // Reset quiz progress
  removeScorableState();
  currentQuestionIndex = 0;
  savedOrder = null;
  order = [];
  idx = 0;
  roundComplete = false;
  
  // Reset music quiz state
  currentQuizType = 'regular';
  stopMusic();
  
  // Handle different exit options
  if (keepNames) {
    // Keep current player names - don't reset them
    console.log('Keeping player names:', player1NameText, player2NameText);
  } else {
    // Reset player names to defaults
    player1NameText = 'Player 1';
    player2NameText = 'Player 2';
    player1Name.textContent = player1NameText;
    player2Name.textContent = player2NameText;
  }
  
  if (keepScores) {
    // Keep current scores - don't reset them
    console.log('Keeping scores');
  } else {
    // Reset scores
    player1Score = 0;
    player2Score = 0;
    player1ScoreEl.textContent = '0';
    player2ScoreEl.textContent = '0';
    
    // Send reset command to ESP32 to reset lightboard scores
    sendToESP32({ action: 'reset' });
  }
  
  if (keepLightboardSettings) {
    // Keep current lightboard settings - don't reset them
    console.log('Keeping lightboard settings');
  } else {
    // Reset lightboard settings to defaults
    lightboardGameMode = 1;
    lightboardP1ColorIndex = 0;
    lightboardP2ColorIndex = 1;
    damageMultiplierValue = 3;
    
    // Update lightboard settings UI
    lightboardMode.value = lightboardGameMode;
    lightboardP1Color.value = lightboardP1ColorIndex;
    lightboardP2Color.value = lightboardP2ColorIndex;
    damageMultiplier.value = damageMultiplierValue;
  }
  
  // Hide modal
  hideExitConfirmation();
  
  // Save current state to localStorage
  savePersistedData();
  
  // Return to category selector
  showCategorySelector();
}

// Modal event listeners
document.getElementById('cancelExit').addEventListener('click', hideExitConfirmation);
document.getElementById('confirmExitKeepNames').addEventListener('click', () => exitToCategories(true, false, true));
document.getElementById('confirmExitKeepScores').addEventListener('click', () => exitToCategories(true, true, true));

// Custom mix builder event listeners
startCustomQuiz.addEventListener('click', loadCustomQuiz);
selectAllCheckbox.addEventListener('change', () => {
  const allCheckboxes = categoryItems.querySelectorAll('.category-checkbox');
  allCheckboxes.forEach(checkbox => {
    checkbox.checked = selectAllCheckbox.checked;
    checkbox.closest('.category-item').classList.toggle('selected', selectAllCheckbox.checked);
  });
  updateTotalQuestions();
});
backToSimpleCategories.addEventListener('click', showCategorySelector);

// Close modals when clicking overlay
confirmModal.addEventListener('click', function(e) {
  if (e.target === confirmModal) {
    hideExitConfirmation();
  }
});

lightboardModal.addEventListener('click', function(e) {
  if (e.target === lightboardModal) {
    lightboardModal.classList.add('hidden');
  }
});

// Quiz Editor state
let quizEditorQuestions = [];
let currentEditingQuiz = null;
let quizEditorMode = 'new'; // 'new' or 'edit'
let quizEditorInitialState = null; // Store initial state to detect changes
let pendingQuizEditorAction = null; // Store pending action after confirmation

// Quiz Editor Functions
function openQuizEditor() {
  categorySelector.classList.add('hidden');
  categorySelection.classList.add('hidden');
  quizDisplay.classList.add('hidden');
  quizEditorDisplay.classList.remove('hidden');
  fileInputSection.classList.add('hidden');
  resetAllData.parentElement.classList.add('hidden');
  lightboardSettingsBtn.parentElement.classList.add('hidden');
  const quizEditorSection = document.getElementById('quizEditorSection');
  if (quizEditorSection) quizEditorSection.classList.add('hidden');
  resetQuizEditor();
  loadQuizList();
  // Store initial state to detect changes
  saveInitialState();
  updateQuizEditorVisibility();
}

function saveInitialState() {
  quizEditorInitialState = JSON.stringify({
    questions: quizEditorQuestions,
    quizName: quizName ? quizName.value : '',
    mode: quizEditorMode,
    selectedQuiz: existingQuizSelect ? existingQuizSelect.value : ''
  });
}

function hasUnsavedChanges() {
  if (!quizEditorInitialState) return false;
  
  const currentState = JSON.stringify({
    questions: quizEditorQuestions,
    quizName: quizName ? quizName.value : '',
    mode: quizEditorMode,
    selectedQuiz: existingQuizSelect ? existingQuizSelect.value : ''
  });
  
  return currentState !== quizEditorInitialState;
}

function showQuizEditorExitConfirmation(actionCallback) {
  if (hasUnsavedChanges()) {
    pendingQuizEditorAction = actionCallback;
    quizEditorExitModal.classList.remove('hidden');
  } else {
    // No unsaved changes, execute action immediately
    if (actionCallback) {
      actionCallback();
    } else {
      closeQuizEditor();
    }
  }
}

function hideQuizEditorExitConfirmation() {
  quizEditorExitModal.classList.add('hidden');
  // Don't clear pendingQuizEditorAction here - it will be cleared after action executes
}

function executePendingQuizEditorAction() {
  if (pendingQuizEditorAction) {
    const action = pendingQuizEditorAction;
    pendingQuizEditorAction = null; // Clear before executing to prevent re-execution
    action();
  }
}

function closeQuizEditor() {
  quizEditorDisplay.classList.add('hidden');
  resetQuizEditor();
  showCategorySelector();
}

function resetQuizEditor() {
  quizEditorQuestions = [];
  currentEditingQuiz = null;
  quizEditorMode = 'new';
  if (quizName) quizName.value = '';
  if (existingQuizSelect) existingQuizSelect.value = '';
  if (questionsList) questionsList.innerHTML = '';
  if (newQuizMode) newQuizMode.classList.add('active');
  if (editQuizMode) editQuizMode.classList.remove('active');
  if (newQuizSection) newQuizSection.classList.remove('hidden');
  if (editQuizSection) editQuizSection.classList.add('hidden');
  quizEditorInitialState = null;
  updateQuizEditorVisibility();
}

function updateQuizEditorVisibility() {
  let shouldShow = false;
  
  if (quizEditorMode === 'new') {
    // Show if quiz name has been entered
    shouldShow = quizName && quizName.value.trim().length > 0;
  } else if (quizEditorMode === 'edit') {
    // Show if a quiz has been selected
    shouldShow = existingQuizSelect && existingQuizSelect.value && existingQuizSelect.value.trim().length > 0;
  }
  
  if (quizQuestionsSection) {
    if (shouldShow) {
      quizQuestionsSection.classList.remove('hidden');
    } else {
      quizQuestionsSection.classList.add('hidden');
    }
  }
  
  if (quizEditorSaveSection) {
    if (shouldShow) {
      quizEditorSaveSection.classList.remove('hidden');
    } else {
      quizEditorSaveSection.classList.add('hidden');
    }
  }
}

function switchQuizEditorMode(mode) {
  // Use confirmation modal if there are unsaved changes
  showQuizEditorExitConfirmation(() => {
    performModeSwitch(mode);
  });
}

function performModeSwitch(mode) {
  quizEditorMode = mode;
  if (mode === 'new') {
    newQuizMode.classList.add('active');
    editQuizMode.classList.remove('active');
    newQuizSection.classList.remove('hidden');
    editQuizSection.classList.add('hidden');
    currentEditingQuiz = null;
    quizEditorQuestions = [];
    if (questionsList) questionsList.innerHTML = '';
    if (existingQuizSelect) existingQuizSelect.value = '';
  } else {
    newQuizMode.classList.remove('active');
    editQuizMode.classList.add('active');
    newQuizSection.classList.add('hidden');
    editQuizSection.classList.remove('hidden');
    if (quizName) quizName.value = '';
    loadQuizList();
  }
  // Save state after mode switch
  saveInitialState();
  updateQuizEditorVisibility();
}

async function loadQuizList() {
  try {
    const quizItems = await getQuizFiles();
    existingQuizSelect.innerHTML = '<option value="">-- Select a quiz --</option>';
    quizItems.forEach(item => {
      const option = document.createElement('option');
      option.value = item.path;
      option.textContent = item.name;
      existingQuizSelect.appendChild(option);
    });
  } catch (error) {
    console.error('Error loading quiz list:', error);
  }
}

async function loadQuizForEditing(quizPath) {
  try {
    const response = await fetch(quizPath);
    if (!response.ok) {
      alert('Failed to load quiz file');
      return;
    }
    
    const csvText = await response.text();
    const csvData = parseCSV(csvText);
    
    quizEditorQuestions = [];
    csvData.forEach((row, index) => {
      const question = row.Question || row.question || row.Q || row.q || Object.values(row)[0];
      const answer = row.Answer || row.answer || row.A || row.a || Object.values(row)[1];
      const audioColumn = row.Audio || row.audio || row.AudioFile || row.audioFile || row.Music || row.music || row.File || row.file || '';
      const badgeColumn = row.Badge || row.badge || row.Icon || row.icon || row.Category || row.category || '';
      
      quizEditorQuestions.push({
        question: question || '',
        answer: answer || '',
        audio: audioColumn || '',
        badge: badgeColumn || '',
        audioFile: null,
        imageFile: null
      });
    });
    
    renderQuestions();
    currentEditingQuiz = quizPath;
    // Save state after loading
    saveInitialState();
    updateQuizEditorVisibility();
  } catch (error) {
    console.error('Error loading quiz:', error);
    alert('Failed to load quiz file');
  }
}

function addQuestion() {
  quizEditorQuestions.push({
    question: '',
    answer: '',
    audio: '',
    badge: '',
    audioFile: null,
    imageFile: null
  });
  renderQuestions();
  // Changes made - will be detected on exit
}

function removeQuestion(index) {
  quizEditorQuestions.splice(index, 1);
  renderQuestions();
  // Changes made - will be detected on exit
}

function updateQuestion(index, field, value) {
  if (quizEditorQuestions[index]) {
    quizEditorQuestions[index][field] = value;
    // Mark that changes have been made (state will be checked on exit)
  }
}

function handleFileUpload(index, type, file) {
  if (!quizEditorQuestions[index]) return;
  
  if (type === 'audio') {
    quizEditorQuestions[index].audioFile = file;
    quizEditorQuestions[index].audio = file.name;
  } else if (type === 'image') {
    quizEditorQuestions[index].imageFile = file;
    quizEditorQuestions[index].badge = file.name;
  }
  
  renderQuestions();
  // Changes made - will be detected on exit
}

function renderQuestions() {
  questionsList.innerHTML = '';
  
  quizEditorQuestions.forEach((q, index) => {
    const questionItem = document.createElement('div');
    questionItem.className = 'question-item';
    
    const audioPreview = q.audioFile ? 
      `<div class="file-upload-preview">
        <div>Audio: ${q.audio}</div>
        <audio controls src="${URL.createObjectURL(q.audioFile)}"></audio>
      </div>` : '';
    
    const imagePreview = q.imageFile ?
      `<div class="file-upload-preview">
        <div>Image: ${q.badge}</div>
        <img src="${URL.createObjectURL(q.imageFile)}" alt="Preview">
      </div>` : '';
    
    questionItem.innerHTML = `
      <div class="question-item-header">
        <span class="question-item-number">Question ${index + 1}</span>
        <button class="delete-question-btn" data-index="${index}">Delete</button>
      </div>
      <div class="question-fields">
        <div class="question-field full-width">
          <label>Question</label>
          <input type="text" class="question-input" data-index="${index}" data-field="question" value="${escapeHtml(q.question)}" placeholder="Enter question">
        </div>
        <div class="question-field full-width">
          <label>Answer</label>
          <input type="text" class="answer-input" data-index="${index}" data-field="answer" value="${escapeHtml(q.answer)}" placeholder="Enter answer">
        </div>
        <div class="question-field">
          <label>Audio File</label>
          <input type="file" class="audio-input" data-index="${index}" accept="audio/*">
          ${q.audio && !q.audioFile ? `<div class="file-upload-preview">Current: ${q.audio}</div>` : ''}
          ${audioPreview}
        </div>
        <div class="question-field">
          <label>Image/Badge</label>
          <input type="file" class="image-input" data-index="${index}" accept="image/*">
          ${q.badge && !q.imageFile ? `<div class="file-upload-preview">Current: ${q.badge}</div>` : ''}
          ${imagePreview}
        </div>
      </div>
    `;
    
    questionsList.appendChild(questionItem);
    
    // Add event listeners
    const deleteBtn = questionItem.querySelector('.delete-question-btn');
    deleteBtn.addEventListener('click', () => removeQuestion(index));
    
    const questionInput = questionItem.querySelector('.question-input');
    questionInput.addEventListener('input', (e) => updateQuestion(index, 'question', e.target.value));
    
    const answerInput = questionItem.querySelector('.answer-input');
    answerInput.addEventListener('input', (e) => updateQuestion(index, 'answer', e.target.value));
    
    const audioInput = questionItem.querySelector('.audio-input');
    audioInput.addEventListener('change', (e) => {
      if (e.target.files.length > 0) {
        handleFileUpload(index, 'audio', e.target.files[0]);
      }
    });
    
    const imageInput = questionItem.querySelector('.image-input');
    imageInput.addEventListener('change', (e) => {
      if (e.target.files.length > 0) {
        handleFileUpload(index, 'image', e.target.files[0]);
      }
    });
  });
}

function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

async function saveQuiz() {
  const name = quizEditorMode === 'new' ? quizName.value.trim() : existingQuizSelect.options[existingQuizSelect.selectedIndex]?.textContent.replace('.csv', '');
  
  if (!name) {
    alert('Please enter a quiz name');
    return;
  }
  
  if (quizEditorQuestions.length === 0) {
    alert('Please add at least one question');
    return;
  }
  
  // Validate all questions have question and answer
  for (let i = 0; i < quizEditorQuestions.length; i++) {
    if (!quizEditorQuestions[i].question.trim() || !quizEditorQuestions[i].answer.trim()) {
      alert(`Question ${i + 1} is missing question or answer`);
      return;
    }
  }
  
  try {
    // Create FormData for file uploads
    const formData = new FormData();
    formData.append('quizName', name);
    formData.append('mode', quizEditorMode);
    if (currentEditingQuiz) {
      formData.append('existingPath', currentEditingQuiz);
    }
    
    // Prepare CSV data
    const csvRows = [];
    csvRows.push(['Question', 'Answer', 'Audio', 'Badge']);
    
    quizEditorQuestions.forEach((q, index) => {
      // Use the uploaded file name if available, otherwise use the existing value
      let audioValue = q.audio || '';
      let badgeValue = q.badge || '';
      
      // If a new file was uploaded, use its name
      if (q.audioFile) {
        audioValue = q.audioFile.name;
      }
      if (q.imageFile) {
        // Images are saved to Images folder, so prepend the path
        badgeValue = `Images/${q.imageFile.name}`;
      } else if (badgeValue && !badgeValue.startsWith('Images/') && !badgeValue.startsWith('/') && !badgeValue.includes('/')) {
        // If badge value exists but doesn't have a path, check if it's an image file
        const imageExtensions = ['.png', '.svg', '.webp', '.jpg', '.jpeg', '.gif'];
        const isImageFile = imageExtensions.some(ext => badgeValue.toLowerCase().endsWith(ext));
        if (isImageFile) {
          // It's an image filename without path, prepend Images/
          badgeValue = `Images/${badgeValue}`;
        }
      }
      
      csvRows.push([
        q.question,
        q.answer,
        audioValue,
        badgeValue
      ]);
    });
    
    // Convert to CSV string
    const csvContent = csvRows.map(row => 
      row.map(cell => {
        // Escape quotes and wrap in quotes if contains comma, quote, or newline
        const cellStr = String(cell || '');
        if (cellStr.includes(',') || cellStr.includes('"') || cellStr.includes('\n')) {
          return '"' + cellStr.replace(/"/g, '""') + '"';
        }
        return cellStr;
      }).join(',')
    ).join('\n');
    
    // Create a File object for the CSV
    const csvBlob = new Blob([csvContent], { type: 'text/csv' });
    const csvFile = new File([csvBlob], `${name}.csv`, { type: 'text/csv' });
    formData.append('csvContent', csvFile);
    
    // Add audio and image files
    quizEditorQuestions.forEach((q, index) => {
      if (q.audioFile) {
        formData.append(`audio_${index}`, q.audioFile);
        formData.append(`audio_name_${index}`, q.audioFile.name);
      }
      if (q.imageFile) {
        formData.append(`image_${index}`, q.imageFile);
        formData.append(`image_name_${index}`, q.imageFile.name);
      }
    });
    
    // Send to server
    const response = await fetch('/api/save-quiz', {
      method: 'POST',
      body: formData
    });
    
    if (!response.ok) {
      const error = await response.text();
      throw new Error(error || 'Failed to save quiz');
    }
    

    // Reset initial state since we just saved
    saveInitialState();
    
    // Don't close the editor here - let the save button handler decide
    // If there's a pending action (like switching mode or changing quiz), it will be executed
    // If not, the save button handler will close the editor
    
    // Update quiz list in editor if we're in edit mode
    if (quizEditorDisplay && !quizEditorDisplay.classList.contains('hidden')) {
      loadQuizList();
    } else {
      // Only reload all quizzes if we're not in the editor
      loadAllQuizzes();
    }
  } catch (error) {
    console.error('Error saving quiz:', error);
    alert('Failed to save quiz: ' + error.message);
  }
}

// Quiz Editor Event Listeners - will be set up in DOMContentLoaded

// Close modals with Escape key
document.addEventListener('keydown', function(e) {
  if (e.key === 'Escape') {
    if (!confirmModal.classList.contains('hidden')) {
      hideExitConfirmation();
    } else if (!lightboardModal.classList.contains('hidden')) {
      lightboardModal.classList.add('hidden');
    } else if (quizEditorDisplay && !quizEditorDisplay.classList.contains('hidden')) {
      showQuizEditorExitConfirmation();
    }
  }
});

// Function to dynamically get list of quiz files from Quizes folder
async function getQuizFiles() {
  try {
    // Get directory listing from the server
    const response = await fetch('/api/quiz-files');
    if (response.ok) {
      const quizItems = await response.json();
      console.log(`Found ${quizItems.length} quiz items:`, quizItems);
      return quizItems;
    } else {
      console.warn('Failed to get quiz files from server:', response.status);
      return [];
    }
  } catch (error) {
    console.error('Error getting quiz files:', error);
    // Return empty array if detection fails
    return [];
  }
}

// Preload all quiz files from Quizes folder
async function loadAllQuizzes() {
  try {
    // Dynamically get list of quiz items from the Quizes folder
    const quizItems = await getQuizFiles();
    
    availableCategories = [];
    fileList.innerHTML = '';
    
    let loadedCount = 0;
    const totalItems = quizItems.length;
    
    for (const quizItem of quizItems) {
      try {
        const response = await fetch(quizItem.path);
        if (!response.ok) {
          console.warn(`Failed to load ${quizItem.path}: ${response.status}`);
          addFileToList(quizItem.name, 'Not found', 'error');
          loadedCount++;
          continue;
        }
        
        const csvText = await response.text();
        const csvData = parseCSV(csvText);
        
        // Convert CSV data to the expected format
        const questions = csvData.map(row => {
          const question = row.Question || row.question || row.Q || row.q || Object.values(row)[0];
          const answer = row.Answer || row.answer || row.A || row.a || Object.values(row)[1];
          
          const questionObj = { q: question, a: answer };
          
          // Get column values - handle CSVs with 2, 3, or 4 columns
          // IMPORTANT: Object.values() doesn't guarantee order, so we need to access by header name
          // Column 3 (index 2): always for audio files (if it exists)
          // Support multiple header names: Audio, Music, File, etc.
          const audioColumn = row.Audio || row.audio || row.AudioFile || row.audioFile || row.Music || row.music || row.File || row.file || '';
          
          // Column 4 (index 3): always for badge category/level (if it exists)
          // IMPORTANT: Don't use row.Level as it's set to the wrong column by parseCSV
          // Support multiple header names: Badge, Icon, Category, Level, etc.
          const badgeColumn = row.Badge || row.badge || row.Icon || row.icon || row.Category || row.category || row.Level || row.level || '';
          
          // Set audio file from column 3 (only if it exists and is not empty)
          if (audioColumn && audioColumn.trim() !== '') {
            questionObj.audioFile = audioColumn.trim();
          }
          
          // Set badge/image from column 4 (only if it exists and is not empty)
          if (badgeColumn && badgeColumn.trim() !== '') {
            questionObj.level = badgeColumn.trim();
            // Check if column 4 is a direct image path (has image extension)
            const imageExtensions = ['.png', '.svg', '.webp', '.jpg', '.jpeg', '.gif'];
            const isImagePath = imageExtensions.some(ext => 
              badgeColumn.trim().toLowerCase().endsWith(ext)
            );
            
            if (isImagePath) {
              // Column 4 is a direct image path - use it as-is
              let imagePath = badgeColumn.trim();
              
              // If it doesn't have a path prefix, assume it's in Images folder
              if (!imagePath.startsWith('http') && !imagePath.startsWith('/') && !imagePath.startsWith('Images/') && !imagePath.includes('/')) {
                // Just a filename - prepend Images/
                imagePath = `Images/${imagePath}`;
              } else if (quizItem.path && !imagePath.startsWith('http') && !imagePath.startsWith('/') && !imagePath.startsWith('Images/')) {
                // Has a path but not Images/ - might be relative to quiz folder
                // But images should be in Images folder, so check if it exists there first
                const imagesPath = `Images/${imagePath.split('/').pop()}`;
                // For now, use Images/ path as that's where they're saved
                imagePath = imagesPath;
              }
              questionObj.iconPath = imagePath;
              console.log(`Set iconPath for question: ${imagePath} (from badge: ${badgeColumn.trim()})`);
            } else {
              // Column 4 is a badge name - try standard icon path
              questionObj.iconPath = getIconPath(badgeColumn.trim());
            }
          }
          
          return questionObj;
        }).filter(qa => qa.q && qa.a);
        
        if (questions.length > 0) {
          const categoryName = quizItem.name.replace('.csv', '').replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
          const categoryData = {
            filename: quizItem.name,
            name: categoryName,
            questions: questions,
            type: quizItem.type
          };
          
          // Add audio files for music quizzes
          if (quizItem.type === 'music' && quizItem.audioFiles) {
            categoryData.audioFiles = quizItem.audioFiles;
          }
          
          availableCategories.push(categoryData);
          
          // Add success message to file list
          const typeIndicator = quizItem.type === 'music' ? '🎵 ' : '';
          addFileToList(quizItem.name, `${typeIndicator}${questions.length} questions`, 'success');
        } else {
          addFileToList(quizItem.name, 'No questions', 'error');
        }
      } catch (error) {
        console.error(`Error loading ${quizItem.path}:`, error);
        addFileToList(quizItem.name, 'Error', 'error');
      }
      
      loadedCount++;
    }
    
    // Show category selector when all files are processed
    // BUT only if we're not in the quiz editor
    if (availableCategories.length > 0) {
      // Only show category selector if quiz editor is not visible
      if (!quizEditorDisplay || quizEditorDisplay.classList.contains('hidden')) {
        showCategorySelector();
        createCategoryButtons(availableCategories);
      } else {
        // We're in the quiz editor, just update the quiz list for editing
        loadQuizList();
      }
    } else {
      console.warn('No valid quiz files loaded');
    }
    
    // Only show loaded files section if we're not in the quiz editor
    if (!quizEditorDisplay || quizEditorDisplay.classList.contains('hidden')) {
      loadedFiles.classList.remove('hidden');
    }
    
  } catch (error) {
    console.error('Error loading quiz files:', error);
    // Fallback to demo data if all else fails
    loadDemoData();
  }
}

// Demo data for testing without CSV files (fallback)
function loadDemoData() {
  const demoCategories = [
    {
      filename: 'demo.csv',
      name: 'Demo Quiz',
      questions: sampleQuestions
    }
  ];
  
  availableCategories = demoCategories;
  showCategorySelector();
  createCategoryButtons(availableCategories);
  
  // Add demo file to the list
  addFileToList('demo.csv', '10 questions (demo)', 'success');
  loadedFiles.classList.remove('hidden');
}

// Cleanup function for client-side resources
function cleanupClientResources() {
  // Clear any pending timeouts
  if (window.tapTimer) {
    clearTimeout(window.tapTimer);
    window.tapTimer = null;
  }
  if (window.tapTimer2) {
    clearTimeout(window.tapTimer2);
    window.tapTimer2 = null;
  }
  
  // Disconnect socket if connected
  if (socket && socket.connected) {
    socket.disconnect();
  }
}

// Initialize quiz on load
document.addEventListener('DOMContentLoaded', function() {
  // Initialize quiz editor elements
  quizEditorBtn = document.getElementById('quizEditorBtn');
  quizEditorDisplay = document.getElementById('quizEditorDisplay');
  exitQuizEditorBtn = document.getElementById('exitQuizEditorBtn');
  saveQuizBtn = document.getElementById('saveQuizBtn');
  newQuizMode = document.getElementById('newQuizMode');
  editQuizMode = document.getElementById('editQuizMode');
  newQuizSection = document.getElementById('newQuizSection');
  editQuizSection = document.getElementById('editQuizSection');
  quizName = document.getElementById('quizName');
  existingQuizSelect = document.getElementById('existingQuizSelect');
  questionsList = document.getElementById('questionsList');
  addQuestionBtn = document.getElementById('addQuestionBtn');
  quizQuestionsSection = document.getElementById('quizQuestionsSection');
  quizEditorSaveSection = document.getElementById('quizEditorSaveSection');
  
  // Load persisted data
  loadPersistedData();
  
  // Setup player name editing
  setupPlayerNameEditing();
  
  // Setup quiz editor event listeners
  if (quizEditorBtn) {
    quizEditorBtn.addEventListener('click', openQuizEditor);
  }
  if (saveQuizBtn) {
    saveQuizBtn.addEventListener('click', saveQuiz);
  }
  if (newQuizMode) {
    newQuizMode.addEventListener('click', () => switchQuizEditorMode('new'));
  }
  if (editQuizMode) {
    editQuizMode.addEventListener('click', () => switchQuizEditorMode('edit'));
  }
  if (existingQuizSelect) {
    existingQuizSelect.addEventListener('change', (e) => {
      const selectedValue = e.target.value;
      
      if (selectedValue) {
        // Check for unsaved changes before loading a different quiz
        if (hasUnsavedChanges() && currentEditingQuiz && currentEditingQuiz !== selectedValue) {
          // Store the selected value temporarily
          const tempValue = selectedValue;
          // Revert the select to current quiz
          existingQuizSelect.value = currentEditingQuiz;
          // Show confirmation modal
          showQuizEditorExitConfirmation(() => {
            // After confirmation, load the new quiz
            existingQuizSelect.value = tempValue;
            loadQuizForEditing(tempValue);
          });
        } else {
          // No unsaved changes or same quiz, load immediately
          loadQuizForEditing(selectedValue);
        }
      } else {
        // Deselecting quiz - check for unsaved changes
        if (hasUnsavedChanges() && currentEditingQuiz) {
          // Revert the select to current quiz
          existingQuizSelect.value = currentEditingQuiz;
          // Show confirmation modal
          showQuizEditorExitConfirmation(() => {
            // After confirmation, clear the quiz
            existingQuizSelect.value = '';
            quizEditorQuestions = [];
            if (questionsList) questionsList.innerHTML = '';
            currentEditingQuiz = null;
            updateQuizEditorVisibility();
            saveInitialState();
          });
        } else {
          // No unsaved changes, clear immediately
          quizEditorQuestions = [];
          if (questionsList) questionsList.innerHTML = '';
          currentEditingQuiz = null;
          updateQuizEditorVisibility();
          saveInitialState();
        }
      }
    });
  }
  if (quizName) {
    quizName.addEventListener('input', () => {
      updateQuizEditorVisibility();
    });
  }
  if (addQuestionBtn) {
    addQuestionBtn.addEventListener('click', addQuestion);
  }
  if (exitQuizEditorBtn) {
    exitQuizEditorBtn.addEventListener('click', () => {
      showQuizEditorExitConfirmation(closeQuizEditor);
    });
  }
  
  // Quiz Editor Exit Modal elements
  quizEditorExitModal = document.getElementById('quizEditorExitModal');
  cancelQuizEditorExit = document.getElementById('cancelQuizEditorExit');
  saveAndExitQuizEditor = document.getElementById('saveAndExitQuizEditor');
  exitWithoutSavingQuizEditor = document.getElementById('exitWithoutSavingQuizEditor');
  
  if (cancelQuizEditorExit) {
    cancelQuizEditorExit.addEventListener('click', () => {
      hideQuizEditorExitConfirmation();
      pendingQuizEditorAction = null; // Clear pending action on cancel
    });
  }
  if (saveAndExitQuizEditor) {
    saveAndExitQuizEditor.addEventListener('click', async () => {
      // Store the pending action before hiding modal - this is the action we want to execute
      const actionToExecute = pendingQuizEditorAction;
      hideQuizEditorExitConfirmation();
      // Temporarily clear pending action so saveQuiz doesn't think there's a pending action
      pendingQuizEditorAction = null;
      await saveQuiz();
      // After saving, execute the stored action if it exists
      // If no stored action, close the editor (this handles the "exit" case)
      if (actionToExecute) {
        // Clear any pending action that might have been set during save
        pendingQuizEditorAction = null;
        // Execute the original action
        actionToExecute();
      } else {
        // No stored action means user was exiting, so close the editor
        closeQuizEditor();
      }
    });
  }
  if (exitWithoutSavingQuizEditor) {
    exitWithoutSavingQuizEditor.addEventListener('click', () => {
      hideQuizEditorExitConfirmation();
      // Execute the pending action (which will discard changes)
      executePendingQuizEditorAction();
    });
  }
  if (quizEditorExitModal) {
    quizEditorExitModal.addEventListener('click', function(e) {
      if (e.target === quizEditorExitModal) {
        hideQuizEditorExitConfirmation();
      }
    });
  }
  
  // Load all quiz files immediately
  loadAllQuizzes();
});

// Cleanup on page unload
window.addEventListener('beforeunload', cleanupClientResources);
window.addEventListener('unload', cleanupClientResources);
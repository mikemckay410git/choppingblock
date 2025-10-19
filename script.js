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
  console.log('ESP32 status:', status);
  
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
  console.log('Received from ESP32:', data);
  
  // Handle different types of ESP32 messages
  if (data.type === 'hit') {
    console.log(`Player ${data.player} hit detected! Time: ${data.time}, Strength: ${data.strength}`);
    // Map backend player numbers to frontend player numbers
    const frontendPlayer = data.player === 2 ? 1 : 2; // Backend Player 2->Frontend Player 1, Backend Player 3->Frontend Player 2
    handlePlayerHit(frontendPlayer);
  } else if (data.type === 'winner') {
    console.log(`Winner declared: ${data.winner}`);
    // Map backend winner names to frontend winner names
    const frontendWinner = data.winner === 'Player 2' ? 'Player 1' : 'Player 2';
    // Only process winner if round is not already complete
    if (!roundComplete) {
      showWinner(frontendWinner);
    } else {
      console.log(`Winner message for ${frontendWinner} ignored - round already complete`);
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
        p2Color: command.p1Color, // Map frontend Player 1 color to backend Player 2 color
        p3Color: command.p2Color  // Map frontend Player 2 color to backend Player 3 color
      };
    } else if (command.action === 'awardPoint') {
      esp32Command = { 
        cmd: 'awardPoint', 
        player: command.player === 1 ? 2 : 3, // Map frontend Player 1->2, Player 2->3 for backend
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
  const lines = csv.split('\n');
  const headers = lines[0].split(',').map(h => h.replace(/"/g, ''));
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
    values.push(current.trim());
    
    const row = {};
    headers.forEach((header, index) => {
      row[header] = values[index] ? values[index].replace(/^"|"$/g, '') : '';
    });
    data.push(row);
  }
  
  return data;
}

// === APP STATE ===
let QA = [];
let currentCategory = '';
let availableCategories = [];
let player1Score = 0;
let player2Score = 0;

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
const quizInterface = document.getElementById('quizInterface');
const quizDisplay = document.getElementById('quizDisplay');
const quizTitle = document.getElementById('quizTitle');
const qEl = document.getElementById('q');
const aEl = document.getElementById('a');
const answerText = document.getElementById('answerText');
const categoryBadge = document.getElementById('categoryBadge');
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
            return { q: question, a: answer };
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
  quizDisplay.classList.add('hidden');
  fileInputSection.classList.remove('hidden');
  resetAllData.parentElement.classList.remove('hidden');
  lightboardSettingsBtn.parentElement.classList.remove('hidden');
  currentCategory = '';
  QA = [];
  console.log('Category selector shown, file input visible');
}

function showQuizDisplay() {
  categorySelector.classList.add('hidden');
  quizDisplay.classList.remove('hidden');
  fileInputSection.classList.add('hidden');
  resetAllData.parentElement.classList.add('hidden');
  lightboardSettingsBtn.parentElement.classList.add('hidden');
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

  // Add "Combine All" button if there are multiple categories
  let buttonsHTML = '';
  if (categories.length > 1) {
    const totalQuestions = categories.reduce((sum, cat) => sum + cat.questions.length, 0);
    buttonsHTML += `
      <div class="category-btn combine-all" style="grid-column: 1 / -1; background: linear-gradient(180deg, rgba(155,225,255,.25), rgba(155,225,255,.15)); border-color: var(--accent2);">
        <div style="font-size: 18px; margin-bottom: 4px;">🎯 Combine All Categories</div>
        <div style="font-size: 12px; color: var(--muted);">${totalQuestions} total questions from ${categories.length} categories</div>
      </div>
    `;
  }

  // Add individual category buttons
  buttonsHTML += categories.map(category => {
    const musicEmoji = category.type === 'music' ? '<div class="category-music-icon">🎵</div>' : '';
    return `
      <div class="category-btn" data-filename="${category.filename}">
        ${musicEmoji}
        <div style="font-size: 18px; margin-bottom: 4px;">${category.name}</div>
        <div style="font-size: 12px; color: var(--muted);">${category.questions.length} questions</div>
      </div>
    `;
  }).join('');

  categoryGrid.innerHTML = buttonsHTML;

  // Add click handlers
  categoryGrid.querySelectorAll('.category-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      if (btn.classList.contains('combine-all')) {
        loadCombinedCategories(categories);
      } else {
        const filename = btn.dataset.filename;
        loadCategory(filename);
      }
    });
  });
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
  
  // Show/hide music controls based on quiz type
  if (currentQuizType === 'music') {
    musicControls.classList.remove('hidden');
  } else {
    musicControls.classList.add('hidden');
    stopMusic(); // Stop any playing music when switching to regular quiz
  }
  
  showQuizDisplay();
  setOrder(true);
  render(true);
  enableControls();
}

function loadCombinedCategories(categories) {
  // Combine all questions from all categories
  QA = [];
  const categoryNames = [];
  let hasMusicQuiz = false;
  
  categories.forEach(category => {
    // Add category information to each question
    const questionsWithCategory = category.questions.map(qa => ({
      ...qa,
      category: category.name
    }));
    QA = QA.concat(questionsWithCategory);
    categoryNames.push(category.name);
    
    // Check if any category is a music quiz
    if (category.type === 'music') {
      hasMusicQuiz = true;
    }
  });

  currentCategory = `Mixed: ${categoryNames.join(', ')}`;
  currentQuizType = hasMusicQuiz ? 'music' : 'regular';
  quizTitle.textContent = currentCategory;
  
  // Show/hide music controls based on whether any category is music
  if (currentQuizType === 'music') {
    musicControls.classList.remove('hidden');
  } else {
    musicControls.classList.add('hidden');
    stopMusic(); // Stop any playing music when switching to regular quiz
  }
  
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
  if (currentQuizType !== 'music' || QA.length === 0) return;
  
  const currentQuestion = QA[order[idx]];
  if (!currentQuestion.audioFile) {
    musicStatus.textContent = '';
    return;
  }
  
  // Stop any currently playing music
  stopMusic();
  
  // Find the current category to get the folder path
  const currentCategoryData = availableCategories.find(cat => 
    cat.questions === QA || cat.name === currentCategory
  );
  
  if (!currentCategoryData || currentCategoryData.type !== 'music') {
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
  nameElement.classList.add('editing');
  
  const currentName = nameElement.textContent;
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
    outline: none;
    font-family: inherit;
    -webkit-user-select: text;
    user-select: text;
    margin: 0;
    padding: 0;
    box-sizing: border-box;
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
    showCategorySelector();
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

// Close modals with Escape key
document.addEventListener('keydown', function(e) {
  if (e.key === 'Escape') {
    if (!confirmModal.classList.contains('hidden')) {
      hideExitConfirmation();
    } else if (!lightboardModal.classList.contains('hidden')) {
      lightboardModal.classList.add('hidden');
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
          const audioFile = row.Audio || row.audio || row.AudioFile || row.audioFile || Object.values(row)[2];
          
          const questionObj = { q: question, a: answer };
          
          // Add audio file reference for music quizzes
          if (quizItem.type === 'music' && audioFile) {
            questionObj.audioFile = audioFile;
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
    if (availableCategories.length > 0) {
      showCategorySelector();
      createCategoryButtons(availableCategories);
    } else {
      console.warn('No valid quiz files loaded');
    }
    
    loadedFiles.classList.remove('hidden');
    
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
  // Load persisted data
  loadPersistedData();
  
  // Setup player name editing
  setupPlayerNameEditing();
  
  // Load all quiz files immediately
  loadAllQuizzes();
});

// Cleanup on page unload
window.addEventListener('beforeunload', cleanupClientResources);
window.addEventListener('unload', cleanupClientResources);
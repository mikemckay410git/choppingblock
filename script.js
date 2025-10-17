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
    handlePlayerHit(data.player);
  } else if (data.type === 'winner') {
    console.log(`Winner declared: ${data.winner}`);
    showWinner(data.winner);
    // Also increment local score and award to lightboard
    const p = (typeof data.winner === 'string') ? (data.winner.includes('2') ? 2 : data.winner.includes('3') ? 3 : null) : null;
    const nowMs = Date.now();
    if (p === 2 || p === 3) {
      // Debounce duplicate awards within 250ms
      if (lastAwardPlayer !== p || (nowMs - lastAwardAtMs) > 250) {
        if (p === 2) {
          player2Score += (damageMultiplierValue || 1);
          player2ScoreEl.textContent = player2Score;
        } else {
          player3Score += (damageMultiplierValue || 1);
          player3ScoreEl.textContent = player3Score;
        }
        sendToESP32({ action: 'awardPoint', player: p, multiplier: damageMultiplierValue || 1 });
        lastAwardPlayer = p;
        lastAwardAtMs = nowMs;
      }
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
        p2Color: command.p2Color, 
        p3Color: command.p3Color 
      };
    } else if (command.action === 'awardPoint') {
      esp32Command = { 
        cmd: 'awardPoint', 
        player: command.player, 
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
let player2Score = 0;
let player3Score = 0;

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
const player2ScoreEl = document.getElementById('player2Score');
const player3ScoreEl = document.getElementById('player3Score');

// Reset button
const resetAllData = document.getElementById('resetAllData');
const confirmModal = document.getElementById('confirmModal');
const cancelReset = document.getElementById('cancelReset');
const confirmReset = document.getElementById('confirmReset');

// Lightboard settings
const lightboardSettingsBtn = document.getElementById('lightboardSettingsBtn');
const lightboardModal = document.getElementById('lightboardModal');
const cancelLightboard = document.getElementById('cancelLightboard');
const confirmLightboard = document.getElementById('confirmLightboard');
const lightboardMode = document.getElementById('lightboardMode');
const lightboardP2Color = document.getElementById('lightboardP2Color');
const lightboardP3Color = document.getElementById('lightboardP3Color');
const damageMultiplier = document.getElementById('damageMultiplier');

// Game status elements
const gameStatus = document.getElementById('gameStatus');
const player2Tile = document.getElementById('player2Tile');
const player3Tile = document.getElementById('player3Tile');
const player2Name = document.querySelector('#player2Tile .player-name');
const player3Name = document.querySelector('#player3Tile .player-name');

let order = [];
let idx = 0;

// Lightboard settings
let lightboardGameMode = 1; // Default to Territory mode
let lightboardP2ColorIndex = 0; // Red
let lightboardP3ColorIndex = 1; // Blue
let damageMultiplierValue = 3; // Default to triple damage

// Debounce awarding to avoid duplicate points on multiple hit logs
let lastAwardAtMs = 0;
let lastAwardPlayer = null;

// Player names with persistence
let player2NameText = 'Player 2';
let player3NameText = 'Player 3';
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
  buttonsHTML += categories.map(category => `
    <div class="category-btn" data-filename="${category.filename}">
      <div style="font-size: 18px; margin-bottom: 4px;">${category.name}</div>
      <div style="font-size: 12px; color: var(--muted);">${category.questions.length} questions</div>
    </div>
  `).join('');

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
  quizTitle.textContent = currentCategory;
  
  showQuizDisplay();
  setOrder(true);
  render(true);
  enableControls();
}

function loadCombinedCategories(categories) {
  // Combine all questions from all categories
  QA = [];
  const categoryNames = [];
  
  categories.forEach(category => {
    // Add category information to each question
    const questionsWithCategory = category.questions.map(qa => ({
      ...qa,
      category: category.name
    }));
    QA = QA.concat(questionsWithCategory);
    categoryNames.push(category.name);
  });

  currentCategory = `Mixed: ${categoryNames.join(', ')}`;
  quizTitle.textContent = currentCategory;
  
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
}

function prev() {
  if (idx > 0) { 
    idx--; 
    render(); 
  } else {
    idx = order.length - 1; 
    render(); 
  }
}

function toggleAnswer() {
  aEl.classList.toggle('show');
  btnToggle.textContent = aEl.classList.contains('show') ? 'Hide Answer' : 'Show Answer';
}

// === RESET FUNCTIONALITY ===
resetAllData.addEventListener('click', () => {
  confirmModal.classList.remove('hidden');
});

cancelReset.addEventListener('click', () => {
  confirmModal.classList.add('hidden');
});

confirmReset.addEventListener('click', () => {
  // Reset all data
  QA = [];
  currentCategory = '';
  availableCategories = [];
  player2Score = 0;
  player3Score = 0;
  
  // Update UI
  player2ScoreEl.textContent = '0';
  player3ScoreEl.textContent = '0';
  
  // Reset file input
  fileInput.value = '';
  fileList.innerHTML = '';
  loadedFiles.classList.add('hidden');
  
  // Show file input section
  showCategorySelector();
  
  // Close modal
  confirmModal.classList.add('hidden');
  
  // Send reset command to ESP32
  sendToESP32({ action: 'reset' });
  
  console.log('All data reset');
});

// === LIGHTBOARD SETTINGS ===
lightboardSettingsBtn.addEventListener('click', () => {
  // Load current settings
  lightboardMode.value = lightboardGameMode;
  lightboardP2Color.value = lightboardP2ColorIndex;
  lightboardP3Color.value = lightboardP3ColorIndex;
  damageMultiplier.value = damageMultiplierValue;
  
  lightboardModal.classList.remove('hidden');
});

cancelLightboard.addEventListener('click', () => {
  lightboardModal.classList.add('hidden');
});

confirmLightboard.addEventListener('click', () => {
  const newMode = parseInt(lightboardMode.value);
  const newP2Color = parseInt(lightboardP2Color.value);
  const newP3Color = parseInt(lightboardP3Color.value);
  const newMultiplier = parseInt(damageMultiplier.value);
  
  // Update local variables
  lightboardGameMode = newMode;
  lightboardP2ColorIndex = newP2Color;
  lightboardP3ColorIndex = newP3Color;
  damageMultiplierValue = newMultiplier;
  
  // Send settings to ESP32
  sendToESP32({
    action: 'lightboardSettings',
    mode: newMode,
    p2Color: newP2Color,
    p3Color: newP3Color
  });
  
  lightboardModal.classList.add('hidden');
  
  console.log('Lightboard settings applied:', { newMode, newP2Color, newP3Color, newMultiplier });
});

// === SCORE MANAGEMENT ===
function awardPoint(player) {
  if (!roundComplete) return;
  
  // Update score based on damage multiplier
  if (player === 'Player 2') {
    player2Score += damageMultiplierValue;
    player2ScoreEl.textContent = player2Score;
    // Send message to ESP32 to award points to Player 2
    sendToESP32({ action: 'awardPoint', player: 2, multiplier: damageMultiplierValue });
  } else if (player === 'Player 3') {
    player3Score += damageMultiplierValue;
    player3ScoreEl.textContent = player3Score;
    // Send message to ESP32 to award points to Player 3
    sendToESP32({ action: 'awardPoint', player: 3, multiplier: damageMultiplierValue });
  }
  
  // Reset game and advance to next question
  removeScorableState();
  hideWinner();
  aEl.classList.remove('show');
  btnToggle.textContent = 'Show Answer';
  // Add a small delay before advancing to next question to prevent flashing
  setTimeout(() => {
    next();
  }, 120);
}

// Handle player hits from ESP32
function handlePlayerHit(playerNumber) {
  const playerName = playerNumber === 2 ? 'Player 2' : 'Player 3';
  console.log(`ESP32 detected hit from ${playerName}`);
  
  // Show winner immediately (ESP32 has already determined winner)
  showWinner(playerName);

  // Award point to lightboard immediately on hit (debounced)
  const nowMs = Date.now();
  if (lastAwardPlayer !== playerNumber || (nowMs - lastAwardAtMs) > 250) {
    if (playerNumber === 2) {
      player2Score += (damageMultiplierValue || 1);
      player2ScoreEl.textContent = player2Score;
    } else if (playerNumber === 3) {
      player3Score += (damageMultiplierValue || 1);
      player3ScoreEl.textContent = player3Score;
    }
    sendToESP32({ action: 'awardPoint', player: playerNumber, multiplier: damageMultiplierValue || 1 });
    lastAwardPlayer = playerNumber;
    lastAwardAtMs = nowMs;
  }
}

// Add scorable state to player tiles
function addScorableState() {
  // Only add scorable class if the tile is not already a winner
  if (!player2Tile.classList.contains('winner')) {
    player2Tile.classList.add('scorable');
  }
  if (!player3Tile.classList.contains('winner')) {
    player3Tile.classList.add('scorable');
  }
  roundComplete = true;
}

// Remove scorable state from player tiles
function removeScorableState() {
  player2Tile.classList.remove('scorable');
  player3Tile.classList.remove('scorable');
  roundComplete = false;
}

function showWinner(player) {
  // Remove winner class from all tiles
  player2Tile.classList.remove('winner');
  player3Tile.classList.remove('winner');
  
  // Add winner class to the winning player's tile
  if (player === 'Player 2') {
    player2Tile.classList.add('winner');
  } else if (player === 'Player 3') {
    player3Tile.classList.add('winner');
  }
  
  // Enable scoring immediately
  addScorableState();
}

function hideWinner() {
  player2Tile.classList.remove('winner');
  player3Tile.classList.remove('winner');
  removeScorableState();
}

// === EVENT LISTENERS ===
btnNext.addEventListener('click', () => {
  next();
  sendToESP32({ action: 'reset', quizNav: true });
});

btnPrev.addEventListener('click', () => {
  prev();
  sendToESP32({ action: 'reset', quizNav: true });
});

btnToggle.addEventListener('click', toggleAnswer);
btnExit.addEventListener('click', showExitConfirmation);
card.addEventListener('click', toggleAnswer);

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

player2Tile.addEventListener('click', handlePlayerClick('Player 2'));
player3Tile.addEventListener('click', handlePlayerClick('Player 3'));

// Player name editing functionality
function setupPlayerNameEditing() {
  // Use double-tap for mobile editing (more reliable than long press)
  let lastTap = 0;
  let tapTimer;
  
  // Player 2 name editing
  player2Name.addEventListener('touchend', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    
    const currentTime = new Date().getTime();
    const tapLength = currentTime - lastTap;
    
    if (tapLength < 500 && tapLength > 0) {
      // Double tap detected
      e.preventDefault();
      startEditingName(player2Name, 'Player 2');
    } else {
      // Single tap - wait for potential double tap
      tapTimer = setTimeout(() => {
        // Single tap confirmed
      }, 500);
    }
    lastTap = currentTime;
  });

  // Player 3 name editing
  let lastTap3 = 0;
  let tapTimer3;
  
  player3Name.addEventListener('touchend', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    
    const currentTime = new Date().getTime();
    const tapLength = currentTime - lastTap3;
    
    if (tapLength < 500 && tapLength > 0) {
      // Double tap detected
      e.preventDefault();
      startEditingName(player3Name, 'Player 3');
    } else {
      // Single tap - wait for potential double tap
      tapTimer3 = setTimeout(() => {
        // Single tap confirmed
      }, 500);
    }
    lastTap3 = currentTime;
  });

  // Desktop double-click for editing
  player2Name.addEventListener('dblclick', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    e.preventDefault();
    startEditingName(player2Name, 'Player 2');
  });

  player3Name.addEventListener('dblclick', function(e) {
    if (roundComplete) return; // Don't allow editing during scoring phase
    e.preventDefault();
    startEditingName(player3Name, 'Player 3');
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
  setTimeout(() => {
    input.focus();
    input.select();
    // Force keyboard to appear on mobile
    input.click();
  }, 100);
  
  function finishEditing() {
    const newName = input.value.trim() || defaultName;
    nameElement.textContent = newName;
    nameElement.classList.remove('editing');
    isEditingName = false;
    
    // Update stored names
    if (nameElement === player2Name) {
      player2NameText = newName;
    } else if (nameElement === player3Name) {
      player3NameText = newName;
    }
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
  if (e.key === 'ArrowRight') { 
    e.preventDefault(); 
    next(); 
    sendToESP32({ action: 'reset', quizNav: true });
  }
  else if (e.key === 'ArrowLeft') { 
    e.preventDefault(); 
    prev(); 
    sendToESP32({ action: 'reset', quizNav: true });
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

function exitToCategories() {
  // Reset game state
  hideWinner();
  aEl.classList.remove('show');
  btnToggle.textContent = 'Show Answer';
  
  // Reset scores
  player2Score = 0;
  player3Score = 0;
  roundComplete = false;
  player2ScoreEl.textContent = '0';
  player3ScoreEl.textContent = '0';
  removeScorableState();
  
  // Hide modal
  hideExitConfirmation();
  
  // Reset lightboard when exiting quiz
  sendToESP32({ action: 'reset' });
  
  // Return to category selector
  showCategorySelector();
}

// Modal event listeners
document.getElementById('cancelExit').addEventListener('click', hideExitConfirmation);
document.getElementById('confirmExit').addEventListener('click', exitToCategories);

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

// Demo data for testing without CSV files
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

// Initialize quiz on load
document.addEventListener('DOMContentLoaded', function() {
  // Setup player name editing
  setupPlayerNameEditing();
  
  // Load demo data immediately (like Player1.ino)
  loadDemoData();
});
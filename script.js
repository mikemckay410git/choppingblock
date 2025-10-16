// Socket.IO client for ESP32 communication
const socket = io('http://localhost:3000');

// Connection status indicator
const connDot = document.getElementById('connDot');

// ESP32 connection status
let esp32Connected = false;
let esp32Enabled = false;

// Socket.IO event handlers
socket.on('connect', () => {
  console.log('Connected to server');
  connDot.className = 'good';
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
    connDot.className = 'good';
  } else if (esp32Enabled && !esp32Connected) {
    connDot.className = 'warning';
  } else {
    connDot.className = 'bad';
  }
});

socket.on('esp32_data', (data) => {
  console.log('Received from ESP32:', data);
  // Handle ESP32 data here if needed
});

// Function to send commands to ESP32
function sendToESP32(command) {
  if (esp32Enabled && esp32Connected) {
    socket.emit('esp32_command', command);
    console.log('Sent to ESP32:', command);
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
const quizTitle = document.getElementById('quizTitle');
const qEl = document.getElementById('q');
const aEl = document.getElementById('a');
const answerText = document.getElementById('answerText');
const categoryBadge = document.getElementById('categoryBadge');
const counterEl = document.getElementById('counter');
const btnPrev = document.getElementById('prev');
const btnNext = document.getElementById('next');
const btnToggle = document.getElementById('toggle');
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

let order = [];
let idx = 0;

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
  quizInterface.classList.add('hidden');
  fileInputSection.classList.remove('hidden');
  currentCategory = '';
  QA = [];
  console.log('Category selector shown, file input visible');
}

function showQuizInterface() {
  categorySelector.classList.add('hidden');
  quizInterface.classList.remove('hidden');
  fileInputSection.classList.add('hidden');
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
      <div class="category-btn combine-all" style="grid-column: 1 / -1; background: linear-gradient(180deg, rgba(155,225,255,.25), rgba(155,225,255,.15)); border-color: var(--accent-2);">
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
  
  showQuizInterface();
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
  
  showQuizInterface();
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
  sendToESP32({ cmd: 'reset' });
  
  console.log('All data reset');
});

// === LIGHTBOARD SETTINGS ===
lightboardSettingsBtn.addEventListener('click', () => {
  lightboardModal.classList.remove('hidden');
});

cancelLightboard.addEventListener('click', () => {
  lightboardModal.classList.add('hidden');
});

confirmLightboard.addEventListener('click', () => {
  const settings = {
    cmd: 'lightboardSettings',
    mode: parseInt(lightboardMode.value),
    p2Color: parseInt(lightboardP2Color.value),
    p3Color: parseInt(lightboardP3Color.value),
    multiplier: parseInt(damageMultiplier.value)
  };
  
  sendToESP32(settings);
  lightboardModal.classList.add('hidden');
  
  console.log('Lightboard settings applied:', settings);
});

// === SCORE MANAGEMENT ===
function awardPoint(player, multiplier = 1) {
  if (player === 2) {
    player2Score += multiplier;
    player2ScoreEl.textContent = player2Score;
  } else if (player === 3) {
    player3Score += multiplier;
    player3ScoreEl.textContent = player3Score;
  }
  
  // Send score update to ESP32
  sendToESP32({ 
    cmd: 'awardPoint', 
    player: player, 
    multiplier: multiplier 
  });
}

// === QUIZ ACTIONS ===
function sendQuizAction(action) {
  sendToESP32({ 
    cmd: 'quizAction', 
    action: action 
  });
}

// === EVENT LISTENERS ===
btnNext.addEventListener('click', () => {
  next();
  sendQuizAction('next');
});

btnPrev.addEventListener('click', () => {
  prev();
  sendQuizAction('prev');
});

btnToggle.addEventListener('click', toggleAnswer);
card.addEventListener('click', toggleAnswer);

// Keyboard shortcuts
window.addEventListener('keydown', (e) => {
  if (e.key === 'ArrowRight') { 
    e.preventDefault(); 
    next(); 
    sendQuizAction('next');
  }
  else if (e.key === 'ArrowLeft') { 
    e.preventDefault(); 
    prev(); 
    sendQuizAction('prev');
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

// Demo data for testing without CSV files
function loadDemoData() {
  const demoCategories = [
    {
      filename: 'demo.csv',
      name: 'Demo Quiz',
      questions: [
        { q: 'What is the capital of France?', a: 'Paris' },
        { q: 'What is 2 + 2?', a: '4' },
        { q: 'What color is the sky?', a: 'Blue' },
        { q: 'How many days are in a week?', a: '7' },
        { q: 'What is the largest planet?', a: 'Jupiter' }
      ]
    }
  ];
  
  availableCategories = demoCategories;
  showCategorySelector();
  createCategoryButtons(availableCategories);
  
  // Add demo file to the list
  addFileToList('demo.csv', '5 questions (demo)', 'success');
  loadedFiles.classList.remove('hidden');
}

// Load demo data on page load if no files are loaded
document.addEventListener('DOMContentLoaded', () => {
  // Add a demo button if no files are loaded after a short delay
  setTimeout(() => {
    if (availableCategories.length === 0) {
      const demoBtn = document.createElement('button');
      demoBtn.textContent = '🎮 Load Demo Quiz';
      demoBtn.className = 'demo-btn';
      demoBtn.style.cssText = `
        background: linear-gradient(180deg, rgba(155,225,255,.25), rgba(155,225,255,.15));
        border: 1px solid var(--accent-2);
        border-radius: 12px;
        padding: 16px;
        color: var(--ink);
        font-weight: 600;
        cursor: pointer;
        margin-top: 16px;
        width: 100%;
      `;
      demoBtn.addEventListener('click', loadDemoData);
      
      const fileInputSection = document.getElementById('fileInputSection');
      fileInputSection.appendChild(demoBtn);
    }
  }, 1000);
});

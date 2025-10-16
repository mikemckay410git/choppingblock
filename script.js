// ===================== ESP32 Bridge Communication =====================
class ESP32Bridge {
    constructor() {
        this.connected = false;
        this.socket = null;
        this.reconnectInterval = 5000; // 5 seconds
        this.heartbeatInterval = 1000; // 1 second
        this.lastHeartbeat = 0;
        this.connect();
    }

    connect() {
        try {
            // For development, connect to localhost
            // In production, replace with your Pi's IP address
            this.socket = new WebSocket('ws://localhost:8080');
            
            this.socket.onopen = () => {
                console.log('Connected to ESP32 Bridge');
                this.connected = true;
                this.updateConnectionStatus(true);
                this.startHeartbeat();
            };

            this.socket.onmessage = (event) => {
                this.handleMessage(event.data);
            };

            this.socket.onclose = () => {
                console.log('ESP32 Bridge connection closed');
                this.connected = false;
                this.updateConnectionStatus(false);
                this.scheduleReconnect();
            };

            this.socket.onerror = (error) => {
                console.error('ESP32 Bridge connection error:', error);
                this.connected = false;
                this.updateConnectionStatus(false);
            };
        } catch (error) {
            console.error('Failed to connect to ESP32 Bridge:', error);
            this.scheduleReconnect();
        }
    }

    scheduleReconnect() {
        setTimeout(() => {
            console.log('Attempting to reconnect to ESP32 Bridge...');
            this.connect();
        }, this.reconnectInterval);
    }

    startHeartbeat() {
        setInterval(() => {
            if (this.connected) {
                this.sendCommand('heartbeat');
            }
        }, this.heartbeatInterval);
    }

    sendCommand(command) {
        if (this.connected && this.socket) {
            this.socket.send(JSON.stringify(command));
        } else {
            console.warn('ESP32 Bridge not connected, cannot send command:', command);
        }
    }

    handleMessage(data) {
        try {
            const message = JSON.parse(data);
            console.log('Received from ESP32 Bridge:', message);

            switch (message.type) {
                case 'status':
                    this.handleStatusUpdate(message);
                    break;
                case 'winner':
                    this.handleWinner(message.winner);
                    break;
                case 'hit':
                    this.handleHit(message);
                    break;
                case 'reset':
                    this.handleReset();
                    break;
                case 'error':
                    this.handleError(message.message);
                    break;
                case 'quizAction':
                    this.handleQuizAction(message.action);
                    break;
            }
        } catch (error) {
            console.error('Error parsing ESP32 Bridge message:', error);
        }
    }

    handleStatusUpdate(status) {
        console.log('ESP32 Bridge Status:', status);
        // Update connection indicators if needed
    }

    handleWinner(winner) {
        if (winner && winner !== 'none') {
            showWinner(winner);
            // Automatically reveal answer when someone wins
            aEl.classList.add('show');
            btnToggle.textContent = 'Hide Answer';
        } else {
            hideWinner();
            // Hide answer when game resets
            aEl.classList.remove('show');
            btnToggle.textContent = 'Show Answer';
        }
    }

    handleHit(hitData) {
        console.log('Hit detected:', hitData);
        // Hit handling is managed by the ESP32 bridge
    }

    handleReset() {
        console.log('Game reset from ESP32 Bridge');
        hideWinner();
        aEl.classList.remove('show');
        btnToggle.textContent = 'Show Answer';
    }

    handleError(message) {
        console.error('ESP32 Bridge Error:', message);
        // Could show error notification to user
    }

    handleQuizAction(action) {
        console.log('Quiz action from ESP32 Bridge:', action);
        // Quiz actions are handled by the main quiz logic
    }

    updateConnectionStatus(connected) {
        const connDot = document.getElementById('connDot');
        if (connDot) {
            connDot.className = connected ? 'ok' : 'bad';
        }
    }

    // Command methods
    resetGame() {
        this.sendCommand({ cmd: 'reset' });
    }

    awardPoint(player, multiplier = 1) {
        this.sendCommand({
            cmd: 'awardPoint',
            player: player,
            multiplier: multiplier
        });
    }

    updateLightboardSettings(mode, p2Color, p3Color) {
        this.sendCommand({
            cmd: 'lightboardSettings',
            mode: mode,
            p2Color: p2Color,
            p3Color: p3Color
        });
    }

    sendQuizAction(action) {
        this.sendCommand({
            cmd: 'quizAction',
            action: action
        });
    }
}

// ===================== Quiz Elements =====================
const quizInterface = document.getElementById('quizInterface');
const resetAllData = document.getElementById('resetAllData');
const fileInputSection = document.getElementById('fileInputSection');
const lightboardModeSection = document.getElementById('lightboardModeSection');
const loadedFiles = document.getElementById('loadedFiles');
const fileList = document.getElementById('fileList');
const categorySelector = document.getElementById('categorySelector');
const categoryGrid = document.getElementById('categoryGrid');
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
const csvFileInput = document.getElementById('csvFile');

// Modal elements
const confirmModal = document.getElementById('confirmModal');
const cancelExit = document.getElementById('cancelExit');
const confirmExit = document.getElementById('confirmExit');
const resetModal = document.getElementById('resetModal');
const cancelReset = document.getElementById('cancelReset');
const confirmReset = document.getElementById('confirmReset');
const lightboardModal = document.getElementById('lightboardModal');
const lightboardSettingsBtn = document.getElementById('lightboardSettingsBtn');
const cancelLightboard = document.getElementById('cancelLightboard');
const confirmLightboard = document.getElementById('confirmLightboard');

// Game status elements in quiz mode
const gameStatus = document.getElementById('gameStatus');
const player2Tile = document.getElementById('player2Tile');
const player3Tile = document.getElementById('player3Tile');
const player2Name = document.querySelector('#player2Tile .player-name');
const player3Name = document.querySelector('#player3Tile .player-name');
const player2ScoreEl = document.getElementById('player2Score');
const player3ScoreEl = document.getElementById('player3Score');

// Lightboard elements (moved to modal)
let lightboardGameMode = 1; // Default to Territory mode
let lightboardP2ColorIndex = 0; // Red
let lightboardP3ColorIndex = 1; // Blue
let damageMultiplier = 3; // Default to triple damage

// Quiz state
let QA = [];
let order = [];
let idx = 0;
let availableCategories = [];

// Player names with persistence
let player2NameText = 'Player 2';
let player3NameText = 'Player 3';

// Quiz state persistence
let currentCategory = null;
let currentQuestionIndex = 0;
let savedOrder = null; // Store the shuffled order to restore exactly

// Scoring system
let player2Score = 0;
let player3Score = 0;
let roundComplete = false;

// ESP32 Bridge instance
let esp32Bridge;

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

// ===================== Data Persistence =====================
function loadPersistedData() {
    try {
        // Load player names
        const savedPlayer2Name = localStorage.getItem('player2Name');
        const savedPlayer3Name = localStorage.getItem('player3Name');
        if (savedPlayer2Name) player2NameText = savedPlayer2Name;
        if (savedPlayer3Name) player3NameText = savedPlayer3Name;
        
        // Load scores
        const savedPlayer2Score = localStorage.getItem('player2Score');
        const savedPlayer3Score = localStorage.getItem('player3Score');
        if (savedPlayer2Score) player2Score = parseInt(savedPlayer2Score);
        if (savedPlayer3Score) player3Score = parseInt(savedPlayer3Score);
        
        // Update player name displays
        player2Name.textContent = player2NameText;
        player3Name.textContent = player3NameText;
        updateScoreDisplay();
        
        // Load categories from localStorage
        const savedCategories = localStorage.getItem('quizCategories');
        if (savedCategories) {
            availableCategories = JSON.parse(savedCategories);
            if (availableCategories.length > 0) {
                showCategorySelector();
                createCategoryButtons(availableCategories);
                
                // Load current quiz state
                const savedCurrentCategory = localStorage.getItem('currentCategory');
                const savedQuestionIndex = localStorage.getItem('currentQuestionIndex');
                const savedOrderData = localStorage.getItem('savedOrder');
                
                if (savedCurrentCategory && savedQuestionIndex !== null) {
                    currentCategory = savedCurrentCategory;
                    currentQuestionIndex = parseInt(savedQuestionIndex);
                    
                    // Restore the saved order if available
                    if (savedOrderData) {
                        try {
                            savedOrder = JSON.parse(savedOrderData);
                        } catch (error) {
                            console.error('Error parsing saved order:', error);
                            savedOrder = null;
                        }
                    }
                    
                    // Restore the quiz to where you left off
                    restoreQuizState();
                }
                
                return; // Don't show sample questions if we have saved categories
            }
        }
    } catch (error) {
        console.error('Error loading persisted data:', error);
    }
    
    // Fallback to sample questions if no saved data
    availableCategories = [{
        filename: 'sample.csv',
        name: 'Sample Questions',
        questions: sampleQuestions
    }];
    showCategorySelector();
    createCategoryButtons(availableCategories);
}

function savePersistedData() {
    const saveData = () => {
        try {
            // Save player names
            localStorage.setItem('player2Name', player2NameText);
            localStorage.setItem('player3Name', player3NameText);
            
            // Save scores
            localStorage.setItem('player2Score', player2Score.toString());
            localStorage.setItem('player3Score', player3Score.toString());
            
            // Save categories
            localStorage.setItem('quizCategories', JSON.stringify(availableCategories));
            
            // Save current quiz state
            if (currentCategory) {
                localStorage.setItem('currentCategory', currentCategory);
                localStorage.setItem('currentQuestionIndex', currentQuestionIndex.toString());
                // Save the current question order to restore exactly
                if (order.length > 0) {
                    localStorage.setItem('savedOrder', JSON.stringify(order));
                }
            }
        } catch (error) {
            console.error('Error saving persisted data:', error);
        }
    };

    // Use requestIdleCallback for better performance if available
    if (window.requestIdleCallback) {
        requestIdleCallback(saveData);
    } else {
        // Fallback for browsers that don't support requestIdleCallback
        setTimeout(saveData, 0);
    }
}

// ===================== Quiz Logic =====================
function initQuiz() {
    loadPersistedData();
    loadLightboardSettings();
}

function setOrder(randomize) {
    order = [...Array(QA.length).keys()];
    if (randomize) shuffle(order);
    idx = 0;
}

function shuffle(arr) {
    for (let i = arr.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [arr[i], arr[j]] = [arr[j], arr[i]];
    }
    return arr;
}

function render(hideAnswer = true) {
    if (QA.length === 0) return;
    
    const qa = QA[order[idx]];
    
    // Batch DOM updates for better performance
    const updates = [
        { element: qEl, property: 'textContent', value: qa.q },
        { element: answerText, property: 'textContent', value: qa.a },
        { element: counterEl, property: 'textContent', value: `${idx+1} / ${QA.length}` }
    ];
    
    // Apply all updates at once
    updates.forEach(update => {
        if (update.element && update.property && update.value !== undefined) {
            update.element[update.property] = update.value;
        }
    });
    
    // Handle answer visibility efficiently
    if (hideAnswer) {
        aEl.classList.remove('show');
        btnToggle.textContent = 'Show Answer';
    }
    
    // Handle category badge efficiently
    if (qa.category) {
        categoryBadge.textContent = qa.category;
        categoryBadge.classList.remove('hidden');
    } else {
        categoryBadge.classList.add('hidden');
    }
}

function navigate(direction) {
    if (direction === 'next') {
        idx = idx < order.length - 1 ? idx + 1 : 0;
    } else {
        idx = idx > 0 ? idx - 1 : order.length - 1;
    }
    
    render();
    currentQuestionIndex = idx;
    savePersistedData();
    
    // Reset game and advance to next question
    esp32Bridge.resetGame();
    hideWinner();
    aEl.classList.remove('show');
    btnToggle.textContent = 'Show Answer';
}

function next() {
    navigate('next');
}

function prev() {
    navigate('prev');
}

function toggleAnswer() {
    aEl.classList.toggle('show');
    btnToggle.textContent = aEl.classList.contains('show') ? 'Hide Answer' : 'Show Answer';
}

// ===================== Game Status =====================
function initQuizMode() {
    // Reset player tiles to default state
    player2Tile.classList.remove('winner');
    player3Tile.classList.remove('winner');
    updateScoreDisplay();
}

function updateScoreDisplay() {
    player2ScoreEl.textContent = player2Score;
    player3ScoreEl.textContent = player3Score;
}

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

function removeScorableState() {
    player2Tile.classList.remove('scorable');
    player3Tile.classList.remove('scorable');
    roundComplete = false;
}

function awardPoint(player) {
    if (!roundComplete) return;
    
    // Update score based on damage multiplier
    if (player === 'Player 2') {
        player2Score += damageMultiplier;
        // Send message to ESP32 Bridge to award points to Player 2
        esp32Bridge.awardPoint(2, damageMultiplier);
    } else if (player === 'Player 3') {
        player3Score += damageMultiplier;
        // Send message to ESP32 Bridge to award points to Player 3
        esp32Bridge.awardPoint(3, damageMultiplier);
    }
    
    updateScoreDisplay();
    savePersistedData();
    
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

// ===================== Event Listeners =====================
// Quiz event listeners
btnNext.addEventListener('click', next);
btnPrev.addEventListener('click', prev);
btnToggle.addEventListener('click', toggleAnswer);
btnExit.addEventListener('click', showExitConfirmation);

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

// Secret long-press functionality for mobile
let pressTimer;
let isLongPress = false;

card.addEventListener('touchstart', function(e) {
    isLongPress = false;
    pressTimer = setTimeout(() => {
        isLongPress = true;
        toggleAnswer();
    }, 800); // 800ms long press
});

card.addEventListener('touchend', function(e) {
    clearTimeout(pressTimer);
});

card.addEventListener('touchmove', function(e) {
    clearTimeout(pressTimer);
});

// Regular click for desktop
card.addEventListener('click', function(e) {
    if (!isLongPress) {
        toggleAnswer();
    }
});

// Keyboard shortcuts
window.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowRight') { e.preventDefault(); next(); }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); prev(); }
    else if (e.key === ' ' || e.code === 'Space') { e.preventDefault(); toggleAnswer(); }
});

// ===================== Modal Functions =====================
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
    
    // Clear current quiz state
    currentCategory = null;
    currentQuestionIndex = 0;
    savedOrder = null;
    localStorage.removeItem('currentCategory');
    localStorage.removeItem('currentQuestionIndex');
    localStorage.removeItem('savedOrder');
    
    // Reset scores
    player2Score = 0;
    player3Score = 0;
    roundComplete = false;
    localStorage.removeItem('player2Score');
    localStorage.removeItem('player3Score');
    updateScoreDisplay();
    removeScorableState();
    
    // Hide modal
    hideExitConfirmation();
    
    // Reset lightboard when exiting quiz
    esp32Bridge.resetGame();
    
    // Return to category selector
    showCategorySelector();
}

// Modal event listeners
cancelExit.addEventListener('click', hideExitConfirmation);
confirmExit.addEventListener('click', exitToCategories);
cancelReset.addEventListener('click', hideResetConfirmation);
confirmReset.addEventListener('click', resetAllDataFunction);
lightboardSettingsBtn.addEventListener('click', showLightboardSettings);
cancelLightboard.addEventListener('click', hideLightboardSettings);
confirmLightboard.addEventListener('click', applyLightboardSettings);

// Close modals when clicking overlay
confirmModal.addEventListener('click', function(e) {
    if (e.target === confirmModal) {
        hideExitConfirmation();
    }
});

resetModal.addEventListener('click', function(e) {
    if (e.target === resetModal) {
        hideResetConfirmation();
    }
});

lightboardModal.addEventListener('click', function(e) {
    if (e.target === lightboardModal) {
        hideLightboardSettings();
    }
});

// Close modals with Escape key
document.addEventListener('keydown', function(e) {
    if (e.key === 'Escape') {
        if (!confirmModal.classList.contains('hidden')) {
            hideExitConfirmation();
        } else if (!resetModal.classList.contains('hidden')) {
            hideResetConfirmation();
        } else if (!lightboardModal.classList.contains('hidden')) {
            hideLightboardSettings();
        }
    }
});

// ===================== Reset Functionality =====================
resetAllData.addEventListener('click', showResetConfirmation);

function showResetConfirmation() {
    resetModal.classList.remove('hidden');
}

function hideResetConfirmation() {
    resetModal.classList.add('hidden');
}

function resetAllDataFunction() {
    // Clear all localStorage data
    localStorage.clear();
    
    // Reset all variables to default state
    availableCategories = [];
    player2Score = 0;
    player3Score = 0;
    player2NameText = 'Player 2';
    player3NameText = 'Player 3';
    currentCategory = null;
    currentQuestionIndex = 0;
    savedOrder = null;
    QA = [];
    order = [];
    idx = 0;
    roundComplete = false;
    
    // Reset lightboard settings to defaults
    lightboardGameMode = 1;
    lightboardP2ColorIndex = 0;
    lightboardP3ColorIndex = 1;
    damageMultiplier = 3;
    
    // Update UI
    player2Name.textContent = player2NameText;
    player3Name.textContent = player3NameText;
    updateScoreDisplay();
    removeScorableState();
    
    // Clear file list
    fileList.innerHTML = '';
    loadedFiles.classList.add('hidden');
    
    // Reset lightboard when resetting all data
    esp32Bridge.resetGame();
    
    // Show sample questions
    availableCategories = [{
        filename: 'sample.csv',
        name: 'Sample Questions',
        questions: sampleQuestions
    }];
    showCategorySelector();
    createCategoryButtons(availableCategories);
    
    // Hide modal
    hideResetConfirmation();
}

// ===================== Lightboard Settings =====================
function showLightboardSettings() {
    // Load current settings from localStorage first
    loadLightboardSettings();
    
    // Set current values in modal
    document.getElementById('lightboardMode').value = lightboardGameMode;
    document.getElementById('lightboardP2Color').value = lightboardP2ColorIndex;
    document.getElementById('lightboardP3Color').value = lightboardP3ColorIndex;
    document.getElementById('damageMultiplier').value = damageMultiplier;
    lightboardModal.classList.remove('hidden');
}

function hideLightboardSettings() {
    lightboardModal.classList.add('hidden');
}

function applyLightboardSettings() {
    const newMode = parseInt(document.getElementById('lightboardMode').value);
    const newP2Color = parseInt(document.getElementById('lightboardP2Color').value);
    const newP3Color = parseInt(document.getElementById('lightboardP3Color').value);
    const newMultiplier = parseInt(document.getElementById('damageMultiplier').value);
    
    // Update local variables
    lightboardGameMode = newMode;
    lightboardP2ColorIndex = newP2Color;
    lightboardP3ColorIndex = newP3Color;
    damageMultiplier = newMultiplier;
    
    // Save settings to localStorage
    saveLightboardSettings();
    
    // Send settings to ESP32 Bridge
    esp32Bridge.updateLightboardSettings(newMode, newP2Color, newP3Color);
    
    hideLightboardSettings();
}

function saveLightboardSettings() {
    localStorage.setItem('lightboardGameMode', lightboardGameMode.toString());
    localStorage.setItem('lightboardP2ColorIndex', lightboardP2ColorIndex.toString());
    localStorage.setItem('lightboardP3ColorIndex', lightboardP3ColorIndex.toString());
    localStorage.setItem('damageMultiplier', damageMultiplier.toString());
}

function loadLightboardSettings() {
    const savedMode = localStorage.getItem('lightboardGameMode');
    const savedP2Color = localStorage.getItem('lightboardP2ColorIndex');
    const savedP3Color = localStorage.getItem('lightboardP3ColorIndex');
    const savedMultiplier = localStorage.getItem('damageMultiplier');
    
    if (savedMode !== null) {
        lightboardGameMode = parseInt(savedMode);
    }
    if (savedP2Color !== null) {
        lightboardP2ColorIndex = parseInt(savedP2Color);
    }
    if (savedP3Color !== null) {
        lightboardP3ColorIndex = parseInt(savedP3Color);
    }
    if (savedMultiplier !== null) {
        damageMultiplier = parseInt(savedMultiplier);
    }
}

// ===================== File Handling =====================
csvFileInput.addEventListener('change', handleFileSelect);

function handleFileSelect(event) {
    const files = event.target.files;
    if (files.length === 0) return;

    // Clear previous categories
    availableCategories = [];
    fileList.innerHTML = '';
    
    let loadedCount = 0;
    const totalFiles = files.length;

    // Helper function to finish loading
    const finishLoading = () => {
        if (availableCategories.length > 0) {
            showCategorySelector();
            createCategoryButtons(availableCategories);
            savePersistedData();
        }
    };

    // Helper function to process file
    const processFile = (file) => {
        if (file.type === 'text/csv' || file.name.endsWith('.csv')) {
            const reader = new FileReader();
            
            reader.onload = function(e) {
                try {
                    const csvText = e.target.result;
                    const csvData = parseCSV(csvText);
                    
                    // Convert CSV data to the expected format
                    const categoryName = file.name.replace('.csv', '').replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
                    const questions = csvData.map(row => {
                        const question = row.Question || row.question || row.Q || row.q || Object.values(row)[0];
                        const answer = row.Answer || row.answer || row.A || row.a || Object.values(row)[1];
                        // Use the filename as the category name for all questions in this file
                        const category = row.Category || row.category || row.Cat || row.cat || categoryName;
                        return { q: question, a: answer, category: category };
                    }).filter(qa => qa.q && qa.a);
                    
                    if (questions.length > 0) {
                        const categoryName = file.name.replace('.csv', '').replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase());
                        availableCategories.push({
                            filename: file.name,
                            name: categoryName,
                            questions: questions
                        });
                        
                        addFileToList(file.name, `${questions.length} questions`, 'success');
                    } else {
                        addFileToList(file.name, 'No questions', 'error');
                    }
                } catch (error) {
                    console.error('Error parsing CSV:', error);
                    addFileToList(file.name, 'Error', 'error');
                }
                
                loadedCount++;
                if (loadedCount === totalFiles) finishLoading();
            };
            
            reader.onerror = function() {
                addFileToList(file.name, 'Read failed', 'error');
                loadedCount++;
                if (loadedCount === totalFiles) finishLoading();
            };
            
            reader.readAsText(file);
        } else {
            addFileToList(file.name, 'Not CSV', 'error');
            loadedCount++;
            if (loadedCount === totalFiles) finishLoading();
        }
    };
    
    Array.from(files).forEach(processFile);
    loadedFiles.classList.remove('hidden');
}

function addFileToList(filename, message, status) {
    const li = document.createElement('li');
    li.className = status;
    li.innerHTML = `${filename.replace('.csv', '')}: ${message}`;
    fileList.appendChild(li);
}

// ===================== CSV Parser =====================
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

// ===================== Category Selection =====================
function showCategorySelector() {
    categorySelector.classList.remove('hidden');
    quizDisplay.classList.add('hidden');
    fileInputSection.classList.remove('hidden');
    resetAllData.parentElement.classList.remove('hidden');
    lightboardModeSection.classList.remove('hidden');
}

function showQuizDisplay() {
    categorySelector.classList.add('hidden');
    quizDisplay.classList.remove('hidden');
    fileInputSection.classList.add('hidden');
    resetAllData.parentElement.classList.add('hidden');
    lightboardModeSection.classList.add('hidden');
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

function loadCategoryData(categoryData, isCombined = false) {
    if (isCombined) {
        // Combine all questions from all categories
        QA = [];
        const categoryNames = [];
        
        categoryData.forEach(category => {
            const questionsWithCategory = category.questions.map(qa => ({
                ...qa,
                category: category.name
            }));
            QA = QA.concat(questionsWithCategory);
            categoryNames.push(category.name);
        });
        
        quizTitle.textContent = `Mixed: ${categoryNames.join(', ')}`;
        currentCategory = 'combined';
    } else {
        QA = categoryData.questions;
        quizTitle.textContent = categoryData.name;
        currentCategory = categoryData.filename;
    }
    
    currentQuestionIndex = 0;
    showQuizDisplay();
    setOrder(true);
    render(true);
    savePersistedData();
}

function loadCategory(filename) {
    const category = availableCategories.find(cat => cat.filename === filename);
    if (!category) {
        alert('Category not found');
        return;
    }
    loadCategoryData(category);
}

function loadCombinedCategories(categories) {
    loadCategoryData(categories, true);
}

// ===================== Player Name Editing =====================
let nameEditTimer;
let isEditingName = false;

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
        
        // Update stored names and save
        if (nameElement === player2Name) {
            player2NameText = newName;
        } else if (nameElement === player3Name) {
            player3NameText = newName;
        }
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

// ===================== Quiz State Restoration =====================
function restoreQuizState() {
    const restoreOrder = () => {
        if (savedOrder && savedOrder.length === QA.length) {
            order = [...savedOrder];
            idx = currentQuestionIndex;
        } else {
            setOrder(true);
            idx = currentQuestionIndex;
        }
        render(true);
    };

    if (currentCategory === 'combined') {
        // Restore combined categories
        QA = [];
        const categoryNames = [];
        
        availableCategories.forEach(category => {
            const questionsWithCategory = category.questions.map(qa => ({
                ...qa,
                category: category.name
            }));
            QA = QA.concat(questionsWithCategory);
            categoryNames.push(category.name);
        });

        quizTitle.textContent = `Mixed: ${categoryNames.join(', ')}`;
        showQuizDisplay();
        restoreOrder();
    } else {
        // Restore specific category
        const category = availableCategories.find(cat => cat.filename === currentCategory);
        if (category) {
            QA = category.questions;
            quizTitle.textContent = category.name;
            showQuizDisplay();
            restoreOrder();
        }
    }
}

// ===================== Initialization =====================
document.addEventListener('DOMContentLoaded', function() {
    // Initialize ESP32 Bridge connection
    esp32Bridge = new ESP32Bridge();
    
    // Initialize quiz
    initQuiz();
    initQuizMode();
    setupPlayerNameEditing();
});

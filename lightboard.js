import fs from 'fs';
import path from 'path';

class LightboardStateManager {
  constructor(stateFilePath = 'lightboard.json') {
    this.stateFilePath = path.join(process.cwd(), stateFilePath);
    this.state = this.loadState();
    this.saveDebounceTimer = null;
    this.saveDebounceMs = 100; // Debounce saves by 100ms
  }

  getDefaultState() {
    return {
      gameState: {
        mode: 1,
        p2ColorIndex: 0,
        p3ColorIndex: 1,
        p2Pos: -1,
        p3Pos: 38,
        nextLedPos: 0,
        tugBoundary: 18,
        p2RacePos: -1,
        p3RacePos: -1,
        celebrating: false,
        winner: 0,
        scoringSequence: []
      }
    };
  }

  loadState() {
    try {
      if (fs.existsSync(this.stateFilePath)) {
        const stateData = fs.readFileSync(this.stateFilePath, 'utf8');
        const loaded = JSON.parse(stateData);
        // Merge with defaults to ensure all fields exist
        const defaultState = this.getDefaultState();
        return {
          gameState: {
            ...defaultState.gameState,
            ...loaded.gameState
          }
        };
      }
    } catch (error) {
      console.error('Error loading lightboard state:', error);
    }
    return this.getDefaultState();
  }

  saveState() {
    try {
      const stateData = JSON.stringify(this.state, null, 2);
      fs.writeFileSync(this.stateFilePath, stateData, 'utf8');
      return true;
    } catch (error) {
      console.error('Error saving lightboard state:', error);
      return false;
    }
  }

  // Debounced save - waits for multiple rapid updates
  saveStateDebounced() {
    if (this.saveDebounceTimer) {
      clearTimeout(this.saveDebounceTimer);
    }
    this.saveDebounceTimer = setTimeout(() => {
      this.saveState();
      this.saveDebounceTimer = null;
    }, this.saveDebounceMs);
  }

  getState() {
    return { ...this.state };
  }

  getGameState() {
    return { ...this.state.gameState };
  }

  updateState(updates) {
    if (updates.gameState) {
      this.state.gameState = {
        ...this.state.gameState,
        ...updates.gameState
      };
    } else {
      this.state = {
        ...this.state,
        ...updates
      };
    }
    this.saveStateDebounced();
  }

  // Handle point award - update state based on game mode
  awardPoint(scoringPlayer, multiplier = 1) {
    const gameState = this.state.gameState;
    
    for (let i = 0; i < multiplier; i++) {
      switch (gameState.mode) {
        case 1: // Territory
          if (scoringPlayer === 2 && gameState.p2Pos < 37) {
            gameState.p2Pos++;
          } else if (scoringPlayer === 3 && gameState.p3Pos > 0) {
            gameState.p3Pos--;
          }
          break;
          
        case 2: // Swap Sides
          if (scoringPlayer === 2 && gameState.p2Pos < 37) {
            gameState.p2Pos++;
          } else if (scoringPlayer === 3 && gameState.p3Pos > 0) {
            gameState.p3Pos--;
          }
          break;
          
        case 3: // Split Scoring
          if (scoringPlayer === 2 && gameState.p2Pos > 0) {
            gameState.p2Pos--;
          } else if (scoringPlayer === 3 && gameState.p3Pos < 37) {
            gameState.p3Pos++;
          }
          break;
          
        case 4: // Score Order
          if (gameState.nextLedPos < 38) {
            if (!gameState.scoringSequence) gameState.scoringSequence = [];
            gameState.scoringSequence[gameState.nextLedPos] = scoringPlayer;
            gameState.nextLedPos++;
          }
          break;
          
        case 5: // Race
          if (scoringPlayer === 2 && gameState.p2RacePos < 37) {
            gameState.p2RacePos++;
          } else if (scoringPlayer === 3 && gameState.p3RacePos < 37) {
            gameState.p3RacePos++;
          }
          break;
          
        case 6: // Tug O War
          if (scoringPlayer === 2 && gameState.tugBoundary < 37) {
            gameState.tugBoundary++;
          } else if (scoringPlayer === 3 && gameState.tugBoundary >= 0) {
            gameState.tugBoundary--;
          }
          break;
      }
    }
    
    this.saveStateDebounced();
  }

  // Reset game state
  resetGame() {
    const mode = this.state.gameState.mode;
    const defaultState = this.getDefaultState().gameState;
    
    switch (mode) {
      case 1:
      case 2:
        this.state.gameState.p2Pos = -1;
        this.state.gameState.p3Pos = 38;
        break;
      case 3:
        this.state.gameState.p2Pos = 18; // CENTER_LEFT + 1
        this.state.gameState.p3Pos = 19; // CENTER_RIGHT - 1
        break;
      case 4:
        this.state.gameState.nextLedPos = 0;
        this.state.gameState.scoringSequence = [];
        break;
      case 5:
        this.state.gameState.p2RacePos = -1;
        this.state.gameState.p3RacePos = -1;
        break;
      case 6:
        this.state.gameState.tugBoundary = 18; // CENTER_LEFT
        break;
    }
    
    this.state.gameState.celebrating = false;
    this.state.gameState.winner = 0;
    this.saveStateDebounced();
  }

  // Update lightboard settings (mode, colors)
  updateSettings(mode, p2Color, p3Color) {
    if (mode >= 1 && mode <= 6) {
      this.state.gameState.mode = mode;
    }
    if (p2Color >= 0 && p2Color <= 4) {
      this.state.gameState.p2ColorIndex = p2Color;
    }
    if (p3Color >= 0 && p3Color <= 4) {
      this.state.gameState.p3ColorIndex = p3Color;
    }
    this.saveStateDebounced();
  }
}

// Export singleton instance
export const lightboardState = new LightboardStateManager();


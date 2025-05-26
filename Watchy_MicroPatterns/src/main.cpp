#include "main.h" // Includes WatchyMicroPatterns class and all manager headers

// Global instance of the watch face
WatchyMicroPatterns watchyFace;

// === WatchyMicroPatterns Class Implementation ===

WatchyMicroPatterns::WatchyMicroPatterns() 
    : Watchy() // Call base class constructor
    , renderController(displayManager) // Initialize RenderController with DisplayManager
    , networkManager(nullptr)        // Initialize NetworkManager (nullptr for SystemManager for now)
    , managersInitialized(false)
{
    // SystemManager, ScriptManager, DisplayManager are default constructed.
    // currentScriptHumanId and currentScriptName will be initialized by initializeManagers
}

void WatchyMicroPatterns::initializeManagers() {
    if (managersInitialized) return;

    Serial.println("Initializing Managers...");

    // Watchy::init() is called by the main setup() before this.
    // Watchy RTC is initialized by Watchy::init()

    if (!displayManager.initializeEPD()) { // Sets up text properties, screen dimensions
        Serial.println("DisplayManager EPD initialization failed!");
        // Handle error: Show error on display
        Watchy::display.fillScreen(GxEPD_WHITE);
        Watchy::display.setTextColor(GxEPD_BLACK);
        Watchy::display.setCursor(10, 10);
        Watchy::display.print("DM Init Fail!");
        Watchy::display.display(false); // Full update
        while(1); // Halt
    }
    Serial.println("DisplayManager initialized.");

    if (!scriptManager.initialize()) { // Initializes SPIFFS
        Serial.println("ScriptManager SPIFFS initialization failed!");
        displayManager.showMessage("SPIFFS Fail!", 50, GxEPD_BLACK, false, true);
        while(1); // Halt
    }
    Serial.println("ScriptManager initialized.");
    
    if (!systemManager.initialize()) { // Loads NVS settings
        Serial.println("SystemManager NVS initialization failed!");
        displayManager.showMessage("NVS Fail!", 50, GxEPD_BLACK, false, true);
        // Not halting, as some functionality might still work with default settings
    }
    Serial.println("SystemManager initialized.");

    // Optional: Attempt WiFi Connection once during initial setup
    // displayManager.showMessage("Connecting WiFi...", 30, GxEPD_BLACK, true, true);
    // bool wifiConnected = networkManager.connectWiFi();
    // if (wifiConnected) {
    //   displayManager.showMessage("WiFi Connected!", 50, GxEPD_BLACK, false, false);
    //   Serial.println("WiFi Connected!");
    //   // Optional: NTP Sync after WiFi connection
    //   // systemManager.syncTimeWithNTP(networkManager);
    // } else {
    //   displayManager.showMessage("WiFi Fail!", 50, GxEPD_BLACK, false, false);
    //   Serial.println("WiFi Connection Failed. Proceeding with cached/default scripts.");
    // }
    // delay(1000);

    managersInitialized = true;
    Serial.println("All managers initialized.");
    
    // Load initial script ID and name after managers are ready
    String tempFileId; // Not directly used here, but getScriptForExecution needs it
    ScriptExecState tempState;
    if (!scriptManager.getScriptForExecution(currentScriptHumanId, tempFileId, tempState)) {
        Serial.println("initializeManagers: Failed to get initial script for execution. Using emergency fallback ID.");
        currentScriptHumanId = ScriptManager::DEFAULT_SCRIPT_ID; // Fallback
    }
    // Attempt to get a display name if list is available
    JsonDocument listDoc;
    if (scriptManager.loadScriptList(listDoc) && listDoc.is<JsonArray>()) {
        for (JsonVariant item : listDoc.as<JsonArray>()) {
            if (item.is<JsonObject>()) {
                JsonObject scriptInfo = item.as<JsonObject>();
                if (!scriptInfo["id"].isNull() && scriptInfo["id"].as<String>() == currentScriptHumanId) {
                    if (!scriptInfo["name"].isNull()) {
                        currentScriptName = scriptInfo["name"].as<String>();
                        break;
                    }
                }
            }
        }
    }
    if (currentScriptName.isEmpty()) currentScriptName = currentScriptHumanId; // Fallback name
    Serial.print("Initial script set to: "); Serial.print(currentScriptName); 
    Serial.print(" (ID: "); Serial.print(currentScriptHumanId); Serial.println(")");
}

void WatchyMicroPatterns::renderCurrentScript() {
    Serial.println("renderCurrentScript called.");
    String fileId, scriptContent; // fileId is retrieved by getScriptForExecution
    ScriptExecState initialState;

    // Use currentScriptHumanId (member) to get the full script details
    if (!scriptManager.getScriptForExecution(currentScriptHumanId, fileId, initialState)) {
        Serial.println("Failed to get script details from ScriptManager. Using emergency fallback.");
        scriptContent = "COLOR BLACK FILL SOLID FILL_RECT X 0 Y 0 WIDTH 200 HEIGHT 200 COLOR WHITE RECT X 10 Y 10 WIDTH 180 HEIGHT 180";
        currentScriptHumanId = "emergency_fallback"; // Ensure humanId is not empty
        // currentScriptName is not updated here, might show old name with fallback script
    } else {
        // currentScriptHumanId is already set or updated by getScriptForExecution
        // fileId and initialState are now populated by getScriptForExecution
        Serial.print("Script to execute: humanId='"); Serial.print(currentScriptHumanId);
        Serial.print("', fileId='"); Serial.print(fileId); Serial.println("'");

        if (!scriptManager.loadScriptContent(fileId, scriptContent)) {
            Serial.print("Failed to load content for script fileId: "); Serial.println(fileId);
            Serial.println("Using emergency fallback script content for rendering.");
            scriptContent = "COLOR BLACK FILL SOLID FILL_RECT X 0 Y 0 WIDTH 200 HEIGHT 200 COLOR WHITE RECT X 5 Y 5 WIDTH 190 HEIGHT 190";
            currentScriptHumanId = "emergency_fallback_load_fail"; 
        } else {
            Serial.print("Script content loaded (length "); Serial.print(scriptContent.length()); Serial.println(").");
        }
    }

    // Update initial state with current time from Watchy RTC
    ::RTC_Date now = Watchy::RTC.getRtcDateTime();
    initialState.hour = now.Hour;
    initialState.minute = now.Minute;
    initialState.second = now.Second;
    // Counter is loaded by getScriptForExecution (from NVS or default 0)

    Serial.println("Attempting to render script...");

    if (displayManager.lockEPD()) {
        RenderResultData result = renderController.renderScript(currentScriptHumanId, scriptContent, initialState);
        displayManager.pushCanvasUpdate(false); // false for full update
        displayManager.unlockEPD();

        if (result.success) {
            Serial.println("Script rendered successfully.");
            if (result.final_state.state_loaded) { 
                 scriptManager.saveScriptExecutionState(currentScriptHumanId, result.final_state);
            }
        } else {
            Serial.print("Script rendering failed: "); Serial.println(result.error_message);
            displayManager.showMessage("Render Fail!", Watchy::display.height()/2 - 20, GxEPD_BLACK, true, true); 
            displayManager.showMessage(result.error_message.substring(0,20), Watchy::display.height()/2, GxEPD_BLACK, false, false); 
            displayManager.showMessage(result.error_message.substring(20,40), Watchy::display.height()/2 + 20, GxEPD_BLACK, false, false); 
        }
    } else {
        Serial.println("Failed to lock EPD for rendering.");
        displayManager.showMessage("EPD Lock Fail!", Watchy::display.height()/2, GxEPD_BLACK, true, true);
    }
}

void WatchyMicroPatterns::drawWatchFace() {
    Serial.println("drawWatchFace called.");
    
    if (!managersInitialized) {
        initializeManagers(); // This will also load initial currentScriptHumanId and currentScriptName
    }
    
    renderCurrentScript();
}

void WatchyMicroPatterns::handleButtonPress() {
    Serial.println("handleButtonPress called.");
    bool scriptChanged = false;

    if (guiState == WATCHFACE_STATE) { 
        if (IS_BTN_UP_PRESSED) { 
            Serial.println("UP Button Action -> Previous Script");
            scriptChanged = scriptManager.selectNextScript(true, currentScriptHumanId, currentScriptName); // true for moveUp (previous)
        } else if (IS_BTN_DOWN_PRESSED) { 
            Serial.println("DOWN Button Action -> Next Script");
            scriptChanged = scriptManager.selectNextScript(false, currentScriptHumanId, currentScriptName); // false for moveDown (next)
        } else if (IS_BTN_MENU_PRESSED) { 
            Serial.println("MENU/SELECT Button Action -> Refresh Current Script");
            scriptChanged = true; // Force refresh
        } else if (IS_BTN_BACK_PRESSED) { 
            Serial.println("BACK Button Action -> (no action defined yet)");
            // displayManager.showMessage("BACK Press", 150, GxEPD_BLACK, false, false);
            // showWatchFace(true); // Partial refresh if needed for the message
            return; // No screen change if no action
        }

        if (scriptChanged) {
            Serial.print("New script selected: "); Serial.print(currentScriptName);
            Serial.print(" (ID: "); Serial.print(currentScriptHumanId); Serial.println(")");
            // Force a full redraw for the new/refreshed script
            showWatchFace(false); 
        }
    }
}


// === Standard Arduino Setup and Loop ===
void setup() {
  watchyFace.init(); // Use Watchy's init, optionally with settings string
  // initializeManagers() will be called by the first drawWatchFace()
}

void loop() {
  // Watchy::loop() is not typically used. Watchy::showWatchFace() is the main execution path.
  // The device sleeps after showWatchFace() and wakes up via RTC or button interrupt.
  // However, Watchy::init() starts a FreeRTOS task that calls showWatchFace.
  // For this project, we don't need anything in Arduino loop().
}

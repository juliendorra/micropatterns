#ifndef MAIN_H
#define MAIN_H

#include <Watchy.h>
#include "display_manager.h"
#include "render_controller.h"
#include "script_manager.h"
#include "network_manager.h"
#include "system_manager.h"
#include "global_setting.h" // For button pins, though Watchy.h also provides them.
#include "event_defs.h"

class WatchyMicroPatterns : public Watchy {
public:
    DisplayManager displayManager;
    RenderController renderController;
    ScriptManager scriptManager;
    NetworkManager networkManager;
    SystemManager systemManager;

    bool managersInitialized;
    String currentScriptHumanId;
    String currentScriptName;

public:
    WatchyMicroPatterns();
    void drawWatchFace() override; // Main function to draw the watch face
    void handleButtonPress() override; // Handle button inputs

private:
    void initializeManagers(); // Helper to initialize all managers
    void renderCurrentScript(); // Helper to load and render the script
};

#endif // MAIN_H

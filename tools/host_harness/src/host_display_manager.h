// Host-only hook for sizing the DisplayManager canvas before initializeEPD().
// Not part of the firmware API.
#ifndef HOST_DISPLAY_MANAGER_H
#define HOST_DISPLAY_MANAGER_H

void hostSetCanvasSize(int width, int height);

#endif // HOST_DISPLAY_MANAGER_H

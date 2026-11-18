/*
 * Copyright 2026, David Beckman.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef OUTPUT_VIEW_CONTROLLER_H
#define OUTPUT_VIEW_CONTROLLER_H

#include "Config.h"

#if ENABLE_LOCAL_OUTPUT

#include "AudioOutputManager.h"
#include <vector>
#include <String.h>

class MainWindow;
class BMenu;
class BMessage;

class OutputViewController {
public:
    explicit OutputViewController(MainWindow* window);
    ~OutputViewController();

    void ShowLocalOutputMenu();
    void RebuildOutputMenu();
    
    void SelectLocalOutput(BMessage* msg);
    void SetConflictPolicy(BMessage* msg);
    void SetFallbackDevice(BMessage* msg);

    void RefreshDevices();
    void ToggleLocalOutputButton();
    void ApplyButtonVisibility();
    
    AudioOutputManager& OutputManager() { return fOutputManager; }

    MixerConflictPolicy Policy() const { return fPolicy; }
    BString FallbackDevice() const { return fFallbackDevice; }

    void SetPolicyAndFallback(MixerConflictPolicy policy, const BString& fallback) {
        fPolicy = policy;
        fFallbackDevice = fallback;
    }

private:
    MainWindow* fWindow;
    AudioOutputManager fOutputManager;
    std::vector<OutputBusInfo> fDevices;
    
    MixerConflictPolicy fPolicy;
    BString fFallbackDevice;

    void _BuildConflictMenu(BMenu* menu);
};

#endif // ENABLE_LOCAL_OUTPUT

#endif // OUTPUT_VIEW_CONTROLLER_H

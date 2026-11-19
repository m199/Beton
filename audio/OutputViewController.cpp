/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "OutputViewController.h"
#include "MainWindow.h"
#include "Messages.h"
#include "AudioPlaybackEngine.h"
#include <Button.h>
#include <Menu.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Catalog.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "OutputViewController"

#if ENABLE_LOCAL_OUTPUT

OutputViewController::OutputViewController(MainWindow* window)
    : fWindow(window),
      fPolicy(MixerConflictPolicy::Disconnect),
      fFallbackDevice("") {
}

OutputViewController::~OutputViewController() {
}

void OutputViewController::ShowLocalOutputMenu() {
    if (!fWindow || !fWindow->fLocalOutputMenu || !fWindow->fBtnLocalOutput)
        return;

    BPoint p = fWindow->fBtnLocalOutput->Bounds().LeftBottom();
    fWindow->fBtnLocalOutput->ConvertToScreen(&p);
    BMenuItem *item = fWindow->fLocalOutputMenu->Go(p);
    if (item && item->Message()) {
        BMessage localMessage(*item->Message());
        fWindow->PostMessage(&localMessage);
    }
}

void OutputViewController::RebuildOutputMenu() {
    if (!fWindow || !fWindow->fLocalOutputMenu)
        return;

    // Clear existing items in popup menu
    while (fWindow->fLocalOutputMenu->CountItems() > 0) {
        delete fWindow->fLocalOutputMenu->RemoveItem((int32)0);
    }
    
    // Clear existing items in settings menu
    if (fWindow->fLocalOutputSettingsMenu) {
        while (fWindow->fLocalOutputSettingsMenu->CountItems() > 0) {
            delete fWindow->fLocalOutputSettingsMenu->RemoveItem((int32)0);
        }
    }

    // Determine if default is selected
    bool isDefaultSelected = (fWindow->fPlaybackEngine->LocalOutputTarget() == OutputTarget::SystemDefault);

    // 1. Add "System default (mixer)"
    BMessage* defaultMsg = new BMessage(MSG_LOCAL_OUTPUT_SELECTED);
    defaultMsg->AddInt32("target", (int32)OutputTarget::SystemDefault);
    BMenuItem* defaultItem = new BMenuItem(B_TRANSLATE("System default (mixer)"), defaultMsg);
    if (isDefaultSelected) {
        defaultItem->SetMarked(true);
    }
    fWindow->fLocalOutputMenu->AddItem(defaultItem);
    
    if (fWindow->fLocalOutputSettingsMenu) {
        BMessage* defaultMsgOut = new BMessage(MSG_LOCAL_OUTPUT_SELECTED);
        defaultMsgOut->AddInt32("target", (int32)OutputTarget::SystemDefault);
        BMenuItem* defaultItemOut = new BMenuItem(B_TRANSLATE("System default (mixer)"), defaultMsgOut);
        if (isDefaultSelected) {
            defaultItemOut->SetMarked(true);
        }
        fWindow->fLocalOutputSettingsMenu->AddItem(defaultItemOut);
    }

    // Refresh devices list
    fDevices.clear();
    fOutputManager.Enumerate(fDevices);

    // Check if we need to fall back fallbackDevice name if empty
    if (fFallbackDevice.IsEmpty() && !fDevices.empty()) {
        // Default fallback to first enumerated device that isn't the default one
        for (const auto& d : fDevices) {
            if (!d.isSystemDefaultDevice) {
                fFallbackDevice = d.deviceName;
                break;
            }
        }
    }

    if (!fDevices.empty()) {
        fWindow->fLocalOutputMenu->AddSeparatorItem();
        if (fWindow->fLocalOutputSettingsMenu) {
            fWindow->fLocalOutputSettingsMenu->AddSeparatorItem();
        }

        // Add each enumerated bus
        for (size_t i = 0; i < fDevices.size(); i++) {
            const auto& bus = fDevices[i];
            
            BString busLabel;
            if (bus.deviceName == bus.busName || bus.busName.StartsWith("input") || bus.busName.StartsWith("output")) {
                busLabel.SetToFormat("%s (%s)", bus.deviceName.String(), bus.busName.String());
            } else {
                busLabel.SetToFormat("%s - %s", bus.deviceName.String(), bus.busName.String());
            }
            
            // Rebuild popup submenu
            BMenu* subMenu = new BMenu(busLabel.String());
            
            BMessage* sharedMsg = new BMessage(MSG_LOCAL_OUTPUT_SELECTED);
            sharedMsg->AddInt32("target", (int32)OutputTarget::DirectShared);
            sharedMsg->AddInt32("index", (int32)i);
            BMenuItem* sharedItem = new BMenuItem(B_TRANSLATE("Shared"), sharedMsg);
            
            BMessage* exclMsg = new BMessage(MSG_LOCAL_OUTPUT_SELECTED);
            exclMsg->AddInt32("target", (int32)OutputTarget::DirectExclusive);
            exclMsg->AddInt32("index", (int32)i);
            BMenuItem* exclItem = new BMenuItem(B_TRANSLATE("Exclusive (claim device)"), exclMsg);
            
            bool isThisBusSelected = (fWindow->fPlaybackEngine->LocalOutputTarget() != OutputTarget::SystemDefault &&
                                      fWindow->fPlaybackEngine->LocalOutputBus().deviceName == bus.deviceName &&
                                      fWindow->fPlaybackEngine->LocalOutputBus().busName == bus.busName);
            
            if (isThisBusSelected) {
                if (fWindow->fPlaybackEngine->LocalOutputTarget() == OutputTarget::DirectShared) {
                    sharedItem->SetMarked(true);
                } else if (fWindow->fPlaybackEngine->LocalOutputTarget() == OutputTarget::DirectExclusive) {
                    exclItem->SetMarked(true);
                }
            }
            
            subMenu->AddItem(sharedItem);
            subMenu->AddItem(exclItem);
            fWindow->fLocalOutputMenu->AddItem(new BMenuItem(subMenu));
            
            // Parallel in settings Output submenu
            if (fWindow->fLocalOutputSettingsMenu) {
                BMenu* subMenuOut = new BMenu(busLabel.String());
                
                BMessage* sharedMsgOut = new BMessage(MSG_LOCAL_OUTPUT_SELECTED);
                sharedMsgOut->AddInt32("target", (int32)OutputTarget::DirectShared);
                sharedMsgOut->AddInt32("index", (int32)i);
                BMenuItem* sharedItemOut = new BMenuItem(B_TRANSLATE("Shared"), sharedMsgOut);
                
                BMessage* exclMsgOut = new BMessage(MSG_LOCAL_OUTPUT_SELECTED);
                exclMsgOut->AddInt32("target", (int32)OutputTarget::DirectExclusive);
                exclMsgOut->AddInt32("index", (int32)i);
                BMenuItem* exclItemOut = new BMenuItem(B_TRANSLATE("Exclusive (claim device)"), exclMsgOut);
                
                if (isThisBusSelected) {
                    if (fWindow->fPlaybackEngine->LocalOutputTarget() == OutputTarget::DirectShared) {
                        sharedItemOut->SetMarked(true);
                    } else if (fWindow->fPlaybackEngine->LocalOutputTarget() == OutputTarget::DirectExclusive) {
                        exclItemOut->SetMarked(true);
                    }
                }
                
                subMenuOut->AddItem(sharedItemOut);
                subMenuOut->AddItem(exclItemOut);
                fWindow->fLocalOutputSettingsMenu->AddItem(new BMenuItem(subMenuOut));
            }
        }
    }

    // Add settings entries to settings output menu
    if (fWindow->fLocalOutputSettingsMenu) {
        fWindow->fLocalOutputSettingsMenu->AddSeparatorItem();
        
        // Show Button
        BMenuItem* showBtnItem = new BMenuItem(B_TRANSLATE("Show Toolbar Button"), new BMessage(MSG_LOCAL_OUTPUT_REFRESH + 100 /* Toggle button msg ID */));
        showBtnItem->SetMarked(fWindow->fShowLocalOutputBtn);
        fWindow->fLocalOutputSettingsMenu->AddItem(showBtnItem);
        
        // Refresh Devices
        BMenuItem* refreshItem = new BMenuItem(B_TRANSLATE("Refresh Devices"), new BMessage(MSG_LOCAL_OUTPUT_REFRESH));
        fWindow->fLocalOutputSettingsMenu->AddItem(refreshItem);
        
        fWindow->fLocalOutputSettingsMenu->AddSeparatorItem();
        
        // Conflict policy
        BMenu* policySubMenu = new BMenu(B_TRANSLATE("On System Mixer Conflict"));
        _BuildConflictMenu(policySubMenu);
        fWindow->fLocalOutputSettingsMenu->AddItem(new BMenuItem(policySubMenu));
    }

    fWindow->fLocalOutputMenu->SetTargetForItems(fWindow);
    if (fWindow->fLocalOutputSettingsMenu) {
        fWindow->fLocalOutputSettingsMenu->SetTargetForItems(fWindow);
    }
}

void OutputViewController::SelectLocalOutput(BMessage* msg) {
    if (!fWindow || !msg)
        return;
        
    int32 targetVal;
    if (msg->FindInt32("target", &targetVal) != B_OK)
        return;
        
    OutputTarget target = (OutputTarget)targetVal;
    
    if (target == OutputTarget::SystemDefault) {
        fWindow->fPlaybackEngine->SetOutputDevice(target, OutputBusInfo(), fPolicy, fFallbackDevice);
    } else {
        int32 index;
        if (msg->FindInt32("index", &index) == B_OK && index >= 0 && index < (int32)fDevices.size()) {
            fWindow->fPlaybackEngine->SetOutputDevice(target, fDevices[index], fPolicy, fFallbackDevice);
        }
    }
    
    // Notify window to trigger settings save
    BMessage saveMsg(MSG_LOCAL_OUTPUT_REFRESH + 101 /* trigger settings save */);
    fWindow->PostMessage(&saveMsg);

    RebuildOutputMenu();
}

void OutputViewController::SetConflictPolicy(BMessage* msg) {
    int32 policyVal;
    if (msg->FindInt32("policy", &policyVal) == B_OK) {
        fPolicy = (MixerConflictPolicy)policyVal;
        
        if (fWindow->fPlaybackEngine->LocalOutputTarget() == OutputTarget::DirectExclusive) {
            fWindow->fPlaybackEngine->SetOutputDevice(
                fWindow->fPlaybackEngine->LocalOutputTarget(),
                fWindow->fPlaybackEngine->LocalOutputBus(),
                fPolicy,
                fFallbackDevice
            );
        }
        
        // Trigger settings save
        BMessage saveMsg(MSG_LOCAL_OUTPUT_REFRESH + 101);
        fWindow->PostMessage(&saveMsg);
        
        RebuildOutputMenu();
    }
}

void OutputViewController::SetFallbackDevice(BMessage* msg) {
    BString fallback;
    if (msg->FindString("device", &fallback) == B_OK) {
        fPolicy = MixerConflictPolicy::SwitchToDevice;
        fFallbackDevice = fallback;
        
        if (fWindow->fPlaybackEngine->LocalOutputTarget() == OutputTarget::DirectExclusive) {
            fWindow->fPlaybackEngine->SetOutputDevice(
                fWindow->fPlaybackEngine->LocalOutputTarget(),
                fWindow->fPlaybackEngine->LocalOutputBus(),
                fPolicy,
                fFallbackDevice
            );
        }
        
        // Trigger settings save
        BMessage saveMsg(MSG_LOCAL_OUTPUT_REFRESH + 101);
        fWindow->PostMessage(&saveMsg);
        
        RebuildOutputMenu();
    }
}

void OutputViewController::RefreshDevices() {
    RebuildOutputMenu();
}

/**
 * @brief Brings the toolbar button in line with fShowLocalOutputBtn.
 *
 * BView::Hide() and Show() nest: each Hide() raises a counter that a Show()
 * lowers again, and only a count of zero is visible. Calling either one
 * unconditionally lets the counter drift away from the setting - one Show()
 * too many drives it negative, and from then on a Hide() only brings it back
 * to zero and the button never disappears. So move the button only when its
 * own state disagrees with the setting, which also makes this safe to call
 * whenever the setting is (re)loaded. IsHidden(button) asks for the button's
 * own state rather than the whole parent chain's.
 */
void OutputViewController::ApplyButtonVisibility() {
    if (!fWindow || !fWindow->fBtnLocalOutput)
        return;

    BButton* btn = fWindow->fBtnLocalOutput;
    const bool wantVisible = fWindow->fShowLocalOutputBtn;
    if (wantVisible == !btn->IsHidden(btn))
        return;

    if (wantVisible)
        btn->Show();
    else
        btn->Hide();

    if (btn->Parent()) {
        btn->Parent()->InvalidateLayout(true);
        btn->Parent()->Relayout();
    }
    fWindow->InvalidateLayout(true);
    fWindow->Layout(true);
}

void OutputViewController::ToggleLocalOutputButton() {
    if (!fWindow)
        return;

    fWindow->fShowLocalOutputBtn = !fWindow->fShowLocalOutputBtn;
    ApplyButtonVisibility();

    // Trigger settings save
    BMessage saveMsg(MSG_LOCAL_OUTPUT_REFRESH + 101);
    fWindow->PostMessage(&saveMsg);

    RebuildOutputMenu();
}

void OutputViewController::_BuildConflictMenu(BMenu* menu) {
    // Conflict Policy: Disconnect
    BMessage* discMsg = new BMessage(MSG_CONFLICT_POLICY_CHANGED);
    discMsg->AddInt32("policy", (int32)MixerConflictPolicy::Disconnect);
    BMenuItem* discItem = new BMenuItem(B_TRANSLATE("Disconnect System Sounds"), discMsg);
    if (fPolicy == MixerConflictPolicy::Disconnect) {
        discItem->SetMarked(true);
    }
    menu->AddItem(discItem);
    
    // Conflict Policy: Switch to Device
    BMenu* switchSubMenu = new BMenu(B_TRANSLATE("Switch System Sounds to"));
    
    std::vector<BString> uniqueDevices;
    for (const auto& d : fDevices) {
        bool found = false;
        for (const auto& name : uniqueDevices) {
            if (name == d.deviceName) {
                found = true;
                break;
            }
        }
        if (!found) {
            uniqueDevices.push_back(d.deviceName);
        }
    }
    
    for (const auto& devName : uniqueDevices) {
        BMessage* switchMsg = new BMessage(MSG_FALLBACK_DEVICE_CHANGED);
        switchMsg->AddString("device", devName);
        BMenuItem* devItem = new BMenuItem(devName.String(), switchMsg);
        
        if (fPolicy == MixerConflictPolicy::SwitchToDevice && fFallbackDevice == devName) {
            devItem->SetMarked(true);
        }
        switchSubMenu->AddItem(devItem);
    }
    
    BMenuItem* switchSubItem = new BMenuItem(switchSubMenu);
    if (fPolicy == MixerConflictPolicy::SwitchToDevice) {
        switchSubItem->SetMarked(true);
    }
    menu->AddItem(switchSubItem);
}

#endif // ENABLE_LOCAL_OUTPUT

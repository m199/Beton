/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "AudioOutputManager.h"
#include "Debug.h"
#include <OS.h>
#include <ParameterWeb.h>
#include <TimeSource.h>
#include <string.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Directory.h>
#include <Entry.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#if ENABLE_LOCAL_OUTPUT

// Helper to get node for ID and increment reference count
static status_t GetNodeForID(media_node_id id, media_node* outNode) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return B_ERROR;
    
    live_node_info nodes[64];
    int32 count = 64;
    status_t st = roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL, B_PHYSICAL_OUTPUT | B_BUFFER_CONSUMER);
    if (st == B_OK) {
        for (int32 i = 0; i < count; i++) {
            if (nodes[i].node.node == id) {
                *outNode = nodes[i].node;
                // Release all other nodes we don't keep
                for (int32 j = 0; j < count; j++) {
                    if (i != j) {
                        roster->ReleaseNode(nodes[j].node);
                    }
                }
                return B_OK;
            }
        }
        // If not found, release all
        for (int32 j = 0; j < count; j++) {
            roster->ReleaseNode(nodes[j].node);
        }
    }
    return B_ENTRY_NOT_FOUND;
}

AudioOutputManager::AudioOutputManager()
    : fAlteredSystem(false),
      fAlteredPolicy(MixerConflictPolicy::Disconnect),
      fHasOriginalSystemInput(false),
      fHasAcquiredNode(false),
      fRunningNodeId(-1) {
}

AudioOutputManager::~AudioOutputManager() {
    Release();
}

status_t AudioOutputManager::Enumerate(std::vector<OutputBusInfo>& out) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return B_ERROR;

    out.clear();

    // Get current system default output to compare against
    media_node defaultNode;
    int32 defaultInputId = -1;
    BString defaultInputName;
    bool hasDefault = (roster->GetAudioOutput(&defaultNode, &defaultInputId, &defaultInputName) == B_OK);
    NodeReleaser defaultReleaser(defaultNode);

    // Get live physical output nodes
    live_node_info nodes[64];
    int32 count = 64;
    status_t st = roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL, B_PHYSICAL_OUTPUT | B_BUFFER_CONSUMER);
    if (st != B_OK)
        return st;

    for (int32 i = 0; i < count; i++) {
        NodeReleaser releaser(nodes[i].node);

        // Get all inputs for this node (connected + free)
        media_input inputs[16];
        int32 inputCount = 0;
        st = roster->GetAllInputsFor(nodes[i].node, inputs, 16, &inputCount);
        if (st != B_OK)
            continue;

        for (int32 j = 0; j < inputCount; j++) {
            // Filter to only raw/unknown audio inputs
            if (inputs[j].format.type != B_MEDIA_RAW_AUDIO && inputs[j].format.type != B_MEDIA_UNKNOWN_TYPE) {
                continue;
            }

            OutputBusInfo info;
            info.deviceName = nodes[i].name;
            info.busName = inputs[j].name;
            if (info.busName.IsEmpty()) {
                info.busName.SetToFormat("output %d", (int)inputs[j].destination.id);
            }
            info.deviceNode = nodes[i].node;
            info.input = inputs[j];
            
            // Device-level: true for every bus of the device the system
            // mixer is currently on. This matches the exclusive-conflict
            // check in Acquire(), which is device-granular (any bus of the
            // target device counts as a conflict).
            info.isSystemDefaultDevice =
                (hasDefault && nodes[i].node.node == defaultNode.node);

            out.push_back(info);
        }
    }

    return B_OK;
}

status_t AudioOutputManager::CurrentSystemDefault(media_node& node, BString& deviceName) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return B_ERROR;

    int32 inputId = -1;
    BString inputName;
    status_t st = roster->GetAudioOutput(&node, &inputId, &inputName);
    if (st != B_OK)
        return st;

    // Find the device name for this node from live nodes
    live_node_info nodes[64];
    int32 count = 64;
    st = roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL, B_PHYSICAL_OUTPUT | B_BUFFER_CONSUMER);
    if (st == B_OK) {
        for (int32 i = 0; i < count; i++) {
            if (nodes[i].node.node == node.node) {
                deviceName = nodes[i].name;
            }
            roster->ReleaseNode(nodes[i].node);
        }
    }
    return B_OK;
}

status_t AudioOutputManager::GetSystemMixerGainDB(float& outGainDB) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return B_ERROR;

    media_node mixer;
    status_t st = roster->GetAudioMixer(&mixer);
    if (st != B_OK)
        return st;
    NodeReleaser mixerReleaser(mixer);

    BParameterWeb* web = NULL;
    st = roster->GetParameterWebFor(mixer, &web);
    if (st != B_OK || web == NULL)
        return st != B_OK ? st : B_ERROR;

    status_t result = B_ENTRY_NOT_FOUND;
    int32 count = web->CountParameters();
    for (int32 i = 0; i < count; i++) {
        BParameter* p = web->ParameterAt(i);
        if (p == NULL || p->Type() != BParameter::B_CONTINUOUS_PARAMETER)
            continue;
        const char* kind = p->Kind();
        if (kind == NULL || strcmp(kind, B_MASTER_GAIN) != 0)
            continue;

        float vals[8];
        size_t size = sizeof(vals);
        bigtime_t when = 0;
        if (p->GetValue(vals, &size, &when) == B_OK && size >= sizeof(float)) {
            int32 n = (int32)(size / sizeof(float));
            float sum = 0.0f;
            for (int32 c = 0; c < n && c < 8; c++)
                sum += vals[c];
            outGainDB = sum / (n < 8 ? n : 8);
            result = B_OK;
        }
        break; // master gain is unique
    }

    delete web;
    return result;
}

bool AudioOutputManager::GetSystemMixerAttenuate3dB() {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return false;

    media_node mixer;
    if (roster->GetAudioMixer(&mixer) != B_OK)
        return false;
    NodeReleaser mixerReleaser(mixer);

    BParameterWeb* web = NULL;
    if (roster->GetParameterWebFor(mixer, &web) != B_OK || web == NULL)
        return false;

    bool result = false;
    int32 count = web->CountParameters();
    for (int32 i = 0; i < count; i++) {
        BParameter* p = web->ParameterAt(i);
        if (p == NULL || p->Type() != BParameter::B_DISCRETE_PARAMETER)
            continue;
        const char* name = p->Name();
        if (name == NULL)
            continue;
        // Identify the "Attenuate mixer output by 3 dB" toggle by name.
        BString n(name);
        n.ToLower();
        if (n.FindFirst("3 db") < 0 && n.FindFirst("3db") < 0)
            continue;
        int32 val = 0;
        size_t size = sizeof(val);
        bigtime_t when = 0;
        if (p->GetValue(&val, &size, &when) == B_OK)
            result = (val != 0);
        break;
    }

    delete web;
    return result;
}

status_t AudioOutputManager::Acquire(const OutputBusInfo& target,
                                     OutputTarget mode,
                                     MixerConflictPolicy policy,
                                     const BString& fallbackDeviceName,
                                     media_node& outNode,
                                     media_input& outInput) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return B_ERROR;

    Release();

    status_t st = GetNodeForID(target.deviceNode.node, &fAcquiredNode);
    if (st != B_OK)
        return st;
    fHasAcquiredNode = true;

    // Check conflict (is target device the current default output node?)
    media_node defaultNode;
    int32 defaultInputId = -1;
    BString defaultInputName;
    bool hasDefault = (roster->GetAudioOutput(&defaultNode, &defaultInputId, &defaultInputName) == B_OK);
    NodeReleaser defaultNodeReleaser(defaultNode);

    bool isTargetDefault = (hasDefault && target.deviceNode.node == defaultNode.node);

    // A direct connection (Shared or Exclusive) must free the mixer from the
    // port(s) it would collide on: Shared frees only the target bus, Exclusive
    // frees every bus of the device (claims the whole device). A physical
    // output node exposes one connection slot per bus, so leaving the mixer on
    // our bus garbles playback (two producers on one slot).
    bool wantEvict = (mode == OutputTarget::DirectShared ||
                      mode == OutputTarget::DirectExclusive) && isTargetDefault;

    // Determine which mixer->device connections conflict with this claim.
    // (All inputs of a node share the node's control port, so a destination on
    // the target device is identified by matching that port; the specific bus
    // is the destination.id.)
    std::vector<media_output> conflicts;
    if (wantEvict) {
        media_node mixer;
        if (roster->GetAudioMixer(&mixer) == B_OK) {
            NodeReleaser mixerReleaser(mixer);
            media_output outputs[16];
            int32 found = 0;
            if (roster->GetConnectedOutputsFor(mixer, outputs, 16, &found) == B_OK) {
                for (int32 i = 0; i < found; i++) {
                    if (outputs[i].destination.port != target.deviceNode.port)
                        continue;
                    bool sameBus = (outputs[i].destination.id ==
                                    target.input.destination.id);
                    if (mode == OutputTarget::DirectExclusive || sameBus)
                        conflicts.push_back(outputs[i]);
                }
            }
        }
    }

    bool hasConflict = wantEvict && !conflicts.empty();
    DEBUG_PRINT("Acquire: mode=%s conflicts=%d (evicting mixer from %s)\n",
                mode == OutputTarget::DirectExclusive ? "Exclusive" : "Shared",
                (int)conflicts.size(),
                mode == OutputTarget::DirectExclusive ? "all buses" : "target bus");

    if (hasConflict) {
        BString origInputName = defaultInputName;
        if (origInputName.IsEmpty() && defaultInputId != -1) {
            origInputName.SetToFormat("output %d", (int)defaultInputId);
        }

        // 1. Write the crash-safe breadcrumb and fsync (refinement D) BEFORE
        //    mutating the system routing.
        st = _WriteBreadcrumb(target.deviceName, origInputName, policy);
        if (st != B_OK) {
            Release();
            return st;
        }

        fAlteredSystem = true;
        fAlteredPolicy = policy;

        // 2. Perform the policy mutation
        if (policy == MixerConflictPolicy::Disconnect) {
            for (size_t i = 0; i < conflicts.size(); i++) {
                fSavedMixerOutputs.push_back(conflicts[i]);
                roster->Disconnect(conflicts[i].node.node, conflicts[i].source,
                                   target.deviceNode.node,
                                   conflicts[i].destination);
            }
        } else if (policy == MixerConflictPolicy::SwitchToDevice) {
            bool fallbackFound = false;
            live_node_info nodes[64];
            int32 count = 64;
            st = roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL, B_PHYSICAL_OUTPUT | B_BUFFER_CONSUMER);
            if (st == B_OK) {
                for (int32 i = 0; i < count; i++) {
                    NodeReleaser releaser(nodes[i].node);
                    if (fallbackDeviceName == nodes[i].name && nodes[i].node.node != target.deviceNode.node) {
                        media_input inputs[16];
                        int32 inputCount = 0;
                        if (roster->GetAllInputsFor(nodes[i].node, inputs, 16, &inputCount) == B_OK && inputCount > 0) {
                            // Find original system input
                            media_input origInputs[16];
                            int32 origInputCount = 0;
                            if (roster->GetAllInputsFor(defaultNode, origInputs, 16, &origInputCount) == B_OK) {
                                for (int32 k = 0; k < origInputCount; k++) {
                                    if (origInputs[k].destination.id == defaultInputId) {
                                        fOriginalSystemInput = origInputs[k];
                                        fHasOriginalSystemInput = true;
                                        break;
                                    }
                                }
                            }

                            st = roster->SetAudioOutput(inputs[0]);
                            if (st == B_OK) {
                                fallbackFound = true;
                            }
                            break;
                        }
                    }
                }
            }
            if (!fallbackFound) {
                Release();
                return B_ENTRY_NOT_FOUND;
            }
        }
    }

    // Ensure the physical output node is running before we connect a
    // BSoundPlayer to it. On a fresh boot nothing has yet driven this device
    // (only System Mixer mode starts it implicitly), so its DAC output thread
    // is stopped and a direct connection would be silent until the user
    // toggled to System Mixer and back. Start it here.
    _EnsureNodeRunning(fAcquiredNode);

    outNode = fAcquiredNode;
    outInput = target.input;
    return B_OK;
}

void AudioOutputManager::_EnsureNodeRunning(const media_node& node) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return;

    // A physical output (DAC) node only consumes buffers while its output
    // thread runs, and that thread is spawned when the node is started
    // (StartNode -> _HandleStart -> _StartOutputThreadIfNeeded). BSoundPlayer
    // starts only its own producer node, so we must start the consumer node
    // ourselves. This is idempotent: the node no-ops a start when already
    // running, so it is safe in the Shared/non-conflict case where the mixer
    // already has the device going.
    bigtime_t performanceTime = 0;
    BTimeSource* ts = roster->MakeTimeSourceFor(node);
    if (ts != NULL) {
        performanceTime = ts->Now();
        ts->Release();
    }
    roster->StartNode(node, performanceTime);

    // Cold start: a just-spawned DAC output thread is not yet stably pulling
    // buffers, so a BSoundPlayer that connects in the same instant hands its
    // first buffers to a consumer that never consumes them — the stream sits
    // silent with the position frozen (the classic "first track after launch
    // does nothing, the second plays fine" symptom; by the second play the
    // node has been running for a second and is warm). Give the freshly
    // started node a moment to settle so the imminent connection lands on a
    // running node, exactly as the working second-play case does.
    //
    // Gate this on a real cold start: Release() leaves the node running, so
    // once we have started a given node this session it stays warm and later
    // track changes skip the wait and stay snappy. A different node id (device
    // switch, or a fresh launch where fRunningNodeId is -1) is a cold start.
    if (node.node != fRunningNodeId.load(std::memory_order_relaxed)) {
        snooze(250000); // 250 ms, one-time per device cold start
        fRunningNodeId.store(node.node, std::memory_order_relaxed);
    }
}

void AudioOutputManager::Prime(const OutputBusInfo& target) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return;

    // Get (reference) the physical node, start it so its DAC output thread is
    // spinning, then release our reference. StartNode's effect persists after
    // the reference is dropped (Release() relies on the same fact), so the
    // node stays warm for the eventual Play(), which then finds fRunningNodeId
    // already set and connects with no settle. Deliberately does not evict the
    // mixer or alter routing — this only makes an idle device ready.
    media_node node;
    if (GetNodeForID(target.deviceNode.node, &node) != B_OK)
        return;
    _EnsureNodeRunning(node);
    roster->ReleaseNode(node);
}

void AudioOutputManager::Release() {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return;

    if (fAlteredSystem) {
        if (fAlteredPolicy == MixerConflictPolicy::Disconnect) {
            // Reconnect every mixer->device connection we broke.
            for (size_t i = 0; i < fSavedMixerOutputs.size(); i++) {
                media_output outOutput;
                media_input outInput;
                media_format format = fSavedMixerOutputs[i].format;
                roster->Connect(fSavedMixerOutputs[i].source,
                                fSavedMixerOutputs[i].destination, &format,
                                &outOutput, &outInput);
            }
        } else if (fAlteredPolicy == MixerConflictPolicy::SwitchToDevice) {
            if (fHasOriginalSystemInput) {
                roster->SetAudioOutput(fOriginalSystemInput);
            }
        }
        _ClearBreadcrumb();
        fAlteredSystem = false;
        fSavedMixerOutputs.clear();
        fHasOriginalSystemInput = false;
    }

    if (fHasAcquiredNode) {
        roster->ReleaseNode(fAcquiredNode);
        fHasAcquiredNode = false;
    }
}

bool AudioOutputManager::RecoverFromBreadcrumb() {
    BString path = _GetBreadcrumbPath();
    if (path.IsEmpty())
        return false;

    int fd = open(path.String(), O_RDONLY);
    if (fd < 0)
        return false;

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (bytes <= 0)
        return false;

    BString data(buf);
    if (data.FindFirst("altered=true") == B_ERROR)
        return false;

    BString deviceName;
    BString inputName;

    int deviceIdx = data.FindFirst("device=");
    if (deviceIdx != B_ERROR) {
        int endIdx = data.FindFirst("\n", deviceIdx);
        if (endIdx != B_ERROR) {
            data.CopyInto(deviceName, deviceIdx + 7, endIdx - (deviceIdx + 7));
        }
    }

    int inputIdx = data.FindFirst("input=");
    if (inputIdx != B_ERROR) {
        int endIdx = data.FindFirst("\n", inputIdx);
        if (endIdx != B_ERROR) {
            data.CopyInto(inputName, inputIdx + 6, endIdx - (inputIdx + 6));
        }
    }

    if (deviceName.IsEmpty()) {
        _ClearBreadcrumb();
        return false;
    }

    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster)
        return false;

    live_node_info nodes[64];
    int32 count = 64;
    status_t st = roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL, B_PHYSICAL_OUTPUT | B_BUFFER_CONSUMER);
    if (st == B_OK) {
        for (int32 i = 0; i < count; i++) {
            NodeReleaser releaser(nodes[i].node);
            if (deviceName == nodes[i].name) {
                media_input inputs[16];
                int32 inputCount = 0;
                if (roster->GetAllInputsFor(nodes[i].node, inputs, 16, &inputCount) == B_OK) {
                    for (int32 j = 0; j < inputCount; j++) {
                        BString name = inputs[j].name;
                        if (name.IsEmpty()) {
                            name.SetToFormat("output %d", (int)inputs[j].destination.id);
                        }
                        if (name == inputName) {
                            roster->SetAudioOutput(inputs[j]);
                            _ClearBreadcrumb();
                            return true;
                        }
                    }
                }
            }
        }
    }

    _ClearBreadcrumb();
    return false;
}

BString AudioOutputManager::_GetBreadcrumbPath() {
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
        path.Append("Beton");
        path.Append("output_breadcrumb");
        return path.Path();
    }
    return "";
}

status_t AudioOutputManager::_WriteBreadcrumb(const BString& originalDeviceName, const BString& originalInputName, MixerConflictPolicy policy) {
    BString path = _GetBreadcrumbPath();
    if (path.IsEmpty())
        return B_ERROR;

    BPath dirPath(path.String());
    dirPath.GetParent(&dirPath);
    create_directory(dirPath.Path(), 0755);

    int fd = open(path.String(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return errno;

    BString content;
    content.SetToFormat("altered=true\ndevice=%s\ninput=%s\npolicy=%d\n",
                        originalDeviceName.String(),
                        originalInputName.String(),
                        (int)policy);

    write(fd, content.String(), content.Length());
    fsync(fd);
    close(fd);

    return B_OK;
}

void AudioOutputManager::_ClearBreadcrumb() {
    BString path = _GetBreadcrumbPath();
    if (!path.IsEmpty()) {
        unlink(path.String());
    }
}

#endif // ENABLE_LOCAL_OUTPUT

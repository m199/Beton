/*
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef AUDIO_OUTPUT_MANAGER_H
#define AUDIO_OUTPUT_MANAGER_H

#include "Config.h"
#include <MediaNode.h>
#include <MediaRoster.h>
#include <String.h>
#include <atomic>
#include <vector>

#if ENABLE_LOCAL_OUTPUT

class NodeReleaser {
public:
    explicit NodeReleaser(const media_node& n) : fNode(n), fArmed(true) {}
    ~NodeReleaser() {
        if (fArmed) {
            BMediaRoster* roster = BMediaRoster::Roster();
            if (roster)
                roster->ReleaseNode(fNode);
        }
    }
    void Dismiss() { fArmed = false; }
private:
    media_node fNode;
    bool       fArmed;
};

struct OutputBusInfo {
    BString    deviceName;     // e.g. "HD Audio", "USB Audio (RMX2)"
    BString    busName;        // e.g. "output 0 (ch 1-2)"
    media_node deviceNode;     // the physical output node
    media_input input;         // the specific bus's input
    bool        isSystemDefaultDevice; // mixer currently on this device?
};

enum class OutputTarget {
    SystemDefault,
    DirectShared,
    DirectExclusive
};

enum class MixerConflictPolicy {
    Disconnect,
    SwitchToDevice
};

class AudioOutputManager {
public:
    AudioOutputManager();
    ~AudioOutputManager();

    status_t Enumerate(std::vector<OutputBusInfo>& out);
    status_t CurrentSystemDefault(media_node& node, BString& deviceName);

    // Read the system audio mixer's master output gain (dB). This is the gain
    // a direct (Shared/Exclusive) connection bypasses; callers apply it in
    // software to keep loudness consistent when switching output modes.
    status_t GetSystemMixerGainDB(float& outGainDB);

    // True if the mixer's "Attenuate mixer output by 3 dB" toggle is on. The
    // mixer applies a straight x0.708 to its output when enabled (and it is on
    // by default), which a direct connection also bypasses.
    bool GetSystemMixerAttenuate3dB();

    status_t Acquire(const OutputBusInfo& target,
                     OutputTarget mode,
                     MixerConflictPolicy policy,
                     const BString& fallbackDeviceName,
                     media_node& outNode,
                     media_input& outInput);

    void Release();
    bool RecoverFromBreadcrumb();

    // Pre-warm a physical output device so its DAC output thread is already
    // running before the first Play connects a BSoundPlayer to it. Starts the
    // node (and settles a genuine cold start, as _EnsureNodeRunning does), then
    // drops our roster reference — the node keeps running. Does NOT touch the
    // mixer or system routing: any conflict eviction still waits until
    // Acquire() at play time. Safe to call from a background thread; it shares
    // only fRunningNodeId (atomic) with the play-path Acquire().
    void Prime(const OutputBusInfo& target);

private:
    bool fAlteredSystem;
    MixerConflictPolicy fAlteredPolicy;
    
    // For Disconnect policy restore: every mixer->device connection we broke
    // (one per bus; Shared breaks the target bus, Exclusive breaks them all).
    std::vector<media_output> fSavedMixerOutputs;
    
    // For SwitchToDevice policy restore
    media_input fOriginalSystemInput;
    bool fHasOriginalSystemInput;
    
    media_node fAcquiredNode;
    bool fHasAcquiredNode;

    // Id of the physical output node we have already cold-started (and left
    // running) this session. Persists across Acquire/Release cycles because
    // Release() deliberately leaves the node running. Used by
    // _EnsureNodeRunning to settle only on a genuine cold start. -1 = none.
    // Atomic: written by Prime() on a background thread, read/written by
    // _EnsureNodeRunning on the play path.
    std::atomic<media_node_id> fRunningNodeId;

    // Start the physical output node so its DAC output thread is running.
    // BSoundPlayer starts only its own producer, never the consumer device
    // node we hand it, so a direct connection is silent until something starts
    // the node. Idempotent — safe when the node is already running.
    void _EnsureNodeRunning(const media_node& node);

    status_t _WriteBreadcrumb(const BString& originalDeviceName, const BString& originalInputName, MixerConflictPolicy policy);
    void _ClearBreadcrumb();
    BString _GetBreadcrumbPath();
};

#endif // ENABLE_LOCAL_OUTPUT

#endif // AUDIO_OUTPUT_MANAGER_H

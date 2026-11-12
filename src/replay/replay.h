// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Record/replay support: what V8 needs to run under Replay's driver when
// recording and under its engine when replaying. Ported from Replay's forks of
// V8 9.4 (node/deps/v8) and 10.8 (chromium/v8). Unlike those, nothing is
// exposed through V8's public headers, and an embedder doesn't call anything:
// V8::Initialize attaches when the process was started to record or replay.

#ifndef V8_REPLAY_REPLAY_H_
#define V8_REPLAY_REPLAY_H_

#include <stddef.h>
#include <stdint.h>

#include "include/v8-local-handle.h"
#include "include/v8-value.h"
#include "src/base/macros.h"

namespace v8 {
namespace recordreplay {

V8_EXPORT_PRIVATE extern bool gIsRecordingOrReplaying;

inline bool IsRecordingOrReplaying() { return gIsRecordingOrReplaying; }
V8_EXPORT_PRIVATE bool IsRecording();
V8_EXPORT_PRIVATE bool IsReplaying();

// Tells the driver the process is busy where nothing else would: a replay
// that goes long without progress is taken for one that's hung.
V8_EXPORT_PRIVATE void NotifyActivity();

// Whether Replay's own scripts are what's running, on the main thread. What
// they compile isn't instrumented, as it isn't the program's.
V8_EXPORT_PRIVATE void EnterReplayCode();
V8_EXPORT_PRIVATE void ExitReplayCode();
V8_EXPORT_PRIVATE bool IsInReplayCode();

// The thread V8 was initialized on, which is the only one recorded in detail.
V8_EXPORT_PRIVATE bool IsMainThread();

V8_EXPORT_PRIVATE bool HasDivergedFromRecording();
V8_EXPORT_PRIVATE bool AreEventsDisallowed();
V8_EXPORT_PRIVATE bool AreEventsPassedThrough();
V8_EXPORT_PRIVATE void BeginDisallowEvents(const char* label);
V8_EXPORT_PRIVATE void EndDisallowEvents();

// A lock threads take in the same order when replaying as when recording.
// Zero, which locks nothing, in a process that's neither.
V8_EXPORT_PRIVATE int CreateOrderedLock(const char* name);
V8_EXPORT_PRIVATE void OrderedLock(int lock);
V8_EXPORT_PRIVATE void OrderedUnlock(int lock);

// A value or bytes that are the same when replaying as when recording.
V8_EXPORT_PRIVATE uintptr_t RecordReplayValue(const char* why, uintptr_t value);
V8_EXPORT_PRIVATE void RecordReplayBytes(const char* why, void* buf,
                                         size_t size);

V8_EXPORT_PRIVATE void Print(const char* format, ...);
V8_EXPORT_PRIVATE void Diagnostic(const char* format, ...);

// Marks this point of the recording with something of |kind|. The point has to
// be the same on both sides; |contents| is only looked at when replaying.
V8_EXPORT_PRIVATE void OnAnnotation(const char* kind, const char* contents);

// An inspector session a replay made for its own commands. It isn't there
// while recording, so nothing it does is part of a recording. Main thread.
V8_EXPORT_PRIVATE void AddReplayOwnedInspectorSession(const void* session);
V8_EXPORT_PRIVATE bool IsReplayOwnedInspectorSession(const void* session);

// The exception that's unwinding, while the driver is told about it.
V8_EXPORT_PRIVATE bool GetCurrentException(v8::Local<v8::Value>* exception);
// What the top frame is returning, while the driver is told it's returning.
V8_EXPORT_PRIVATE bool GetCurrentReturnValue(v8::Local<v8::Value>* value);

// Attaches to the driver if the process was started to record or replay, which
// has to happen before V8's flags are frozen.
void MaybeAttach();

}  // namespace recordreplay

namespace replayio {

// Calls to the system on this thread aren't part of the recording while this is
// alive. They run for real on both sides. This is for code that doesn't run at
// the same points when replaying as when recording, like the GC.
class V8_NODISCARD AutoDisallowEvents {
 public:
  explicit AutoDisallowEvents(const char* label = nullptr) {
    if (recordreplay::IsRecordingOrReplaying()) {
      active_ = true;
      recordreplay::BeginDisallowEvents(label);
    }
  }
  ~AutoDisallowEvents() {
    if (active_) recordreplay::EndDisallowEvents();
  }
  AutoDisallowEvents(const AutoDisallowEvents&) = delete;
  AutoDisallowEvents& operator=(const AutoDisallowEvents&) = delete;

 private:
  bool active_ = false;
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_REPLAY_H_

// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_REPLAY_GC_POLL_H_
#define V8_REPLAY_GC_POLL_H_

namespace v8 {
namespace internal {
class Isolate;
}  // namespace internal

namespace replayio {

// The GC runs at points which do not replay, so what it did to tracked
// FinalizationRegistries and WeakRefs, and to the embedder's objects (see
// ReplayGCPollParticipant below), is recorded/replayed here
// instead, with a single value when there is nothing to describe. See
// src/replay/finalization-registry.h and src/replay/weak-refs.h.
// The embedder's weak callbacks only note what the GC collected. When
// recording, the poll records whether an active participant has notes. Where
// one did, the poll calls Poll() on every active participant on both sides,
// which records/replays the notes and runs the cleanup.
class ReplayGCPollParticipant {
 public:
  virtual ~ReplayGCPollParticipant() = default;

  // Whether the poll records a value on this participant's behalf. Has to be
  // the same when recording and replaying.
  virtual bool IsActive() const = 0;
  // Only called when recording: whether anything was noted since the last
  // Poll().
  virtual bool HasPending() const = 0;
  // Called on every active participant, in the order they were added, after
  // V8's own work. Must not add or remove participants.
  virtual void Poll() = 0;
};

class ReplayGCPoll {
 public:
  // Called at the end of every microtask checkpoint, when the embedder calls
  // Isolate::ClearKeptObjects(), and when the cleanup task for tracked
  // registries is done.
  static void Poll(internal::Isolate* isolate);
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_GC_POLL_H_

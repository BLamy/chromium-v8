// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/arguments-inl.h"
#include "src/objects/js-weak-refs-inl.h"
#include "src/replay/finalization-registry.h"
#include "src/replay/replay.h"
#include "src/replay/weak-refs.h"
#include "src/runtime/runtime-utils.h"

namespace v8 {
namespace internal {

RUNTIME_FUNCTION(
    Runtime_JSFinalizationRegistryRegisterWeakCellWithUnregisterToken) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  DirectHandle<JSFinalizationRegistry> finalization_registry =
      args.at<JSFinalizationRegistry>(0);
  DirectHandle<WeakCell> weak_cell = args.at<WeakCell>(1);

  JSFinalizationRegistry::RegisterWeakCellWithUnregisterToken(
      finalization_registry, weak_cell, isolate);

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_JSWeakRefAddToKeptObjects) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<HeapObject> object = args.at<HeapObject>(0);
  DCHECK(Object::CanBeHeldWeakly(*object));

  isolate->heap()->KeepDuringJob(object);

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_RecordReplayWeakRefConstruct) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSWeakRef> weak_ref = args.at<JSWeakRef>(0);
  weak_ref->set_record_replay_id(0);
  if (recordreplay::IsRecordingOrReplaying()) {
    replayio::ReplayWeakRefs::OnConstruct(isolate, weak_ref);
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

// Called from WeakRef.prototype.deref(). Records/replays target liveness so
// the result is deterministic. When replaying, the GC keeps the target alive
// (see MarkingVisitorBase::VisitJSWeakRef) until it is observed dead here or
// ReplayGCPoll::Poll replays the recording's GC clearing it.
RUNTIME_FUNCTION(Runtime_RecordReplayWeakRefDeref) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSWeakRef> weak_ref = args.at<JSWeakRef>(0);
  DirectHandle<Object> target(weak_ref->target(), isolate);
  // The driver passes the value through where events are unavailable, e.g.
  // while evaluating in a replay pause, warning on every call.
  if (!recordreplay::IsRecordingOrReplaying() ||
      !recordreplay::AreEventsAvailable()) {
    return *target;
  }

  const bool had_target = !IsUndefined(*target);
  uintptr_t alive = had_target ? 1 : 0;
  uintptr_t recorded_alive =
      recordreplay::RecordReplayValue("JSWeakRef.deref", alive);
  // The replay's GC keeps targets alive, so it cannot be ahead of the
  // recording.
  CHECK_WITH_MSG(alive || !recorded_alive,
                 "WeakRef target collected before the recording collected it");

  if (!recorded_alive && alive) {
    // The target is still alive but at recording time it was dead.
    DCHECK(recordreplay::IsReplaying());
    weak_ref->set_target(ReadOnlyRoots(isolate).undefined_value());
    return ReadOnlyRoots(isolate).undefined_value();
  }
  return *target;
}

RUNTIME_FUNCTION(Runtime_RecordReplayFinalizationRegistryConstruct) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSFinalizationRegistry> registry =
      args.at<JSFinalizationRegistry>(0);
  registry->set_record_replay_id(0);
  if (recordreplay::IsRecordingOrReplaying()) {
    replayio::ReplayFinalizationRegistries::OnConstruct(isolate, registry);
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_RecordReplayFinalizationRegistryRegister) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  if (recordreplay::IsRecordingOrReplaying()) {
    replayio::ReplayFinalizationRegistries::OnRegister(
        isolate, args.at<JSFinalizationRegistry>(0), args.at<WeakCell>(1));
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

}  // namespace internal
}  // namespace v8

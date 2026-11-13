// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// From Replay's fork of V8 10.8 (chromium/v8), with this V8's handle and
// tagged types.

#include "src/replay/weak-refs.h"

#include <vector>

#include "src/execution/isolate.h"
#include "src/handles/global-handles-inl.h"
#include "src/heap/factory.h"
#include "src/objects/fixed-array-inl.h"
#include "src/objects/js-weak-refs-inl.h"
#include "src/replay/replay-isolate-data.h"
#include "src/replay/replay.h"

namespace v8 {
namespace replayio {

namespace i = internal;

namespace {

bool Enabled() { return recordreplay::FeatureEnabled("weak-ref-collection"); }

}  // namespace

void ReplayWeakRefs::OnConstruct(i::Isolate* isolate,
                                 i::DirectHandle<i::JSWeakRef> weak_ref) {
  if (!Enabled() || !recordreplay::AreEventsAvailable()) return;

  ReplayIsolateData* data = isolate->EnsureReplayData();
  int id = data->NewWeakRefId();
  recordreplay::Assert("WeakRef.construct %d", id);
  weak_ref->set_record_replay_id(id);

  if (!recordreplay::IsReplaying()) return;

  // The ids are consecutive, so the WeakRef with a given id is at index
  // id - 1. The list holds them weakly. It is allocated in old space, as a
  // global handle whose object is replaced is not tracked for scavenges.
  i::Handle<i::WeakArrayList> weak_refs;
  i::Address* location = data->tracked_weak_refs_location();
  if (location) {
    weak_refs = i::Handle<i::WeakArrayList>(location);
  } else {
    weak_refs = isolate->factory()->empty_weak_array_list();
  }
  CHECK_EQ(weak_refs->length().value(), static_cast<uint32_t>(id - 1));
  weak_refs = i::WeakArrayList::EnsureSpace(isolate, weak_refs, id,
                                            i::AllocationType::kOld);
  weak_refs->Set(id - 1, i::MakeWeak(*weak_ref));
  weak_refs->set_length(id);
  if (location) {
    *location = weak_refs->ptr();
  } else {
    data->set_tracked_weak_refs_location(
        isolate->global_handles()->Create(*weak_refs).location());
  }
}

void ReplayWeakRefs::OnTargetCleared(i::Isolate* isolate,
                                     i::Tagged<i::JSWeakRef> weak_ref) {
  // Runs inside the GC, so this only notes the id for the next poll.
  int id = weak_ref->record_replay_id();
  if (!id) return;
  ReplayIsolateData* data = isolate->replay_data();
  if (!data) return;
  data->cleared_weak_refs().push_back(id);
}

void ReplayWeakRefs::TakeCleared(i::Isolate* isolate, std::vector<int>* ids) {
  ids->swap(isolate->replay_data()->cleared_weak_refs());
}

void ReplayWeakRefs::ClearTargets(i::Isolate* isolate,
                                  const std::vector<int>& ids) {
  i::Address* location = isolate->replay_data()->tracked_weak_refs_location();
  CHECK_WITH_MSG(location, "Recorded cleared WeakRefs without a tracked one");
  i::Tagged<i::WeakArrayList> weak_refs =
      i::Cast<i::WeakArrayList>(i::Tagged<i::Object>(*location));
  for (int id : ids) {
    i::Tagged<i::HeapObject> weak_ref;
    // The replay's GC may have collected the WeakRef itself already.
    if (weak_refs->Get(id - 1).GetHeapObjectIfWeak(&weak_ref)) {
      i::Cast<i::JSWeakRef>(weak_ref)->set_target(
          i::ReadOnlyRoots(isolate).undefined_value());
    }
  }
}

}  // namespace replayio
}  // namespace v8

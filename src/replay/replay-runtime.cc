// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// What the record/replay bytecodes call. The progress counter advances at every
// function entry, loop iteration, catch and generator resume, on both sides, and
// is how a point in a recording is named. Instrumentation sites do nothing unless
// the engine is looking for one, which only happens when replaying.

#include <sstream>
#include <vector>

#include "src/api/api-inl.h"
#include "src/execution/arguments-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/objects/shared-function-info-inl.h"
#include "src/replay/replay-internal.h"
#include "src/runtime/runtime-utils.h"

namespace v8 {
namespace internal {

namespace {

// Site indexes in bytecode are offset by this, which makes them always take four
// bytes, so bytecode offsets are stable whatever the index is.
constexpr int kBytecodeSiteOffset = 1 << 16;

struct InstrumentationSite {
  const char* kind;
  int source_position;
  // The index of this site within its function.
  int function_index;
  // Set on the first use of the site.
  std::string function_id;
};

// Main thread only.
std::vector<InstrumentationSite>* gInstrumentationSites;

InstrumentationSite& GetInstrumentationSite(int index) {
  CHECK(recordreplay::IsMainThread());
  CHECK_NOT_NULL(gInstrumentationSites);
  index -= kBytecodeSiteOffset;
  CHECK_LT(static_cast<size_t>(index), gInstrumentationSites->size());
  return (*gInstrumentationSites)[index];
}

void OnExecutionProgress() {
  if (++*gProgressCounter == gTargetProgress) {
    RecordReplayOnTargetProgressReached();
  }
}

void OnInstrumentation(Tagged<JSFunction> function, int index) {
  if (!gRecordReplayInstrumentationEnabled) return;
  // Reading values at a pause can call back into instrumented code, like a
  // getter, and that isn't part of the recording.
  if (recordreplay::AreEventsDisallowed()) return;
  InstrumentationSite& site = GetInstrumentationSite(index);
  if (site.function_id.empty()) {
    site.function_id = GetRecordReplayFunctionId(function->shared());
  }
  RecordReplayInstrument(site.kind, site.function_id.c_str(),
                         site.function_index);
}

// The progress when an exception last started unwinding.
uint64_t gLastExceptionUnwindProgress;
// The exception that's unwinding, for commands to read.
DirectHandle<Object>* gCurrentException;

}  // namespace

int RegisterInstrumentationSite(const char* kind, int source_position,
                                int function_index) {
  CHECK(recordreplay::IsMainThread());
  if (!gInstrumentationSites) {
    gInstrumentationSites = new std::vector<InstrumentationSite>();
  }
  int index = static_cast<int>(gInstrumentationSites->size());
  gInstrumentationSites->push_back(
      {kind, source_position, function_index, std::string()});
  return index + kBytecodeSiteOffset;
}

const char* InstrumentationSiteKind(int index) {
  return GetInstrumentationSite(index).kind;
}

int InstrumentationSiteSourcePosition(int index) {
  return GetInstrumentationSite(index).source_position;
}

int InstrumentationSiteFunctionIndex(int index) {
  return GetInstrumentationSite(index).function_index;
}

std::string GetRecordReplayFunctionId(Tagged<SharedFunctionInfo> shared) {
  // An ID that can be parsed back to a script and source location.
  std::ostringstream os;
  os << Cast<Script>(shared->script())->id() << ":" << shared->StartPosition();
  return os.str();
}

void ParseRecordReplayFunctionId(const std::string& function_id, int* script_id,
                                 int* source_position) {
  const char* raw = function_id.c_str();
  *script_id = atoi(raw);
  const char* colon = strchr(raw, ':');
  *source_position = colon ? atoi(colon + 1) : 0;
}

// An exception is starting to unwind, or one that's unwinding has left a frame.
// The progress counter advances at every catch and finally, so it's unchanged
// when this is the same exception leaving another frame, which the driver doesn't
// need to know about.
void RecordReplayOnExceptionUnwind(Isolate* isolate) {
  if (!recordreplay::IsRecordingOrReplaying() ||
      !recordreplay::IsMainThread() || recordreplay::AreEventsDisallowed() ||
      *gProgressCounter == gLastExceptionUnwindProgress) {
    return;
  }
  if (!recordreplay::HasDivergedFromRecording() && isolate->has_exception()) {
    HandleScope scope(isolate);
    DirectHandle<Object> exception(isolate->exception(), isolate);
    if (isolate->is_catchable_by_javascript(*exception)) {
      // The driver can run commands here, which need a clean slate.
      isolate->clear_exception();
      DirectHandle<Object> message(isolate->pending_message(), isolate);
      isolate->clear_pending_message();
      gCurrentException = &exception;
      RecordReplayOnExceptionUnwindRaw();
      gCurrentException = nullptr;
      CHECK(!isolate->has_exception());
      isolate->set_exception(*exception);
      isolate->set_pending_message(*message);
    }
  }
  // After the call, which advances the counter.
  gLastExceptionUnwindProgress = *gProgressCounter;
}

RUNTIME_FUNCTION(Runtime_RecordReplayIncExecutionProgressCounter) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  OnExecutionProgress();
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_RecordReplayInstrumentation) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(2, args.length());
  OnInstrumentation(Cast<JSFunction>(args[0]), args.smi_value_at(1));
  return ReadOnlyRoots(isolate).undefined_value();
}

}  // namespace internal

bool recordreplay::GetCurrentException(v8::Local<v8::Value>* exception) {
  if (!internal::gCurrentException) return false;
  *exception = Utils::ToLocal(*internal::gCurrentException);
  return true;
}

}  // namespace v8

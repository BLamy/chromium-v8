// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The connection to Replay's driver when recording and its engine when
// replaying. Both are reached the same way: dlopen and dlsym are the engine's
// when replaying, which answers these names itself.

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/flags/flags.h"
#include "src/replay/replay-internal.h"

namespace v8 {

namespace {

void (*gOnNewSource)(const char* id, const char* kind, const char* url);
void (*gOnConsoleMessage)(size_t bookmark);
void (*gOnExceptionUnwind)();
void (*gPrint)(const char* format, va_list args);
void (*gDiagnostic)(const char* format, va_list args);
void (*gOnInstrument)(const char* kind, const char* function,
                      int function_index);
void (*gAddPossibleBreakpoint)(int line, int column, const char* function,
                               int function_index);
void (*gBytes)(const char* why, void* buf, size_t size);
uintptr_t (*gValue)(const char* why, uintptr_t value);
bool (*gAreEventsDisallowed)();
bool (*gAreEventsPassedThrough)();
void (*gProgressReached)();
void (*gBeginDisallowEvents)();
void (*gBeginDisallowEventsWithLabel)(const char* label);
void (*gEndDisallowEvents)();
bool (*gIsReplaying)();
bool (*gHasDivergedFromRecording)();
const char* (*gReplaceSourceContents)(const char* contents);
size_t (*gNewBookmark)();
void (*gOnAnnotation)(const char* kind, const char* contents);

pthread_t gMainThread;

template <typename T>
void LoadSymbol(void* handle, const char* name, T& function) {
  void* symbol = dlsym(handle, name);
  if (!symbol) {
    fprintf(stderr, "Could not find %s in the Record Replay driver\n", name);
    abort();
  }
  function = reinterpret_cast<T>(symbol);
}

// What running under the driver needs. All of it has to be the same when
// recording and replaying, as it's the same bytecode and heap on both sides
// that makes a replay reach the same points.
void ApplyFlagOverrides() {
  using internal::v8_flags;

  // JS runs in the interpreter, whose bytecode has the instrumentation, and
  // nothing runs on another thread. The driver parks V8's worker threads.
  v8_flags.single_threaded = true;
  v8_flags.sparkplug = false;
  v8_flags.always_sparkplug = false;
  v8_flags.maglev = false;
  v8_flags.turbofan = false;

  // Bytecode is what a replay is addressed by, so it stays the same once made.
  v8_flags.flush_bytecode = false;
  v8_flags.flush_baseline_code = false;
  v8_flags.stress_flush_code = false;
  v8_flags.enable_lazy_source_positions = false;

  // These act at times that depend on the GC or on timers.
  v8_flags.compilation_cache = false;
  v8_flags.incremental_marking = false;
  v8_flags.incremental_marking_task = false;
  v8_flags.minor_gc_task = false;
  v8_flags.memory_reducer = false;

  // WeakRefs and FinalizationRegistries are strong, as when their targets die is
  // visible to the program and depends on when the GC runs. See VisitJSWeakRef.
  v8_flags.handle_weak_ref_weakly_in_minor_gc = false;

  // Async stack traces depend on how code was compiled.
  v8_flags.async_stack_traces = false;

  // Whether a reservation near the binary succeeds depends on where the
  // system puts things, which isn't the same on a replay. When it failed
  // there and not when recording, the fallback's static was initialized on
  // one side only.
  v8_flags.better_code_range_allocation = false;
}

void SetRecordingOrReplaying(void* handle) {
  LoadSymbol(handle, "RecordReplayOnNewSource", gOnNewSource);
  LoadSymbol(handle, "RecordReplayOnConsoleMessage", gOnConsoleMessage);
  LoadSymbol(handle, "RecordReplayOnExceptionUnwind", gOnExceptionUnwind);
  LoadSymbol(handle, "RecordReplayPrint", gPrint);
  LoadSymbol(handle, "RecordReplayDiagnostic", gDiagnostic);
  LoadSymbol(handle, "RecordReplayOnInstrument", gOnInstrument);
  LoadSymbol(handle, "RecordReplayAddPossibleBreakpoint",
             gAddPossibleBreakpoint);
  LoadSymbol(handle, "RecordReplayBytes", gBytes);
  LoadSymbol(handle, "RecordReplayValue", gValue);
  LoadSymbol(handle, "RecordReplayAreEventsDisallowed", gAreEventsDisallowed);
  LoadSymbol(handle, "RecordReplayAreEventsPassedThrough",
             gAreEventsPassedThrough);
  LoadSymbol(handle, "RecordReplayProgressReached", gProgressReached);
  LoadSymbol(handle, "RecordReplayBeginDisallowEvents", gBeginDisallowEvents);
  LoadSymbol(handle, "RecordReplayBeginDisallowEventsWithLabel",
             gBeginDisallowEventsWithLabel);
  LoadSymbol(handle, "RecordReplayEndDisallowEvents", gEndDisallowEvents);
  LoadSymbol(handle, "RecordReplayIsReplaying", gIsReplaying);
  LoadSymbol(handle, "RecordReplayHasDivergedFromRecording",
             gHasDivergedFromRecording);

  // Not every driver has this one.
  gReplaceSourceContents = reinterpret_cast<const char* (*)(const char*)>(
      dlsym(handle, "RecordReplayReplaceSourceContents"));

  LoadSymbol(handle, "RecordReplayNewBookmark", gNewBookmark);
  LoadSymbol(handle, "RecordReplayOnAnnotation", gOnAnnotation);

  gMainThread = pthread_self();
  recordreplay::gIsRecordingOrReplaying = true;

  void (*set_default_command_callback)(char* (*)(const char*, const char*));
  LoadSymbol(handle, "RecordReplaySetDefaultCommandCallback",
             set_default_command_callback);
  set_default_command_callback(internal::CommandCallback);

  void (*set_possible_breakpoints_callback)(void (*)(const char*));
  LoadSymbol(handle, "RecordReplaySetPossibleBreakpointsCallback",
             set_possible_breakpoints_callback);
  set_possible_breakpoints_callback(internal::PossibleBreakpointsCallback);

  void (*set_clear_pause_data_callback)(void (*)());
  LoadSymbol(handle, "RecordReplaySetClearPauseDataCallback",
             set_clear_pause_data_callback);
  set_clear_pause_data_callback(internal::ClearPauseDataCallback);

  uint64_t* (*get_progress_counter)();
  LoadSymbol(handle, "RecordReplayProgressCounter", get_progress_counter);
  internal::gProgressCounter = get_progress_counter();

  void (*set_change_instrument_callback)(void (*)(bool));
  LoadSymbol(handle, "RecordReplaySetChangeInstrumentCallback",
             set_change_instrument_callback);
  set_change_instrument_callback([](bool enabled) {
    internal::gRecordReplayInstrumentationEnabled = enabled;
  });

  void (*set_progress_callback)(void (*)(uint64_t));
  LoadSymbol(handle, "RecordReplaySetProgressCallback", set_progress_callback);
  set_progress_callback(
      [](uint64_t progress) { internal::gTargetProgress = progress; });

  void (*set_progress_interrupt_callback)(void (*)());
  LoadSymbol(handle, "RecordReplaySetProgressInterruptCallback",
             set_progress_interrupt_callback);
  set_progress_interrupt_callback([]() {});

  // Checkpoints every so much JS, which the engine and timeline both use.
  void (*enable_progress_checkpoints)();
  LoadSymbol(handle, "RecordReplayEnableProgressCheckpoints",
             enable_progress_checkpoints);
  enable_progress_checkpoints();

  ApplyFlagOverrides();

  // What the engine has to do starts at the first checkpoint, including telling
  // the callbacks above that it wants instrumentation, so they have to be set by
  // then. Nothing's addressed by progress before it, as no JS has run.
  void (*new_checkpoint)();
  LoadSymbol(handle, "RecordReplayNewCheckpoint", new_checkpoint);
  new_checkpoint();
}

}  // namespace

namespace recordreplay {

bool gIsRecordingOrReplaying = false;

bool IsRecording() { return IsRecordingOrReplaying() && !IsReplaying(); }

bool IsReplaying() { return IsRecordingOrReplaying() && gIsReplaying(); }

bool IsMainThread() {
  return IsRecordingOrReplaying() && pthread_equal(gMainThread, pthread_self());
}

bool HasDivergedFromRecording() {
  return IsRecordingOrReplaying() && gHasDivergedFromRecording();
}

bool AreEventsDisallowed() {
  return IsRecordingOrReplaying() && gAreEventsDisallowed();
}

bool AreEventsPassedThrough() {
  return IsRecordingOrReplaying() && gAreEventsPassedThrough();
}

void BeginDisallowEvents(const char* label) {
  if (label) {
    gBeginDisallowEventsWithLabel(label);
  } else {
    gBeginDisallowEvents();
  }
}

void EndDisallowEvents() { gEndDisallowEvents(); }

uintptr_t RecordReplayValue(const char* why, uintptr_t value) {
  return IsRecordingOrReplaying() ? gValue(why, value) : value;
}

void RecordReplayBytes(const char* why, void* buf, size_t size) {
  if (IsRecordingOrReplaying()) gBytes(why, buf, size);
}

void OnAnnotation(const char* kind, const char* contents) {
  if (!IsRecordingOrReplaying()) return;
  gOnAnnotation(kind, contents);
}

void Print(const char* format, ...) {
  if (!IsRecordingOrReplaying()) return;
  va_list args;
  va_start(args, format);
  gPrint(format, args);
  va_end(args);
}

void Diagnostic(const char* format, ...) {
  if (!IsRecordingOrReplaying()) return;
  va_list args;
  va_start(args, format);
  gDiagnostic(format, args);
  va_end(args);
}

void MaybeAttach() {
  if (gIsRecordingOrReplaying) return;
  if (getenv("RECORD_REPLAY_DONT_RECORD")) return;
  // The driver has been loaded by whatever started this process when recording,
  // and refuses to be loaded again. When replaying, dlsym is the engine's, which
  // answers for itself. In a process that's doing neither, this finds nothing.
  if (!dlsym(RTLD_DEFAULT, "RecordReplayAttach")) return;
  // Read before anything is recorded, where it's the real environment.
  internal::gWarnMissingDependencyExecution =
      !!getenv("RECORD_REPLAY_WARN_MISSING_EXECUTION");
  SetRecordingOrReplaying(RTLD_DEFAULT);
}

}  // namespace recordreplay

namespace internal {

uint64_t* gProgressCounter;
uint64_t gTargetProgress;
bool gRecordReplayInstrumentationEnabled;

void RecordReplayOnNewSource(const char* id, const char* kind,
                             const char* url) {
  gOnNewSource(id, kind, url);
}

const char* RecordReplayReplaceSourceContents(const char* contents) {
  return gReplaceSourceContents ? gReplaceSourceContents(contents) : nullptr;
}

size_t RecordReplayNewBookmark() { return gNewBookmark(); }

void RecordReplayOnConsoleMessage(size_t bookmark) {
  gOnConsoleMessage(bookmark);
}

void RecordReplayOnExceptionUnwindRaw() { gOnExceptionUnwind(); }

void RecordReplayOnTargetProgressReached() {
  // Nothing is at a point the recording knows about while events are disallowed.
  if (recordreplay::AreEventsDisallowed()) return;
  gProgressReached();
}

void RecordReplayInstrument(const char* kind, const char* function,
                            int function_index) {
  gOnInstrument(kind, function, function_index);
}

void RecordReplayAddPossibleBreakpoint(int line, int column,
                                       const char* function,
                                       int function_index) {
  gAddPossibleBreakpoint(line, column, function, function_index);
}

}  // namespace internal
}  // namespace v8

// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_REPLAY_REPLAY_INTERNAL_H_
#define V8_REPLAY_REPLAY_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "src/handles/handles.h"
#include "src/replay/replay.h"

namespace v8 {
namespace internal {

class Isolate;
class Script;
class SharedFunctionInfo;

// replay-driver.cc
extern uint64_t* gProgressCounter;
extern uint64_t gTargetProgress;
extern bool gRecordReplayInstrumentationEnabled;
void RecordReplayOnNewSource(const char* id, const char* kind, const char* url);
void RecordReplayOnConsoleMessage(size_t bookmark);
void RecordReplayOnExceptionUnwindRaw();
void RecordReplayOnTargetProgressReached();
void RecordReplayInstrument(const char* kind, const char* function,
                            int function_index);
void RecordReplayAddPossibleBreakpoint(int line, int column,
                                       const char* function,
                                       int function_index);

// replay-runtime.cc
V8_EXPORT_PRIVATE int RegisterInstrumentationSite(const char* kind,
                                                  int source_position,
                                                  int function_index);
const char* InstrumentationSiteKind(int index);
int InstrumentationSiteSourcePosition(int index);
int InstrumentationSiteFunctionIndex(int index);
std::string GetRecordReplayFunctionId(Tagged<SharedFunctionInfo> shared);
void ParseRecordReplayFunctionId(const std::string& function_id, int* script_id,
                                 int* source_position);
V8_EXPORT_PRIVATE void RecordReplayOnExceptionUnwind(Isolate* isolate);

// replay-debug.cc
V8_EXPORT_PRIVATE bool RecordReplayShouldEmitOpcodes(bool record_replay_ignore);
V8_EXPORT_PRIVATE bool RecordReplayIgnoreScriptByURL(const char* url);
V8_EXPORT_PRIVATE void RecordReplayRegisterScript(Isolate* isolate,
                                                  DirectHandle<Script> script);
V8_EXPORT_PRIVATE int RecordReplayNextScriptId();
// Scripts live for as long as their isolate, and can't be read after it's gone.
V8_EXPORT_PRIVATE void RecordReplayOnIsolateDispose(Isolate* isolate);
bool RecordReplayIsRegisteredScript(int script_id);
char* CommandCallback(const char* command, const char* params);
void ClearPauseDataCallback();
void PossibleBreakpointsCallback(const char* source_id);

// A console call that's being reported to the driver, whose arguments the
// current command can read.
V8_EXPORT_PRIVATE void RecordReplaySetCurrentConsoleCall(
    const char* level, void* args /* BuiltinArguments* */);
V8_EXPORT_PRIVATE void RecordReplayGetCurrentConsoleCall(const char** level,
                                                         void** args);

// The commands written in JS need the inspector, which not every binary with V8
// in it has (mksnapshot doesn't), so replay-glue.cc registers these.
struct RecordReplayGlue {
  std::string (*run_command)(Isolate* isolate, const char* command,
                             const char* params);
  void (*clear_pause_data)(Isolate* isolate);
  void (*on_isolate_dispose)(Isolate* isolate);
};
V8_EXPORT_PRIVATE void RecordReplaySetGlue(const RecordReplayGlue* glue);

}  // namespace internal
}  // namespace v8

#endif  // V8_REPLAY_REPLAY_INTERNAL_H_

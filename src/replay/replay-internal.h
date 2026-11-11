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
// A place in the recording that isn't a step of the program's: where an
// uncaught error is reported, which is after its frames have gone.
V8_EXPORT_PRIVATE size_t RecordReplayNewBookmark();
void RecordReplayOnExceptionUnwindRaw();
void RecordReplayOnTargetProgressReached();
void RecordReplayInstrument(const char* kind, const char* function,
                            int function_index);
void RecordReplayAddPossibleBreakpoint(int line, int column,
                                       const char* function,
                                       int function_index);
// What a replay wants a script's source to be instead, or null. Whoever is
// replaying can hand over a rewritten source for a script, to have its own
// calls made from inside the program's code.
V8_EXPORT_PRIVATE const char* RecordReplayReplaceSourceContents(
    const char* contents);

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

// What a debugger compiles for itself isn't the program's: an expression a
// DevTools client evaluates, or a function it calls on an object. A page that's
// recorded while something drives it over DevTools would otherwise have each
// of those as a source. They run on both sides all the same, as what DevTools
// said is in the recording.
class V8_EXPORT_PRIVATE V8_NODISCARD RecordReplayDebuggerCompileScope {
 public:
  RecordReplayDebuggerCompileScope();
  ~RecordReplayDebuggerCompileScope();
};
V8_EXPORT_PRIVATE bool RecordReplayIsDebuggerCompile();
// While set, the next script made gets this ID: a script compiled from
// replaced source stands in for the one the recording has.
V8_EXPORT_PRIVATE void RecordReplaySetReplacementScriptId(int script_id);
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
// An uncaught error the embedder is reporting to the driver, as JSON, or null.
V8_EXPORT_PRIVATE void RecordReplaySetCurrentPageError(const char* json);
V8_EXPORT_PRIVATE const char* RecordReplayCurrentPageError();

// The commands written in JS need the inspector, which not every binary with V8
// in it has (mksnapshot doesn't), so replay-glue.cc registers these.
struct RecordReplayGlue {
  std::string (*run_command)(Isolate* isolate, const char* command,
                             const char* params);
  void (*clear_pause_data)(Isolate* isolate);
  void (*on_isolate_dispose)(Isolate* isolate);
};
V8_EXPORT_PRIVATE void RecordReplaySetGlue(const RecordReplayGlue* glue);

// The isolate whose embedder answers commands in a page, once there is one.
V8_EXPORT_PRIVATE void RecordReplaySetPageCommandIsolate(Isolate* isolate);
// Target.evaluatePrivileged asked for before any page existed, as the params
// each came with, or null past the last. Whoever replays sets itself up at the
// first checkpoint, which is before V8 has an isolate. The embedder runs these
// in the first page.
V8_EXPORT_PRIVATE const char* RecordReplayPendingPrivilegedEvaluation(
    size_t index);

}  // namespace internal
}  // namespace v8

#endif  // V8_REPLAY_REPLAY_INTERNAL_H_

// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_REPLAY_REPLAY_INTERNAL_H_
#define V8_REPLAY_REPLAY_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "include/v8-promise.h"
#include "src/handles/handles.h"
#include "src/replay/replay.h"

namespace v8 {
namespace internal {

class AstRawString;

class Isolate;
class JSFunction;
class JSPromise;
class Object;
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
// Asks the driver, when recording on the main thread, to call back the next
// time the progress counter advances: a point a replay reaches too.
void RecordReplayTriggerProgressInterrupt();
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
// A bookmark for here, where a message is being made: for an uncaught error,
// that's where it's thrown, which is where the driver says the message is.
// 0 when there's none to have.
V8_EXPORT_PRIVATE int RecordReplayNewMessageBookmark(Isolate* isolate);

// An ID for an object, the same for as long as the object lives and, for one
// that's given it where it's made, the same in every replay.
int RecordReplayObjectId(v8::Isolate* isolate, v8::Local<v8::Value> value,
                         bool allow_create);
// Whether an object that has this property assigned, as |this| or in a
// literal, gets its ID there. A replay does that for what might be a React
// fiber, which has an "alternate", and for everything when the driver asks.
bool RecordReplayTrackThisObjectAssignment(const AstRawString* property);
void RecordReplayTrackObjectsCallback(bool track_objects);
// How many instrumented JS frames are on the main thread's stack. A recording
// and its replays use different amounts of native stack for the same JS, so
// the stack overflows at a depth instead: kRecordReplayMaxJsFrameDepth.
extern int gRecordReplayJsFrameDepth;
constexpr int kRecordReplayMaxJsFrameDepth = 1024;
// Counts the frames again where an exception is caught, as the frames it
// unwound through never said they'd gone.
void RecordReplaySyncJsFrameDepth(Isolate* isolate, int frames_above_handler);

// The ID of the generator whose frame is being announced, or 0.
int RecordReplayCurrentGeneratorId();

// Promises are part of the dependency graph a replay builds, which takes a
// promise hook. The hook is there while recording too, so that promises take
// the same paths on both sides, and does nothing then.
bool RecordReplayShouldCallOnPromiseHook();
void RecordReplayOnPromiseHook(Isolate* isolate, v8::PromiseHookType type,
                               DirectHandle<JSPromise> promise,
                               DirectHandle<Object> parent);
// |promise| was resolved with |resolution|, a thenable, and settles when that
// does.
void RecordReplayOnPromiseAdoption(Isolate* isolate,
                                   DirectHandle<JSPromise> promise,
                                   DirectHandle<Object> resolution);

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

// Called for each of the program's scripts that names a source map, as the
// script is registered. It mustn't run JS: the script is still being compiled.
using RecordReplayNewScriptCallback = void (*)(v8::Isolate* isolate,
                                               int script_id,
                                               const char* url,
                                               const char* source_map_url);
V8_EXPORT_PRIVATE void RecordReplaySetNewScriptCallback(
    RecordReplayNewScriptCallback callback);
V8_EXPORT_PRIVATE bool RecordReplayScriptSource(int script_id,
                                                std::string* source);

// Answers a command the embedder registered for, if it is one.
bool RecordReplayEmbedderCommand(const char* command, const char* params,
                                 std::string* result);

// Set when the process attaches, from RECORD_REPLAY_WARN_MISSING_EXECUTION.
extern bool gWarnMissingDependencyExecution;
void RecordReplayCheckDependencyExecution(Isolate* isolate,
                                          DirectHandle<JSFunction> function);

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

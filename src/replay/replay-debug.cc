// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Which scripts a recording knows about, and the protocol commands that can be
// answered without running JS, so that frequent ones touch nothing on the JS heap.

#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "include/v8-locker.h"
#include "src/api/api-inl.h"
#include "src/codegen/compiler.h"
#include "src/debug/debug.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/handles/global-handles-inl.h"
#include "src/interpreter/bytecode-array-iterator.h"
#include "src/objects/script-inl.h"
#include "src/objects/shared-function-info-inl.h"
#include "src/replay/replay-internal.h"

namespace v8 {
namespace internal {

namespace {

// Every isolate shares one list of scripts and one counter of script IDs, as the
// recording has one main thread and doesn't know about isolates. Scripts stay
// alive for as long as the process does, so that they can always be read.
struct RegisteredScript {
  Isolate* isolate;
  IndirectHandle<Script> script;
  IndirectHandle<Context> context;
};
std::unordered_map<int, RegisteredScript>* gScripts;

// Compiling on the main thread when recording and replaying uses this, which
// keeps IDs the same on both sides.
int gNextScriptId = 100000;
// A replay compiling something of its own, like an expression being evaluated,
// uses these instead, so that the IDs of everything later stay the same.
int gNextDivergentScriptId = 1 << 30;

const RecordReplayGlue* gGlue;

const char* gCurrentConsoleLevel;
void* gCurrentConsoleArgs;

// A little of JSON, for reading the flat parameters the engine sends. The values
// are numbers, or strings holding IDs which have nothing to escape.
const char* FindJsonValue(const char* json, const char* key) {
  std::string quoted = std::string("\"") + key + "\"";
  const char* found = strstr(json, quoted.c_str());
  if (!found) return nullptr;
  const char* value = found + quoted.size();
  while (*value == ' ' || *value == ':') value++;
  return value;
}

std::optional<std::string> JsonString(const char* json, const char* key) {
  const char* value = FindJsonValue(json, key);
  if (!value || *value != '"') return std::nullopt;
  const char* end = strchr(value + 1, '"');
  if (!end) return std::nullopt;
  return std::string(value + 1, end);
}

std::optional<int> JsonInt(const char* json, const char* key) {
  const char* value = FindJsonValue(json, key);
  if (!value || !(*value == '-' || (*value >= '0' && *value <= '9'))) {
    return std::nullopt;
  }
  return atoi(value);
}

// The text of an object value, which is enough for {line, column} and {sourceId}.
std::string JsonObjectText(const char* json, const char* key) {
  const char* value = FindJsonValue(json, key);
  if (!value || *value != '{') return std::string();
  const char* end = strchr(value, '}');
  return end ? std::string(value, end + 1) : std::string();
}

std::string JsonEscape(const char* text) {
  std::string out;
  for (const char* p = text; *p; p++) {
    unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

// Runs in a script's isolate, entering it if this is between turns, when there's
// no current isolate, or if a different isolate is paused.
class V8_NODISCARD ScriptScope {
 public:
  explicit ScriptScope(const RegisteredScript& entry)
      : api_isolate_(reinterpret_cast<v8::Isolate*>(entry.isolate)) {
    if (v8::Isolate::TryGetCurrent() != api_isolate_) {
      locker_.emplace(api_isolate_);
      isolate_scope_.emplace(api_isolate_);
    }
    handle_scope_.emplace(api_isolate_);
    if (!entry.context.is_null()) {
      save_context_.emplace(entry.isolate);
      entry.isolate->set_context(*entry.context);
    }
  }

 private:
  v8::Isolate* api_isolate_;
  std::optional<v8::Locker> locker_;
  std::optional<v8::Isolate::Scope> isolate_scope_;
  std::optional<v8::HandleScope> handle_scope_;
  std::optional<SaveContext> save_context_;
};

const RegisteredScript* FindScript(int script_id) {
  if (!gScripts) return nullptr;
  auto iter = gScripts->find(script_id);
  return iter == gScripts->end() ? nullptr : &iter->second;
}

const RegisteredScript* FindScript(const char* params) {
  std::optional<std::string> id = JsonString(params, "sourceId");
  return id ? FindScript(atoi(id->c_str())) : nullptr;
}

void GetSiteLocation(DirectHandle<Script> script, int site, int* line,
                     int* column) {
  Script::PositionInfo info;
  Script::GetPositionInfo(script, InstrumentationSiteSourcePosition(site),
                          &info);
  // Lines are 1-indexed in the protocol.
  *line = info.line + 1;
  *column = info.column;
}

// Calls back with every instrumentation site in a script. Compiles any functions
// that haven't been yet, which the replay can do at any time, as that isn't
// visible to the program. Based on Debug::GetPossibleBreakpoints.
template <typename Callback>
void ForEachInstrumentationSite(Isolate* isolate, Handle<Script> script,
                                Callback callback) {
  bool tried_top_level_compile = false;
  while (true) {
    HandleScope scope(isolate);
    // The GC collects the top level function once nothing references it, along
    // with the functions only its bytecode referenced. Compiling it recreates them.
    if (!tried_top_level_compile && script->infos()->ulength().value() > 0) {
      bool did_compile = false;
      isolate->debug()->GetTopLevelWithRecompile(script, &did_compile);
      tried_top_level_compile = true;
    }

    std::vector<Handle<SharedFunctionInfo>> candidates;
    std::vector<IsCompiledScope> compiled_scopes;
    {
      SharedFunctionInfo::ScriptIterator iterator(isolate, *script);
      for (Tagged<SharedFunctionInfo> info = iterator.Next(); !info.is_null();
           info = iterator.Next()) {
        if (!info->IsSubjectToDebugging()) continue;
        if (!info->is_compiled() && !info->allows_lazy_compilation()) continue;
        candidates.push_back(handle(info, isolate));
      }
    }

    bool was_compiled = false;
    for (const auto& candidate : candidates) {
      IsCompiledScope is_compiled_scope(candidate->is_compiled_scope(isolate));
      if (!is_compiled_scope.is_compiled()) {
        if (Compiler::Compile(isolate, candidate, Compiler::CLEAR_EXCEPTION,
                              &is_compiled_scope)) {
          was_compiled = true;
        } else {
          // A function that can't be compiled never ran, so has no sites.
          continue;
        }
      }
      compiled_scopes.push_back(is_compiled_scope);
    }
    // Compiling can reveal more functions.
    if (was_compiled) continue;

    for (const auto& candidate : candidates) {
      if (!candidate->HasBytecodeArray()) continue;
      Handle<BytecodeArray> bytecode(candidate->GetBytecodeArray(isolate),
                                     isolate);
      for (interpreter::BytecodeArrayIterator it(bytecode); !it.done();
           it.Advance()) {
        if (it.current_bytecode() ==
            interpreter::Bytecode::kRecordReplayInstrumentation) {
          callback(*candidate,
                   static_cast<int>(it.GetUnsignedImmediateOperand(0)));
        }
      }
    }
    return;
  }
}

// The breakpoint sites of scripts whose isolates have been disposed, on a replay.
struct RememberedBreakpoint {
  int line;
  int column;
  std::string function;
  int function_index;
};
std::unordered_map<int, std::vector<RememberedBreakpoint>>* gRememberedBreakpoints;

void RememberBreakpoints(int script_id, const RegisteredScript& entry);

// Sites of kind breakpoint within the begin and end in a command's parameters.
template <typename Callback>
void ForEachBreakpointInRange(const RegisteredScript& entry, const char* params,
                              Callback callback) {
  std::string begin = JsonObjectText(params, "begin");
  std::string end = JsonObjectText(params, "end");
  int begin_line = begin.empty() ? 1 : JsonInt(begin.c_str(), "line").value_or(1);
  int begin_column =
      begin.empty() ? 0 : JsonInt(begin.c_str(), "column").value_or(0);
  int end_line =
      end.empty() ? INT32_MAX : JsonInt(end.c_str(), "line").value_or(INT32_MAX);
  int end_column = end.empty()
                       ? INT32_MAX
                       : JsonInt(end.c_str(), "column").value_or(INT32_MAX);
  Handle<Script> script = entry.script;
  ForEachInstrumentationSite(
      entry.isolate, script,
      [&](Tagged<SharedFunctionInfo> shared, int site) {
        if (strcmp(InstrumentationSiteKind(site), "breakpoint")) return;
        int line, column;
        GetSiteLocation(script, site, &line, &column);
        if (line < begin_line || (line == begin_line && column < begin_column) ||
            line > end_line || (line == end_line && column > end_column)) {
          return;
        }
        callback(shared, site, line, column);
      });
}

// Every frame on the stack in instrumented code, top first. Other frames aren't
// visible to the engine, as they don't have instrumentation to tell it about them.
template <typename Callback>
void ForEachInstrumentedFrame(Isolate* isolate, Callback callback) {
  for (JavaScriptStackFrameIterator it(isolate); !it.done(); it.Advance()) {
    FrameSummaries summaries = it.frame()->Summarize();
    for (int i = summaries.size() - 1; i >= 0; i--) {
      if (!summaries.frames[i].IsJavaScript()) continue;
      Tagged<SharedFunctionInfo> shared =
          summaries.frames[i].AsJavaScript().function()->shared();
      Tagged<Object> script = shared->script();
      if (IsScript(script) && Cast<Script>(script)->record_replay_instrumented()) {
        callback(shared);
      }
    }
  }
}

std::string CommandGetSourceContents(const char* params) {
  const RegisteredScript* entry = FindScript(params);
  if (!entry) return "{\"error\":\"Unknown source\"}";
  ScriptScope scope(*entry);
  DirectHandle<Script> script = entry->script;

  // Lines are padded to where the script starts, for scripts that don't start on
  // the first line of something. This runs as sources appear while scanning, which
  // stays the same as the recording by not allocating on the JS heap, as finding
  // a position would by computing the script's line ends.
  std::string padded(script->line_offset(), '\n');
  std::unique_ptr<char[]> source = Cast<String>(script->source())->ToCString();
  padded += source.get();
  return "{\"contents\":\"" + JsonEscape(padded.c_str()) +
         "\",\"contentType\":\"text/javascript\"}";
}

std::string CommandGetPossibleBreakpoints(const char* params) {
  const RegisteredScript* entry = FindScript(params);
  if (!entry) return "{\"lineLocations\":[]}";
  ScriptScope scope(*entry);
  std::map<int, std::vector<int>> lines;
  ForEachBreakpointInRange(
      *entry, params, [&](Tagged<SharedFunctionInfo>, int, int line, int column) {
        lines[line].push_back(column);
      });
  std::ostringstream os;
  os << "{\"lineLocations\":[";
  bool first_line = true;
  for (const auto& [line, columns] : lines) {
    os << (first_line ? "" : ",") << "{\"line\":" << line << ",\"columns\":[";
    first_line = false;
    for (size_t i = 0; i < columns.size(); i++) {
      os << (i ? "," : "") << columns[i];
    }
    os << "]}";
  }
  os << "]}";
  return os.str();
}

std::string CommandConvertLocationToFunctionOffset(const char* params) {
  std::string location = JsonObjectText(params, "location");
  const RegisteredScript* entry = FindScript(location.c_str());
  std::optional<int> line = JsonInt(location.c_str(), "line");
  std::optional<int> column = JsonInt(location.c_str(), "column");
  if (!entry || !line || !column) return "{}";
  ScriptScope scope(*entry);
  std::string result = "{}";
  ForEachBreakpointInRange(
      *entry, "",
      [&](Tagged<SharedFunctionInfo> shared, int site, int site_line,
          int site_column) {
        if (site_line == *line && site_column == *column) {
          std::ostringstream os;
          os << "{\"functionId\":\"" << GetRecordReplayFunctionId(shared)
             << "\",\"offset\":" << InstrumentationSiteFunctionIndex(site)
             << "}";
          result = os.str();
        }
      });
  return result;
}

std::string CommandConvertFunctionOffsetToLocation(const char* params) {
  std::optional<std::string> function_id = JsonString(params, "functionId");
  if (!function_id) return "{}";
  int script_id, function_position;
  ParseRecordReplayFunctionId(*function_id, &script_id, &function_position);
  const RegisteredScript* entry = FindScript(script_id);
  if (!entry) return "{}";
  ScriptScope scope(*entry);
  Handle<Script> script = entry->script;

  int line = 0, column = 0;
  std::optional<int> offset = JsonInt(params, "offset");
  if (offset) {
    ForEachInstrumentationSite(
        entry->isolate, script,
        [&](Tagged<SharedFunctionInfo> shared, int site) {
          if (InstrumentationSiteFunctionIndex(site) == *offset &&
              GetRecordReplayFunctionId(shared) == *function_id) {
            GetSiteLocation(script, site, &line, &column);
          }
        });
  }
  if (!line) {
    // The function itself, with no offset or one that isn't known.
    Script::PositionInfo info;
    Script::GetPositionInfo(script, function_position, &info);
    line = info.line + 1;
    column = info.column;
  }
  std::ostringstream os;
  os << "{\"location\":{\"sourceId\":\"" << script_id << "\",\"line\":" << line
     << ",\"column\":" << column << "}}";
  return os.str();
}

std::string CommandGetFunctionsInRange(const char* params) {
  const RegisteredScript* entry = FindScript(params);
  if (!entry) return "{\"functions\":[]}";
  ScriptScope scope(*entry);
  std::map<std::string, bool> functions;
  ForEachBreakpointInRange(
      *entry, params, [&](Tagged<SharedFunctionInfo> shared, int, int, int) {
        functions[GetRecordReplayFunctionId(shared)] = true;
      });
  std::ostringstream os;
  os << "{\"functions\":[";
  bool first = true;
  for (const auto& [function_id, unused] : functions) {
    os << (first ? "" : ",") << "\"" << function_id << "\"";
    first = false;
  }
  os << "]}";
  return os.str();
}

std::string CommandCountStackFrames() {
  v8::Isolate* current = v8::Isolate::TryGetCurrent();
  size_t count = 0;
  if (current) {
    Isolate* isolate = reinterpret_cast<Isolate*>(current);
    HandleScope scope(isolate);
    ForEachInstrumentedFrame(isolate,
                             [&](Tagged<SharedFunctionInfo>) { count++; });
  }
  std::ostringstream os;
  os << "{\"count\":" << count << "}";
  return os.str();
}

std::string CommandGetStackFunctionIds() {
  std::ostringstream os;
  os << "{\"frameFunctions\":[";
  v8::Isolate* current = v8::Isolate::TryGetCurrent();
  if (current) {
    Isolate* isolate = reinterpret_cast<Isolate*>(current);
    HandleScope scope(isolate);
    bool first = true;
    ForEachInstrumentedFrame(isolate, [&](Tagged<SharedFunctionInfo> shared) {
      os << (first ? "" : ",") << "\"" << GetRecordReplayFunctionId(shared)
         << "\"";
      first = false;
    });
  }
  os << "]}";
  return os.str();
}

// With no frames there's nothing a JS command could say.
std::string CommandWithoutFrames(const char* command) {
  if (!strcmp(command, "Pause.getAllFrames")) {
    return "{\"frames\":[],\"data\":{\"frames\":[]}}";
  }
  return "{\"error\":\"No JS is running here\"}";
}

void RememberBreakpoints(int script_id, const RegisteredScript& entry) {
  if (!gRememberedBreakpoints) {
    gRememberedBreakpoints =
        new std::unordered_map<int, std::vector<RememberedBreakpoint>>();
  }
  ScriptScope scope(entry);
  Handle<Script> script = entry.script;
  std::vector<RememberedBreakpoint>& remembered =
      (*gRememberedBreakpoints)[script_id];
  ForEachInstrumentationSite(
      entry.isolate, script, [&](Tagged<SharedFunctionInfo> shared, int site) {
        if (strcmp(InstrumentationSiteKind(site), "breakpoint")) return;
        int line, column;
        GetSiteLocation(script, site, &line, &column);
        remembered.push_back({line, column, GetRecordReplayFunctionId(shared),
                              InstrumentationSiteFunctionIndex(site)});
      });
}

}  // namespace

bool RecordReplayShouldEmitOpcodes(bool record_replay_ignore) {
  return recordreplay::IsRecordingOrReplaying() && !record_replay_ignore &&
         recordreplay::IsMainThread();
}

bool RecordReplayIgnoreScriptByURL(const char* url) {
  // workerd's own modules, which are to a worker what a browser or Node's
  // internals are to their programs.
  static const char* const kInternalPrefixes[] = {
      "node:",    "node-internal:", "cloudflare:",           "cloudflare-internal:",
      "workerd:", "internal:",      "wasm-instantiate-shim", "v8/",
      "pyodide:", "pyodide-internal:", "pyodideRuntime-internal:",
  };
  for (const char* prefix : kInternalPrefixes) {
    if (!strncmp(url, prefix, strlen(prefix))) return true;
  }
  return false;
}

namespace {
int gReplacementScriptId = 0;
}  // namespace

void RecordReplaySetReplacementScriptId(int script_id) {
  gReplacementScriptId = script_id;
}

int RecordReplayNextScriptId() {
  if (gReplacementScriptId) return gReplacementScriptId;
  if (recordreplay::IsMainThread() && !recordreplay::AreEventsDisallowed()) {
    return gNextScriptId++;
  }
  static std::atomic<int> next(gNextDivergentScriptId);
  return next.fetch_add(1, std::memory_order_relaxed);
}

bool RecordReplayIsRegisteredScript(int script_id) {
  return FindScript(script_id) != nullptr;
}

void RecordReplayRegisterScript(Isolate* isolate, DirectHandle<Script> script) {
  if (!recordreplay::IsRecordingOrReplaying() ||
      !recordreplay::IsMainThread() || !script->record_replay_instrumented()) {
    return;
  }
  if (!gScripts) gScripts = new std::unordered_map<int, RegisteredScript>();
  if (gScripts->count(script->id())) return;

  RegisteredScript entry;
  entry.isolate = isolate;
  entry.script =
      Cast<Script>(isolate->global_handles()->Create(*script));
  if (!isolate->context().is_null()) {
    entry.context =
        Cast<Context>(isolate->global_handles()->Create(*isolate->native_context()));
  }
  gScripts->emplace(script->id(), entry);

  // Only instrumented compiles get here, which have events allowed.
  if (recordreplay::AreEventsDisallowed()) return;

  std::string id = std::to_string(script->id());
  std::string url;
  if (IsString(script->name())) {
    url = Cast<String>(script->name())->ToCString().get();
  }
  RecordReplayOnNewSource(id.c_str(), "scriptSource",
                          url.empty() ? nullptr : url.c_str());
}

char* CommandCallback(const char* command, const char* params) {
  CHECK(recordreplay::IsMainThread());
  replayio::AutoDisallowEvents disallow("CommandCallback");

  // Reading a value can run the program's own instrumented code, like a getter,
  // and a replay still has to reach the next point at the recorded progress.
  uint64_t progress = *gProgressCounter;

  std::string result;
  if (!strcmp(command, "Debugger.getSourceContents")) {
    result = CommandGetSourceContents(params);
  } else if (!strcmp(command, "Debugger.getPossibleBreakpoints")) {
    result = CommandGetPossibleBreakpoints(params);
  } else if (!strcmp(command, "Target.convertLocationToFunctionOffset")) {
    result = CommandConvertLocationToFunctionOffset(params);
  } else if (!strcmp(command, "Target.convertFunctionOffsetToLocation")) {
    result = CommandConvertFunctionOffsetToLocation(params);
  } else if (!strcmp(command, "Target.countStackFrames")) {
    result = CommandCountStackFrames();
  } else if (!strcmp(command, "Target.getFunctionsInRange")) {
    result = CommandGetFunctionsInRange(params);
  } else if (!strcmp(command, "Target.currentGeneratorId")) {
    // Nothing here needs generators tied together across their resumes.
    result = "{}";
  } else if (!strcmp(command, "Target.getStackFunctionIDs")) {
    result = CommandGetStackFunctionIds();
  } else {
    v8::Isolate* current = v8::Isolate::TryGetCurrent();
    if (!current) {
      result = CommandWithoutFrames(command);
    } else if (!gGlue) {
      result = "{\"error\":\"Command callback not installed\"}";
    } else {
      result = gGlue->run_command(reinterpret_cast<Isolate*>(current), command,
                                  params);
    }
  }

  if (!recordreplay::HasDivergedFromRecording()) {
    *gProgressCounter = progress;
  }
  return strdup(result.c_str());
}

void RecordReplaySetGlue(const RecordReplayGlue* glue) { gGlue = glue; }

void RecordReplayOnIsolateDispose(Isolate* isolate) {
  if (!recordreplay::IsRecordingOrReplaying()) return;
  // A replay is asked for each source's breakpoints when it reaches the end,
  // which for a process that ends by itself is after its isolates have gone.
  if (recordreplay::IsReplaying() && gScripts) {
    replayio::AutoDisallowEvents disallow("RecordReplayOnIsolateDispose");
    for (auto& [id, entry] : *gScripts) {
      if (entry.isolate == isolate) RememberBreakpoints(id, entry);
    }
  }
  if (gGlue) gGlue->on_isolate_dispose(isolate);
  if (!gScripts) return;
  for (auto it = gScripts->begin(); it != gScripts->end();) {
    it = it->second.isolate == isolate ? gScripts->erase(it) : std::next(it);
  }
}

void RecordReplaySetCurrentConsoleCall(const char* level, void* args) {
  gCurrentConsoleLevel = level;
  gCurrentConsoleArgs = args;
}

void RecordReplayGetCurrentConsoleCall(const char** level, void** args) {
  *level = gCurrentConsoleLevel;
  *args = gCurrentConsoleArgs;
}

void ClearPauseDataCallback() {
  CHECK(recordreplay::IsMainThread());
  replayio::AutoDisallowEvents disallow("ClearPauseDataCallback");
  v8::Isolate* current = v8::Isolate::TryGetCurrent();
  if (current && gGlue) {
    gGlue->clear_pause_data(reinterpret_cast<Isolate*>(current));
  }
}

void PossibleBreakpointsCallback(const char* source_id) {
  CHECK(recordreplay::IsMainThread());
  replayio::AutoDisallowEvents disallow("PossibleBreakpointsCallback");
  const RegisteredScript* entry = FindScript(atoi(source_id));
  if (!entry) {
    if (gRememberedBreakpoints) {
      auto remembered = gRememberedBreakpoints->find(atoi(source_id));
      if (remembered != gRememberedBreakpoints->end()) {
        for (const RememberedBreakpoint& b : remembered->second) {
          RecordReplayAddPossibleBreakpoint(b.line, b.column, b.function.c_str(),
                                            b.function_index);
        }
      }
    }
    return;
  }
  ScriptScope scope(*entry);
  Handle<Script> script = entry->script;
  ForEachInstrumentationSite(
      entry->isolate, script, [&](Tagged<SharedFunctionInfo> shared, int site) {
        if (strcmp(InstrumentationSiteKind(site), "breakpoint")) return;
        int line, column;
        GetSiteLocation(script, site, &line, &column);
        std::string function_id = GetRecordReplayFunctionId(shared);
        RecordReplayAddPossibleBreakpoint(
            line, column, function_id.c_str(),
            InstrumentationSiteFunctionIndex(site));
      });
}

}  // namespace internal
}  // namespace v8

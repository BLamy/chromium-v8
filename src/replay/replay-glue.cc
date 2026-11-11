// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The commands written in JS (replay-glue.js), each isolate's inspector session
// they use, and the native functions they call. These are only ever used when
// replaying, at points where the replay is stopped, and with events disallowed.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "include/v8-context.h"
#include "include/v8-function.h"
#include "include/v8-inspector.h"
#include "include/v8-template.h"
#include "src/api/api-inl.h"
#include "src/builtins/builtins-utils-inl.h"
#include "src/debug/debug-interface.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/replay/replay-glue-js.h"
#include "src/inspector/protocol/Runtime.h"
#include "src/replay/replay-internal.h"
#include "../../third_party/inspector_protocol/crdtp/json.h"
#include "../../third_party/inspector_protocol/crdtp/span.h"

namespace v8 {
namespace internal {

namespace {

constexpr int kContextGroupId = 1;

std::string ToUtf8(v8_inspector::StringView view) {
  if (view.is8Bit()) {
    return std::string(reinterpret_cast<const char*>(view.characters8()),
                       view.length());
  }
  std::string out;
  out.reserve(view.length());
  const uint16_t* chars = view.characters16();
  for (size_t i = 0; i < view.length(); i++) {
    uint32_t c = chars[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < view.length() &&
        chars[i + 1] >= 0xDC00 && chars[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (chars[++i] - 0xDC00);
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

v8::Local<v8::String> ToV8String(v8::Isolate* isolate, const std::string& text) {
  return v8::String::NewFromUtf8(isolate, text.c_str(), v8::NewStringType::kNormal,
                                 static_cast<int>(text.size()))
      .ToLocalChecked();
}

class GlueClient : public v8_inspector::V8InspectorClient {};

// Keeps the response to the message being sent. Events aren't needed.
class GlueChannel : public v8_inspector::V8Inspector::Channel {
 public:
  void sendResponse(int, std::unique_ptr<v8_inspector::StringBuffer> message) override {
    response_ = ToUtf8(message->string());
  }
  void sendNotification(std::unique_ptr<v8_inspector::StringBuffer>) override {}
  void flushProtocolNotifications() override {}

  std::string TakeResponse() { return std::move(response_); }

 private:
  std::string response_;
};

struct IsolateGlue {
  GlueClient client;
  GlueChannel channel;
  // The inspector made here, unless the embedder had already made one.
  std::unique_ptr<v8_inspector::V8Inspector> own_inspector;
  v8_inspector::V8Inspector* inspector = nullptr;
  std::unique_ptr<v8_inspector::V8InspectorSession> session;
  v8::Global<v8::Context> context;
  v8::Global<v8::Function> command_callback;
  v8::Global<v8::Function> clear_pause_data_callback;
  // The progress when pause data was last cleared.
  uint64_t pause_progress = 0;
};

std::map<v8::Isolate*, IsolateGlue*>* gGlue;

void RegisterContext(IsolateGlue* glue, v8::Local<v8::Context> context) {
  if (v8::debug::GetContextId(context) != 0) return;
  static const char kName[] = "worker";
  glue->inspector->contextCreated(v8_inspector::V8ContextInfo(
      context, kContextGroupId,
      v8_inspector::StringView(reinterpret_cast<const uint8_t*>(kName),
                               sizeof(kName) - 1)));
}

void NativeLog(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::String::Utf8Value text(info.GetIsolate(), info[0]);
  recordreplay::Print("%s", *text ? *text : "");
}

void NativeSendCDPMessage(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  IsolateGlue* glue = (*gGlue)[isolate];
  v8::String::Utf8Value message(isolate, info[0]);
  glue->session->dispatchProtocolMessage(v8_inspector::StringView(
      reinterpret_cast<const uint8_t*>(*message), message.length()));
  info.GetReturnValue().Set(ToV8String(isolate, glue->channel.TakeResponse()));
}

// What the embedder answers commands with, in a page's own context: Blink
// knows about the DOM, and what's evaluated there sees the page.
}  // namespace

struct PageCommandHandler {
  v8::Global<v8::Context> context;
  v8::Global<v8::Function> function;
};
std::map<v8::Isolate*, PageCommandHandler>* gPageCommandHandlers;

namespace {

// The embedder's answer to a command the glue has no callback for, as JSON,
// or undefined when it has none either.
void NativePageCommand(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  if (!gPageCommandHandlers) return;
  auto iter = gPageCommandHandlers->find(isolate);
  if (iter == gPageCommandHandlers->end()) return;
  v8::String::Utf8Value method(isolate, info[0]);
  v8::String::Utf8Value params(isolate, info[1]);
  v8::Local<v8::Context> context = iter->second.context.Get(isolate);
  std::string answer;
  {
    v8::Context::Scope context_scope(context);
    v8::TryCatch try_catch(isolate);
    v8::Local<v8::Value> args[] = {ToV8String(isolate, *method ? *method : ""),
                                   ToV8String(isolate, *params ? *params : "")};
    v8::Local<v8::Value> result;
    if (!iter->second.function.Get(isolate)
             ->Call(context, v8::Undefined(isolate), 2, args)
             .ToLocal(&result)) {
      v8::String::Utf8Value error(isolate, try_catch.Exception());
      recordreplay::Print("Page command %s threw: %s", *method ? *method : "",
                          *error ? *error : "");
      return;
    }
    if (!result->IsString()) return;
    v8::String::Utf8Value text(isolate, result);
    answer = *text ? *text : "";
  }
  info.GetReturnValue().Set(ToV8String(isolate, answer.c_str()));
}

// Whether a script's frames are hidden, by the ID the inspector reports it with.
void NativeIgnoreScript(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::String::Utf8Value id(info.GetIsolate(), info[0]);
  bool registered = *id && RecordReplayIsRegisteredScript(atoi(*id));
  info.GetReturnValue().Set(!registered);
}

void NativeCurrentConsoleCall(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  IsolateGlue* glue = (*gGlue)[isolate];
  const char* level;
  void* raw_args;
  RecordReplayGetCurrentConsoleCall(&level, &raw_args);
  std::string json = "{\"level\":\"";
  json += level ? level : "info";
  json += "\",\"args\":[";
  if (raw_args) {
    BuiltinArguments& args = *static_cast<BuiltinArguments*>(raw_args);
    // The arguments belong to whoever's console this is, not to this context.
    Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
    DirectHandle<NativeContext> native(args.target()->native_context(), i_isolate);
    v8::Local<v8::Context> context = v8::Utils::ToLocal(native);
    RegisterContext(glue, context);
    // Index 0 is the receiver.
    for (int i = 1; i < args.length(); i++) {
      if (i != 1) json += ",";
      static const char kGroup[] = "replay";
      auto remote = glue->session->wrapObject(
          context, v8::Utils::ToLocal(args.at(i)),
          v8_inspector::StringView(reinterpret_cast<const uint8_t*>(kGroup),
                                   sizeof(kGroup) - 1),
          false);
      if (!remote) {
        json += "{\"type\":\"undefined\"}";
        continue;
      }
      std::vector<uint8_t> cbor;
      remote->AppendSerialized(&cbor);
      std::vector<uint8_t> text;
      v8_crdtp::json::ConvertCBORToJSON(v8_crdtp::SpanFrom(cbor), &text);
      json.append(reinterpret_cast<const char*>(text.data()), text.size());
    }
  }
  json += "]}";
  info.GetReturnValue().Set(ToV8String(isolate, json));
}

void AddNative(v8::Isolate* isolate, v8::Local<v8::Context> context,
               v8::Local<v8::Object> object, const char* name,
               v8::FunctionCallback callback) {
  object
      ->Set(context, ToV8String(isolate, name),
            v8::Function::New(context, callback).ToLocalChecked())
      .Check();
}

IsolateGlue* EnsureGlue(v8::Isolate* isolate) {
  if (!gGlue) gGlue = new std::map<v8::Isolate*, IsolateGlue*>();
  auto iter = gGlue->find(isolate);
  if (iter != gGlue->end()) return iter->second;

  IsolateGlue* glue = new IsolateGlue();
  (*gGlue)[isolate] = glue;

  v8::HandleScope handles(isolate);

  // workerd makes an inspector per isolate when asked to serve one, and its
  // contexts are registered there. Otherwise, it's up to this.
  glue->inspector = v8::debug::GetInspector(isolate);
  if (!glue->inspector) {
    glue->own_inspector = v8_inspector::V8Inspector::create(isolate, &glue->client);
    glue->inspector = glue->own_inspector.get();
  }
  glue->session = glue->inspector->connect(
      kContextGroupId, &glue->channel, v8_inspector::StringView(),
      v8_inspector::V8Inspector::kFullyTrusted,
      v8_inspector::V8Inspector::kNotWaitingForDebugger);

  v8::Local<v8::Context> context = v8::Context::New(isolate);
  glue->context.Reset(isolate, context);
  v8::Context::Scope context_scope(context);

  v8::Local<v8::Object> rr = v8::Object::New(isolate);
  AddNative(isolate, context, rr, "log", NativeLog);
  AddNative(isolate, context, rr, "sendCDPMessage", NativeSendCDPMessage);
  AddNative(isolate, context, rr, "ignoreScript", NativeIgnoreScript);
  AddNative(isolate, context, rr, "pageCommand", NativePageCommand);
  AddNative(isolate, context, rr, "currentConsoleCall", NativeCurrentConsoleCall);

  v8::TryCatch try_catch(isolate);
  v8::Local<v8::UnboundScript> unbound;
  v8::Local<v8::Value> factory;
  if (!v8::debug::CompileInspectorScript(isolate,
                                         ToV8String(isolate, kReplayGlueSource))
           .ToLocal(&unbound) ||
      !unbound->BindToCurrentContext()->Run(context).ToLocal(&factory)) {
    v8::String::Utf8Value error(isolate, try_catch.Exception());
    recordreplay::Print("Replay glue failed to load: %s", *error ? *error : "");
    return glue;
  }
  v8::Local<v8::Value> arg = rr;
  v8::Local<v8::Value> result;
  if (!factory.As<v8::Function>()
           ->Call(context, v8::Undefined(isolate), 1, &arg)
           .ToLocal(&result)) {
    v8::String::Utf8Value error(isolate, try_catch.Exception());
    recordreplay::Print("Replay glue failed to start: %s", *error ? *error : "");
    return glue;
  }
  v8::Local<v8::Object> exports = result.As<v8::Object>();
  v8::Local<v8::Value> command, clear;
  exports->Get(context, ToV8String(isolate, "commandCallback")).ToLocal(&command);
  exports->Get(context, ToV8String(isolate, "clearPauseDataCallback"))
      .ToLocal(&clear);
  glue->command_callback.Reset(isolate, command.As<v8::Function>());
  glue->clear_pause_data_callback.Reset(isolate, clear.As<v8::Function>());
  return glue;
}

// The inspector can only talk about contexts it's been told about, and the
// embedder only tells it when it's serving an inspector.
void RegisterStackContexts(Isolate* isolate, IsolateGlue* glue) {
  std::vector<v8::Local<v8::Context>> contexts;
  for (JavaScriptStackFrameIterator it(isolate); !it.done(); it.Advance()) {
    Tagged<JSFunction> function = it.frame()->function();
    if (!IsScript(function->shared()->script())) continue;
    DirectHandle<NativeContext> native(function->native_context(), isolate);
    v8::Local<v8::Context> context = v8::Utils::ToLocal(native);
    if (v8::debug::GetContextId(context) == 0) contexts.push_back(context);
  }
  for (v8::Local<v8::Context> context : contexts) RegisterContext(glue, context);
}

}  // namespace

void RecordReplayClearGluePauseData(Isolate* i_isolate);

std::string RecordReplayRunGlueCommand(Isolate* i_isolate, const char* command,
                                       const char* params) {
  v8::Isolate* isolate = reinterpret_cast<v8::Isolate*>(i_isolate);
  v8::HandleScope handles(isolate);
  IsolateGlue* glue = EnsureGlue(isolate);
  if (glue->command_callback.IsEmpty()) {
    return "{\"error\":\"Command callback not installed\"}";
  }
  RegisterStackContexts(i_isolate, glue);

  // In the cloud each pause gets a process of its own, and the engine only clears pause
  // data around console messages. Here one replay stops at thousands of points, and
  // what the inspector wraps at each stays alive until it's released.
  //
  // The program has run on once progress is up by more than the one a bookmark adds,
  // which the engine does between reading a console message and asking about what it
  // read. A replay that's diverged won't run on, and its progress moves with whatever
  // it evaluates.
  if (*gProgressCounter > glue->pause_progress + 1 &&
      !recordreplay::HasDivergedFromRecording()) {
    RecordReplayClearGluePauseData(i_isolate);
  }

  v8::Local<v8::Context> context = glue->context.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> args[] = {ToV8String(isolate, command),
                                 ToV8String(isolate, params)};
  v8::Local<v8::Value> result;
  if (!glue->command_callback.Get(isolate)
           ->Call(context, v8::Undefined(isolate), 2, args)
           .ToLocal(&result)) {
    return "{\"error\":\"Command failed\"}";
  }
  v8::String::Utf8Value text(isolate, result);
  return *text ? std::string(*text) : std::string("{}");
}

void RecordReplayClearGluePauseData(Isolate* i_isolate) {
  if (!gGlue) return;
  v8::Isolate* isolate = reinterpret_cast<v8::Isolate*>(i_isolate);
  auto iter = gGlue->find(isolate);
  if (iter == gGlue->end() || iter->second->clear_pause_data_callback.IsEmpty()) {
    return;
  }
  IsolateGlue* glue = iter->second;
  glue->pause_progress = *gProgressCounter;
  v8::HandleScope handles(isolate);
  v8::Local<v8::Context> context = glue->context.Get(isolate);
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate);
  USE(glue->clear_pause_data_callback.Get(isolate)->Call(
      context, v8::Undefined(isolate), 0, nullptr));
}

void RecordReplayDisposeGlue(Isolate* i_isolate) {
  if (gPageCommandHandlers) {
    gPageCommandHandlers->erase(reinterpret_cast<v8::Isolate*>(i_isolate));
  }
  if (!gGlue) return;
  auto iter = gGlue->find(reinterpret_cast<v8::Isolate*>(i_isolate));
  if (iter == gGlue->end()) return;
  delete iter->second;
  gGlue->erase(iter);
}

// Called from a static initializer in the inspector, which brings this file along
// wherever the inspector is linked.
void RecordReplayInstallGlue() {
  static const RecordReplayGlue kGlue = {RecordReplayRunGlueCommand,
                                         RecordReplayClearGluePauseData,
                                         RecordReplayDisposeGlue};
  RecordReplaySetGlue(&kGlue);
}

}  // namespace internal
}  // namespace v8

// For the embedder, which doesn't see V8's internal headers. Blink declares
// these where it calls them.

// A function of (method, params as JSON) that answers a command with JSON, or
// with undefined for one it doesn't have. The newest one set is asked.
extern "C" V8_EXPORT void V8RecordReplaySetPageCommandHandler(
    v8::Isolate* isolate, v8::Local<v8::Context> context,
    v8::Local<v8::Function> function) {
  using v8::internal::gPageCommandHandlers;
  if (!gPageCommandHandlers) {
    gPageCommandHandlers =
        new std::map<v8::Isolate*, v8::internal::PageCommandHandler>();
  }
  v8::internal::PageCommandHandler& handler = (*gPageCommandHandlers)[isolate];
  handler.context.Reset(isolate, context);
  handler.function.Reset(isolate, function);
}

extern "C" V8_EXPORT uint64_t V8RecordReplayProgressCounter() {
  return v8::internal::gProgressCounter ? *v8::internal::gProgressCounter : 0;
}

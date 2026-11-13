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
#include "src/execution/execution.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/execution/messages.h"
#include "src/objects/debug-objects-inl.h"
#include "src/inspector/v8-inspector-impl.h"
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
  // The session of the context group the glue is made with, which is the
  // only one there is outside a browser.
  std::unique_ptr<v8_inspector::V8InspectorSession> session;
  // Blink gives every local frame root a context group of its own, and a
  // session only sees its group's contexts. A page navigated to after the
  // renderer started is in a later group than the first, so there's a session
  // for each group that's been asked about, as in Replay's Chromium 108.
  std::map<int, std::unique_ptr<v8_inspector::V8InspectorSession>> group_sessions;
  // The group the command being answered is about.
  int current_group = kContextGroupId;
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

// The context group the inspector has a context in, or the glue's own for a
// context it hasn't been told about.
int GroupOf(IsolateGlue* glue, v8::Local<v8::Context> context) {
  if (context.IsEmpty()) return kContextGroupId;
  int id = v8::debug::GetContextId(context);
  if (!id) return kContextGroupId;
  int group = static_cast<v8_inspector::V8InspectorImpl*>(glue->inspector)
                  ->contextGroupId(id);
  return group ? group : kContextGroupId;
}

// The replay's session in a context group, made when the group is first
// asked about, with the domains the glue's script enabled in the first.
v8_inspector::V8InspectorSession* SessionFor(IsolateGlue* glue, int group) {
  if (group == kContextGroupId) return glue->session.get();
  auto iter = glue->group_sessions.find(group);
  if (iter != glue->group_sessions.end()) return iter->second.get();
  std::unique_ptr<v8_inspector::V8InspectorSession> session =
      glue->inspector->connect(
          group, &glue->channel, v8_inspector::StringView(),
          v8_inspector::V8Inspector::kFullyTrusted,
          v8_inspector::V8Inspector::kNotWaitingForDebugger);
  recordreplay::AddReplayOwnedInspectorSession(session.get());
  for (const char* message :
       {"{\"id\":1,\"method\":\"Runtime.enable\"}",
        "{\"id\":2,\"method\":\"Debugger.enable\"}"}) {
    session->dispatchProtocolMessage(v8_inspector::StringView(
        reinterpret_cast<const uint8_t*>(message), strlen(message)));
    glue->channel.TakeResponse();
  }
  v8_inspector::V8InspectorSession* raw = session.get();
  glue->group_sessions[group] = std::move(session);
  return raw;
}

void NativeLog(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::String::Utf8Value text(info.GetIsolate(), info[0]);
  recordreplay::Print("%s", *text ? *text : "");
}

void NativeSendCDPMessage(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  IsolateGlue* glue = (*gGlue)[isolate];
  v8::String::Utf8Value message(isolate, info[0]);
  SessionFor(glue, glue->current_group)
      ->dispatchProtocolMessage(v8_inspector::StringView(
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

void NativeCurrentPageError(const v8::FunctionCallbackInfo<v8::Value>& info) {
  const char* error = RecordReplayCurrentPageError();
  info.GetReturnValue().Set(ToV8String(info.GetIsolate(), error ? error : ""));
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
      auto remote = SessionFor(glue, GroupOf(glue, context))->wrapObject(
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

// The inspector's description of any value, as JSON, for the glue to give an
// ID to. The embedder hands the glue page objects this way.
// What the top frame is returning, when the replay is stopped where it returns.
void NativeHasReturnValue(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Value> value;
  info.GetReturnValue().Set(v8::recordreplay::GetCurrentReturnValue(&value));
}

void NativeReturnValue(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Value> value;
  if (v8::recordreplay::GetCurrentReturnValue(&value)) {
    info.GetReturnValue().Set(value);
  }
}

void NativeWrapValue(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  IsolateGlue* glue = (*gGlue)[isolate];
  v8::Local<v8::Value> value = info[0];
  // An object is described where it was made, which is a context the
  // inspector may not have been told about yet.
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (value->IsObject()) {
    v8::Local<v8::Context> creation;
    if (value.As<v8::Object>()->GetCreationContext(isolate).ToLocal(&creation)) {
      context = creation;
    }
  }
  RegisterContext(glue, context);
  static const char kGroup[] = "replay";
  auto remote = SessionFor(glue, GroupOf(glue, context))->wrapObject(
      context, value,
      v8_inspector::StringView(reinterpret_cast<const uint8_t*>(kGroup),
                               sizeof(kGroup) - 1),
      false);
  if (!remote) return;
  std::vector<uint8_t> cbor;
  remote->AppendSerialized(&cbor);
  std::vector<uint8_t> text;
  v8_crdtp::json::ConvertCBORToJSON(v8_crdtp::SpanFrom(cbor), &text);
  info.GetReturnValue().Set(ToV8String(
      isolate,
      std::string(reinterpret_cast<const char*>(text.data()), text.size())));
}

// An object's persistent ID, or zero when it has none. Never makes one: an ID
// made at a pause would be made in one replay and not another.
void NativePersistentId(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  int id = info.Length() && info[0]->IsObject()
               ? RecordReplayObjectId(isolate, info[0], /* allow_create */ false)
               : 0;
  info.GetReturnValue().Set(v8::Integer::New(isolate, id));
}

// The value the inspector gave an object ID to.
void NativeUnwrapObject(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  IsolateGlue* glue = (*gGlue)[isolate];
  v8::String::Utf8Value id(isolate, info[0]);
  std::unique_ptr<v8_inspector::StringBuffer> error;
  v8::Local<v8::Value> value;
  v8::Local<v8::Context> context;
  if (!*id) return;
  v8_inspector::StringView view(reinterpret_cast<const uint8_t*>(*id),
                                id.length());
  // An ID is its session's, and the group being asked about is the likeliest.
  if (SessionFor(glue, glue->current_group)
          ->unwrapObject(&error, view, &value, &context, nullptr)) {
    info.GetReturnValue().Set(value);
    return;
  }
  if (glue->current_group != kContextGroupId &&
      glue->session->unwrapObject(&error, view, &value, &context, nullptr)) {
    info.GetReturnValue().Set(value);
    return;
  }
  for (auto& entry : glue->group_sessions) {
    if (entry.first == glue->current_group) continue;
    if (entry.second->unwrapObject(&error, view, &value, &context, nullptr)) {
      info.GetReturnValue().Set(value);
      return;
    }
  }
}

// An Error's stack as text, if it has been formatted and the Error still has
// the stack accessor it was made with. In V8 10.8 a formatted stack was a data
// property, which a preview shows as its value; since then it's an accessor
// on each Error, which a preview would leave for the viewer to call. Nothing
// of the page's runs here: an unformatted stack would be formatted by the
// page's Error.prepareStackTrace, so that one stays an accessor.
void NativeErrorFormattedStack(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
  if (!info[0]->IsObject() || info[0]->IsProxy()) return;
  v8::Local<v8::Object> object = info[0].As<v8::Object>();
  DirectHandle<JSReceiver> receiver = Utils::OpenDirectHandle(*object);
  if (!IsJSObject(*receiver) || IsAccessCheckNeeded(*receiver)) return;
  v8::Local<v8::Context> context;
  if (!object->GetCreationContext(isolate).ToLocal(&context)) return;
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> stack_name = ToV8String(isolate, "stack");
  v8::Local<v8::String> get_name = ToV8String(isolate, "get");
  v8::Local<v8::Value> fresh =
      v8::Exception::Error(v8::String::Empty(isolate));
  v8::Local<v8::Value> own_descriptor, fresh_descriptor, own_get, fresh_get;
  if (!fresh->IsObject() ||
      !object->GetOwnPropertyDescriptor(context, stack_name)
           .ToLocal(&own_descriptor) ||
      !fresh.As<v8::Object>()
           ->GetOwnPropertyDescriptor(context, stack_name)
           .ToLocal(&fresh_descriptor) ||
      !own_descriptor->IsObject() || !fresh_descriptor->IsObject() ||
      !own_descriptor.As<v8::Object>()->Get(context, get_name).ToLocal(&own_get) ||
      !fresh_descriptor.As<v8::Object>()
           ->Get(context, get_name)
           .ToLocal(&fresh_get) ||
      !own_get->IsFunction() || !own_get->StrictEquals(fresh_get)) {
    return;
  }
  ErrorUtils::StackPropertyLookupResult lookup =
      ErrorUtils::GetErrorStackProperty(i_isolate, receiver);
  if (!IsErrorStackData(*lookup.error_stack)) return;
  Tagged<ErrorStackData> data = Cast<ErrorStackData>(*lookup.error_stack);
  if (!data->HasFormattedStack() || !IsString(data->formatted_stack())) return;
  info.GetReturnValue().Set(Utils::ToLocal(
      direct_handle(Cast<String>(data->formatted_stack()), i_isolate)));
}

// Gives the glue of an isolate the natives the embedder has for it.
void (*gGlueNativesInstaller)(v8::Isolate* isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Object> rr) = nullptr;

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
  recordreplay::AddReplayOwnedInspectorSession(glue->session.get());

  v8::Local<v8::Context> context = v8::Context::New(isolate);
  glue->context.Reset(isolate, context);
  v8::Context::Scope context_scope(context);

  v8::Local<v8::Object> rr = v8::Object::New(isolate);
  AddNative(isolate, context, rr, "log", NativeLog);
  AddNative(isolate, context, rr, "sendCDPMessage", NativeSendCDPMessage);
  AddNative(isolate, context, rr, "ignoreScript", NativeIgnoreScript);
  AddNative(isolate, context, rr, "pageCommand", NativePageCommand);
  AddNative(isolate, context, rr, "currentPageError", NativeCurrentPageError);
  AddNative(isolate, context, rr, "currentConsoleCall", NativeCurrentConsoleCall);
  AddNative(isolate, context, rr, "wrapValue", NativeWrapValue);
  AddNative(isolate, context, rr, "hasReturnValue", NativeHasReturnValue);
  AddNative(isolate, context, rr, "returnValue", NativeReturnValue);
  AddNative(isolate, context, rr, "unwrapObject", NativeUnwrapObject);
  AddNative(isolate, context, rr, "persistentId", NativePersistentId);
  AddNative(isolate, context, rr, "errorFormattedStack",
            NativeErrorFormattedStack);
  if (gGlueNativesInstaller) gGlueNativesInstaller(isolate, context, rr);

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

// The context group a command is about: the one the innermost script on the
// stack runs in, which is where the replay is stopped. With no script on the
// stack it's the group of the page the embedder answers commands for.
int CurrentGroup(Isolate* isolate, IsolateGlue* glue) {
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8::Local<v8::Context> own = glue->context.Get(v8_isolate);
  for (JavaScriptStackFrameIterator it(isolate); !it.done(); it.Advance()) {
    Tagged<JSFunction> function = it.frame()->function();
    if (!IsScript(function->shared()->script())) continue;
    DirectHandle<NativeContext> native(function->native_context(), isolate);
    v8::Local<v8::Context> context = v8::Utils::ToLocal(native);
    if (context == own) continue;
    return GroupOf(glue, context);
  }
  if (gPageCommandHandlers) {
    auto iter = gPageCommandHandlers->find(v8_isolate);
    if (iter != gPageCommandHandlers->end() && !iter->second.context.IsEmpty()) {
      return GroupOf(glue, iter->second.context.Get(v8_isolate));
    }
  }
  return kContextGroupId;
}

}  // namespace

v8_inspector::V8InspectorSession* RecordReplayGlueSession(
    v8::Isolate* isolate) {
  v8::HandleScope handles(isolate);
  return EnsureGlue(isolate)->session.get();
}

v8_inspector::V8InspectorSession* RecordReplayGlueSessionForGroup(
    v8::Isolate* isolate, int group) {
  v8::HandleScope handles(isolate);
  return SessionFor(EnsureGlue(isolate), group > 0 ? group : kContextGroupId);
}

void RecordReplaySetGlueNativesInstaller(
    void (*installer)(v8::Isolate*, v8::Local<v8::Context>,
                      v8::Local<v8::Object>)) {
  gGlueNativesInstaller = installer;
}

void RecordReplayClearGluePauseData(Isolate* i_isolate);

namespace {
// Calls one of the glue's own functions. Not through the API: that runs the
// embedder's before-call callback, and Blink's checks that script isn't
// forbidden, which it is where a command can arrive (the inspector tells a
// replay's session that a context is gone while a frame is being detached).
// The glue's script is the replay's, not the page's. It doesn't run microtasks
// afterwards either, which would be the page's.
v8::MaybeLocal<v8::Value> CallGlueFunction(Isolate* i_isolate,
                                           v8::Local<v8::Function> function,
                                           int argc,
                                           v8::Local<v8::Value>* argv) {
  std::vector<DirectHandle<Object>> args;
  for (int i = 0; i < argc; i++) {
    args.push_back(Utils::OpenDirectHandle(*argv[i]));
  }
  DirectHandle<Object> result;
  if (!Execution::TryCall(i_isolate, Utils::OpenDirectHandle(*function),
                          i_isolate->factory()->undefined_value(),
                          base::VectorOf(args),
                          Execution::MessageHandling::kKeepPending, nullptr)
           .ToHandle(&result)) {
    return {};
  }
  return Utils::ToLocal(result);
}
}  // namespace

std::string RecordReplayRunGlueCommand(Isolate* i_isolate, const char* command,
                                       const char* params) {
  v8::Isolate* isolate = reinterpret_cast<v8::Isolate*>(i_isolate);
  v8::HandleScope handles(isolate);
  IsolateGlue* glue = EnsureGlue(isolate);
  if (glue->command_callback.IsEmpty()) {
    return "{\"error\":\"Command callback not installed\"}";
  }
  RegisterStackContexts(i_isolate, glue);
  glue->current_group = CurrentGroup(i_isolate, glue);

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
  if (!CallGlueFunction(i_isolate, glue->command_callback.Get(isolate), 2,
                        args)
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
  USE(CallGlueFunction(i_isolate, glue->clear_pause_data_callback.Get(isolate),
                       0, nullptr));
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
  v8::internal::RecordReplaySetPageCommandIsolate(
      reinterpret_cast<v8::internal::Isolate*>(isolate));
}

extern "C" V8_EXPORT const char* V8RecordReplayPendingPrivilegedEvaluation(
    size_t index) {
  return v8::internal::RecordReplayPendingPrivilegedEvaluation(index);
}

extern "C" V8_EXPORT uint64_t V8RecordReplayProgressCounter() {
  return v8::internal::gProgressCounter ? *v8::internal::gProgressCounter : 0;
}

// A stretch of the replay's own code, run from inside the program's: a hook
// it put into a script it replaced. Nothing in it is in the recording, and it
// can call the program's functions, so the count of progress is put back
// when it ends, as it is after a command.
namespace {
std::vector<uint64_t>* gReplayCodeProgress = nullptr;
}  // namespace

extern "C" V8_EXPORT void V8RecordReplayBeginReplayCode(const char* label) {
  if (!v8::recordreplay::IsRecordingOrReplaying()) return;
  if (!gReplayCodeProgress) gReplayCodeProgress = new std::vector<uint64_t>();
  v8::recordreplay::BeginDisallowEvents(label);
  gReplayCodeProgress->push_back(
      v8::internal::gProgressCounter ? *v8::internal::gProgressCounter : 0);
}

extern "C" V8_EXPORT void V8RecordReplayEndReplayCode() {
  if (!v8::recordreplay::IsRecordingOrReplaying() || !gReplayCodeProgress ||
      gReplayCodeProgress->empty()) {
    return;
  }
  uint64_t progress = gReplayCodeProgress->back();
  gReplayCodeProgress->pop_back();
  if (v8::internal::gProgressCounter &&
      !v8::recordreplay::HasDivergedFromRecording()) {
    *v8::internal::gProgressCounter = progress;
  }
  v8::recordreplay::EndDisallowEvents();
}

extern "C" V8_EXPORT void V8RecordReplayEnterReplayCode() {
  v8::recordreplay::EnterReplayCode();
}

extern "C" V8_EXPORT void V8RecordReplayExitReplayCode() {
  v8::recordreplay::ExitReplayCode();
}

// An uncaught error, which the embedder knows about and V8's console doesn't.
// The driver reads what it says, with a command, before this returns.
extern "C" V8_EXPORT void V8RecordReplayOnPageError(const char* message,
                                                    const char* url,
                                                    int line, int column,
                                                    int script_id,
                                                    int bookmark) {
  if (!v8::recordreplay::IsRecordingOrReplaying() ||
      !v8::recordreplay::IsMainThread() ||
      v8::recordreplay::AreEventsDisallowed()) {
    return;
  }
  auto escape = [](const char* text) {
    std::string out;
    for (const char* p = text ? text : ""; *p; p++) {
      unsigned char c = static_cast<unsigned char>(*p);
      if (c == '"' || c == '\\') {
        out += '\\';
        out += static_cast<char>(c);
      } else if (c < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
    return out;
  };
  std::string json = "{\"message\":\"" + escape(message) + "\",\"url\":\"" +
                     escape(url) + "\",\"line\":" + std::to_string(line) +
                     ",\"column\":" + std::to_string(column) +
                     ",\"scriptId\":" + std::to_string(script_id) + "}";
  v8::internal::RecordReplaySetCurrentPageError(json.c_str());
  // No JS is running by now, so the message is where it was thrown, which
  // has a bookmark when V8 made the message for an exception.
  v8::internal::RecordReplayOnConsoleMessage(
      bookmark > 0 ? static_cast<size_t>(bookmark)
                   : v8::internal::RecordReplayNewBookmark());
  v8::internal::RecordReplaySetCurrentPageError(nullptr);
}


// The dependency graph a replay builds. The embedder has the functions that
// add to it, and gives them to V8 for what V8 knows about: promises.
namespace v8 {
namespace internal {
namespace {

struct DependencyGraphFunctions {
  bool (*enabled)() = nullptr;
  int (*new_node)(const char* json) = nullptr;
  void (*add_edge)(int source, int target, const char* json) = nullptr;
  void (*begin_execution)(int node) = nullptr;
  void (*end_execution)() = nullptr;
  int (*execution_node)() = nullptr;
};
DependencyGraphFunctions gDependencyGraph;

int (*gApiObjectId)(v8::Local<v8::Object> object) = nullptr;

// Commands the embedder answers, with a string the caller frees.
std::map<std::string, char* (*)(const char* params)>* gEmbedderCommands =
    nullptr;

// Tables keyed weakly by object, each for the main thread's isolate, which is
// the first to use one.
struct WeakIntTable {
  v8::Global<v8::debug::EphemeronTable> table;

  int Get(v8::Isolate* isolate, v8::Local<v8::Value> key) {
    if (table.IsEmpty()) return 0;
    v8::Local<v8::Value> value;
    if (!table.Get(isolate)->Get(isolate, key).ToLocal(&value) ||
        !value->IsInt32()) {
      return 0;
    }
    return value.As<v8::Int32>()->Value();
  }

  void Set(v8::Isolate* isolate, v8::Local<v8::Value> key, int value) {
    v8::Local<v8::debug::EphemeronTable> current =
        table.IsEmpty() ? v8::debug::EphemeronTable::New(isolate)
                        : table.Get(isolate);
    table.Reset(isolate,
                current->Set(isolate, key, v8::Integer::New(isolate, value)));
  }
};

v8::Isolate* gObjectIdIsolate = nullptr;
WeakIntTable* gObjectIds = nullptr;
WeakIntTable* gPromiseNewNodes = nullptr;
WeakIntTable* gPromiseSettledNodes = nullptr;
int gNextObjectId = 1;

bool UseObjectTables(v8::Isolate* isolate) {
  if (!v8::recordreplay::IsMainThread()) return false;
  if (!gObjectIdIsolate) {
    gObjectIdIsolate = isolate;
    gObjectIds = new WeakIntTable();
    gPromiseNewNodes = new WeakIntTable();
    gPromiseSettledNodes = new WeakIntTable();
  }
  return gObjectIdIsolate == isolate;
}

bool DependencyGraphEnabled() {
  return gDependencyGraph.enabled && gDependencyGraph.enabled();
}

}  // namespace

bool gWarnMissingDependencyExecution = false;

// An ID for an object, the same for as long as the object lives. An object of
// the embedder's has the one the embedder gave it.
int RecordReplayObjectId(v8::Isolate* isolate, v8::Local<v8::Value> value,
                         bool allow_create) {
  if (!value->IsObject()) return 0;
  if (gApiObjectId) {
    if (int id = gApiObjectId(value.As<v8::Object>())) return id;
  }
  if (!UseObjectTables(isolate)) return 0;
  v8::HandleScope scope(isolate);
  if (int id = gObjectIds->Get(isolate, value)) return id;
  if (!allow_create) return 0;
  int id = gNextObjectId++;
  gObjectIds->Set(isolate, value, id);
  return id;
}

bool RecordReplayShouldCallOnPromiseHook() {
  return recordreplay::IsRecordingOrReplaying() && recordreplay::IsMainThread();
}

void RecordReplayOnPromiseHook(Isolate* isolate, PromiseHookType type,
                               DirectHandle<JSPromise> promise,
                               DirectHandle<Object> parent) {
  if (!DependencyGraphEnabled()) return;
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  if (!UseObjectTables(v8_isolate)) return;
  v8::HandleScope scope(v8_isolate);
  v8::Local<v8::Value> key = v8::Utils::ToLocal(promise);

  switch (type) {
    case PromiseHookType::kInit: {
      if (gPromiseNewNodes->Get(v8_isolate, key)) break;
      std::string json =
          "{\"kind\":\"newPromise\",\"persistentId\":" +
          std::to_string(RecordReplayObjectId(v8_isolate, key, true)) + "}";
      int node = gDependencyGraph.new_node(json.c_str());
      gPromiseNewNodes->Set(v8_isolate, key, node);
      if (IsJSPromise(*parent)) {
        int parent_node =
            gPromiseNewNodes->Get(v8_isolate, v8::Utils::ToLocal(parent));
        if (parent_node) {
          gDependencyGraph.add_edge(parent_node, node,
                                    "{\"kind\":\"parentPromise\"}");
        }
      }
      break;
    }
    case PromiseHookType::kResolve: {
      int node = gPromiseNewNodes->Get(v8_isolate, key);
      if (!node || gPromiseSettledNodes->Get(v8_isolate, key)) break;
      int settled = gDependencyGraph.new_node("{\"kind\":\"promiseSettled\"}");
      gPromiseSettledNodes->Set(v8_isolate, key, settled);
      gDependencyGraph.add_edge(node, settled, "{\"kind\":\"basePromise\"}");
      break;
    }
    case PromiseHookType::kBefore: {
      int settled = gPromiseSettledNodes->Get(v8_isolate, key);
      gDependencyGraph.begin_execution(
          settled ? settled : gPromiseNewNodes->Get(v8_isolate, key));
      break;
    }
    case PromiseHookType::kAfter:
      gDependencyGraph.end_execution();
      break;
  }
}

// With RECORD_REPLAY_WARN_MISSING_EXECUTION set, says which of the program's
// functions were called while no node of the graph was executing, which is a
// place the graph has no account of.
void RecordReplayCheckDependencyExecution(Isolate* isolate,
                                          DirectHandle<JSFunction> function) {
  if (!gWarnMissingDependencyExecution || !recordreplay::IsReplaying() ||
      !recordreplay::IsMainThread() || recordreplay::AreEventsDisallowed() ||
      !DependencyGraphEnabled() || !gDependencyGraph.execution_node ||
      gDependencyGraph.execution_node() != 0 ||
      !IsScript(function->shared()->script())) {
    return;
  }
  recordreplay::Print("DependencyGraphMissingExecution %s",
                      GetRecordReplayFunctionId(function->shared()).c_str());
}

void RecordReplayOnPromiseAdoption(Isolate* isolate,
                                   DirectHandle<JSPromise> promise,
                                   DirectHandle<Object> resolution) {
  if (!DependencyGraphEnabled() || !IsJSPromise(*resolution)) return;
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  if (!UseObjectTables(v8_isolate)) return;
  v8::HandleScope scope(v8_isolate);
  int settled =
      gPromiseSettledNodes->Get(v8_isolate, v8::Utils::ToLocal(promise));
  int adopted =
      gPromiseNewNodes->Get(v8_isolate, v8::Utils::ToLocal(resolution));
  if (settled && adopted) {
    gDependencyGraph.add_edge(settled, adopted,
                              "{\"kind\":\"adoptedPromise\"}");
  }
}

}  // namespace internal
}  // namespace v8

extern "C" V8_EXPORT void V8RecordReplaySetDependencyGraphFunctions(
    bool (*enabled)(),
    int (*new_node)(const char* json),
    void (*add_edge)(int source, int target, const char* json),
    void (*begin_execution)(int node),
    void (*end_execution)(),
    int (*execution_node)()) {
  v8::internal::gDependencyGraph.execution_node = execution_node;
  v8::internal::gDependencyGraph.enabled = enabled;
  v8::internal::gDependencyGraph.new_node = new_node;
  v8::internal::gDependencyGraph.add_edge = add_edge;
  v8::internal::gDependencyGraph.begin_execution = begin_execution;
  v8::internal::gDependencyGraph.end_execution = end_execution;
}

// The embedder's own ID for one of its objects, or zero.
extern "C" V8_EXPORT void V8RecordReplaySetApiObjectIdCallback(
    int (*callback)(v8::Local<v8::Object> object)) {
  v8::internal::gApiObjectId = callback;
}

extern "C" V8_EXPORT int V8RecordReplayObjectId(v8::Isolate* isolate,
                                                v8::Local<v8::Value> value,
                                                bool allow_create) {
  return v8::internal::RecordReplayObjectId(isolate, value, allow_create);
}

// |handler| answers |name| from now on, with JSON made by malloc.
extern "C" V8_EXPORT void V8RecordReplayRegisterEmbedderCommand(
    const char* name, char* (*handler)(const char* params)) {
  using v8::internal::gEmbedderCommands;
  if (!gEmbedderCommands) {
    gEmbedderCommands =
        new std::map<std::string, char* (*)(const char* params)>();
  }
  (*gEmbedderCommands)[name] = handler;
}

namespace v8 {
namespace internal {

bool RecordReplayEmbedderCommand(const char* command, const char* params,
                                 std::string* result) {
  if (!gEmbedderCommands) return false;
  auto it = gEmbedderCommands->find(command);
  if (it == gEmbedderCommands->end()) return false;
  char* answer = it->second(params);
  *result = answer ? answer : "{}";
  free(answer);
  return true;
}

}  // namespace internal
}  // namespace v8

// The embedder's natives for the glue, set before the first command: on the
// object the glue script is given, functions for what only the embedder knows.
extern "C" V8_EXPORT void V8RecordReplaySetGlueNativesInstaller(
    void (*installer)(v8::Isolate* isolate, v8::Local<v8::Context> context,
                      v8::Local<v8::Object> rr)) {
  v8::internal::RecordReplaySetGlueNativesInstaller(installer);
}

// The inspector session a replay's commands go through in this isolate.
extern "C" V8_EXPORT v8_inspector::V8InspectorSession*
V8RecordReplayGlueSession(v8::Isolate* isolate) {
  return v8::internal::RecordReplayGlueSession(isolate);
}

// The replay's session in one context group. Blink's agents for a frame are
// given its root's.
extern "C" V8_EXPORT v8_inspector::V8InspectorSession*
V8RecordReplayGlueSessionForGroup(v8::Isolate* isolate, int group) {
  return v8::internal::RecordReplayGlueSessionForGroup(isolate, group);
}

extern "C" V8_EXPORT void V8RecordReplaySetNewScriptCallback(
    void (*callback)(v8::Isolate* isolate, int script_id, const char* url,
                     const char* source_map_url)) {
  v8::internal::RecordReplaySetNewScriptCallback(callback);
}

// The text of one of the program's scripts, by its ID. The caller frees it.
extern "C" V8_EXPORT char* V8RecordReplayScriptSource(int script_id) {
  std::string source;
  if (!v8::internal::RecordReplayScriptSource(script_id, &source)) {
    return nullptr;
  }
  return strdup(source.c_str());
}

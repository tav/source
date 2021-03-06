#include "binding.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include "libplatform/libplatform.h"
#include "v8.h"

using namespace v8;

struct worker_s {
  int id;
  Isolate* isolate;
  std::string last_exception;
  Persistent<Function> recv;
  Persistent<Context> context;
  Persistent<Function> recv_sync_handler;
};

// Per-context Module data, allowing sharing of module maps across top-level
// module loads. Adapted from V8's source.
class ModuleData {
 private:
  class ModuleHash {
   public:
    explicit ModuleHash(Isolate* isolate) : isolate_(isolate) {}
    size_t operator()(const Global<Module>& module) const {
      return module.Get(isolate_)->GetIdentityHash();
    }

   private:
    Isolate* isolate_;
  };

 public:
  explicit ModuleData(Isolate* isolate)
      : module_to_url_map(10, ModuleHash(isolate)) {}

  std::unordered_map<std::string, Global<Module>> url_to_module_map;
  std::unordered_map<Global<Module>, std::string, ModuleHash> module_to_url_map;
};

// CopyString converts a std::string to a C string.
const char* CopyString(const std::string& value) {
  char* c = (char*)malloc(value.length());
  strcpy(c, value.c_str());
  return c;
}

// ToCString extracts a C string from a V8 Utf8Value.
const char* ToCString(const String::Utf8Value& value) {
  return *value ? *value : "<v8worker: string conversion failed>";
}

std::string ToStdString(Isolate* isolate, Local<String> value) {
  String::Utf8Value utf8(isolate, value);
  return *utf8;
}

// ExceptionString gathers details about the latest Exception.
std::string ExceptionString(Isolate* isolate,
                            Local<Context> context,
                            TryCatch* try_catch) {
  std::string out;
  size_t scratchSize = 20;
  char scratch[scratchSize];

  HandleScope handle_scope(isolate);
  String::Utf8Value exception(try_catch->Exception());
  const char* exception_string = ToCString(exception);

  Handle<Message> message = try_catch->Message();

  if (message.IsEmpty()) {
    // V8 didn't provide any extra information about this error; just
    // print the exception.
    out.append(exception_string);
    out.append("\n");
  } else {
    // Print (filename):(line number)
    String::Utf8Value filename(message->GetScriptOrigin().ResourceName());
    const char* filename_string = ToCString(filename);
    int linenum = message->GetLineNumber();

    snprintf(scratch, scratchSize, "%i", linenum);
    out.append(filename_string);
    out.append(":");
    out.append(scratch);
    out.append("\n");

    // Print line of source code.
    String::Utf8Value sourceline(message->GetSourceLine());
    const char* sourceline_string = ToCString(sourceline);

    out.append(sourceline_string);
    out.append("\n");

    // Print wavy underline.
    int start = message->GetStartColumn(context).FromMaybe(0);
    for (int i = 0; i < start; i++) {
      out.append(" ");
    }
    int end = message->GetEndColumn(context).FromMaybe(0);
    for (int i = start; i < end; i++) {
      out.append("^");
    }
    out.append("\n");
    String::Utf8Value stack_trace(try_catch->StackTrace());
    if (stack_trace.length() > 0) {
      const char* stack_trace_string = ToCString(stack_trace);
      out.append(stack_trace_string);
      out.append("\n");
    } else {
      out.append(exception_string);
      out.append("\n");
    }
  }
  return out;
}

ModuleData* GetModuleData(Local<Context> context) {
  return static_cast<ModuleData*>(
      context->GetAlignedPointerFromEmbedderData(1));
}

void InitModuleData(Local<Context> context) {
  context->SetAlignedPointerInEmbedderData(
      1, new ModuleData(context->GetIsolate()));
}

MaybeLocal<Module> ResolveModuleCallback(Local<Context> context,
                                         Local<String> url,
                                         Local<Module> referrer) {
  Isolate* isolate = context->GetIsolate();
  ModuleData* d = GetModuleData(context);
  std::string url_str = ToStdString(isolate, url);
  auto module_it = d->url_to_module_map.find(url_str);
  return module_it->second.Get(isolate);
}

extern "C" {
#include "_cgo_export.h"

void LoadModule(worker* w,
                Local<Context> context,
                Local<String> url,
                MaybeLocal<Module>& mod) {
  ScriptOrigin origin(url, Local<Integer>(), Local<Integer>(), Local<Boolean>(),
                      Local<Integer>(), Local<Value>(), Local<Boolean>(),
                      Local<Boolean>(), True(w->isolate));

  std::string url_str = ToStdString(w->isolate, url);
  char* source_str = getModuleSource(w->id, (char*)url_str.c_str());
  Local<String> source_text = String::NewFromUtf8(w->isolate, source_str);
  ScriptCompiler::Source source(source_text, origin);

  Local<Module> module;
  if (!ScriptCompiler::CompileModule(w->isolate, &source).ToLocal(&module)) {
    return;
  }

  ModuleData* d = GetModuleData(context);
  d->url_to_module_map.insert(
      std::make_pair(url_str, Global<Module>(w->isolate, module)));
  d->module_to_url_map.insert(
      std::make_pair(Global<Module>(w->isolate, module), url_str));

  for (int i = 0, length = module->GetModuleRequestsLength(); i < length; ++i) {
    Local<String> name = module->GetModuleRequest(i);
    MaybeLocal<Module> submodule;
    LoadModule(w, context, name, submodule);
    if (submodule.IsEmpty()) {
      return;
    }
  }

  mod = module;
  return;
}

// The $print function.
void Print(const FunctionCallbackInfo<Value>& args) {
  bool first = true;
  for (int i = 0; i < args.Length(); i++) {
    HandleScope handle_scope(args.GetIsolate());
    if (first) {
      first = false;
    } else {
      printf(" ");
    }
    String::Utf8Value str(args[i]);
    const char* cstr = ToCString(str);
    printf("%s", cstr);
  }
  printf("\n");
  fflush(stdout);
}

// The $recv function. Sets the given callback.
void Recv(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  worker* w = (worker*)isolate->GetData(0);
  assert(w->isolate == isolate);

  HandleScope handle_scope(isolate);

  Local<Context> context = Local<Context>::New(w->isolate, w->context);
  Context::Scope context_scope(context);

  Local<Value> v = args[0];
  assert(v->IsFunction());
  Local<Function> func = Local<Function>::Cast(v);

  w->recv.Reset(isolate, func);
}

// The $recvSync function. Sets the given callback.
void RecvSync(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  worker* w = (worker*)isolate->GetData(0);
  assert(w->isolate == isolate);

  HandleScope handle_scope(isolate);

  Local<Context> context = Local<Context>::New(w->isolate, w->context);
  Context::Scope context_scope(context);

  Local<Value> v = args[0];
  assert(v->IsFunction());
  Local<Function> func = Local<Function>::Cast(v);

  w->recv_sync_handler.Reset(isolate, func);
}

// The $send function. Calls the corresponding worker's Callback in Go.
void Send(const FunctionCallbackInfo<Value>& args) {
  std::string msg;
  worker* w = NULL;
  {
    Isolate* isolate = args.GetIsolate();
    w = static_cast<worker*>(isolate->GetData(0));
    assert(w->isolate == isolate);

    Locker locker(w->isolate);
    HandleScope handle_scope(isolate);

    Local<Context> context = Local<Context>::New(w->isolate, w->context);
    Context::Scope context_scope(context);

    Local<Value> v = args[0];
    assert(v->IsString());

    String::Utf8Value str(v);
    msg = ToCString(str);
  }
  // TODO(tav): should we use Unlocker?
  recvCb(w->id, (char*)msg.c_str());
}

// The $sendSync function. Calls the corresponding worker's SyncCallback in Go.
void SendSync(const FunctionCallbackInfo<Value>& args) {
  std::string msg;
  worker* w = NULL;
  {
    Isolate* isolate = args.GetIsolate();
    w = static_cast<worker*>(isolate->GetData(0));
    assert(w->isolate == isolate);

    Locker locker(w->isolate);
    HandleScope handle_scope(isolate);

    Local<Context> context = Local<Context>::New(w->isolate, w->context);
    Context::Scope context_scope(context);

    Local<Value> v = args[0];
    assert(v->IsString());

    String::Utf8Value str(v);
    msg = ToCString(str);
  }
  char* returnMsg = recvSyncCb(w->id, (char*)msg.c_str());
  Local<String> returnV = String::NewFromUtf8(w->isolate, returnMsg);
  args.GetReturnValue().Set(returnV);
  free(returnMsg);
}

void v8_init() {
  const char* options = "--harmony_public_fields --harmony_private_fields";
  V8::SetFlagsFromString(options, strlen(options));
  Platform* platform = platform::CreateDefaultPlatform();
  V8::InitializePlatform(platform);
  V8::Initialize();
}

void worker_dispose(worker* w) {
  w->isolate->Dispose();
  delete (w);
}

const char* worker_last_exception(worker* w) {
  return CopyString(w->last_exception);
}

int worker_load_module(worker* w, char* url_s) {
  Locker locker(w->isolate);
  Isolate::Scope isolate_scope(w->isolate);
  HandleScope handle_scope(w->isolate);

  Local<Context> context = Local<Context>::New(w->isolate, w->context);
  Context::Scope context_scope(context);
  TryCatch try_catch(w->isolate);

  Local<String> url = String::NewFromUtf8(w->isolate, url_s);
  MaybeLocal<Module> mod;
  LoadModule(w, context, url, mod);

  Local<Module> module;
  if (!mod.ToLocal(&module)) {
    w->last_exception = ExceptionString(w->isolate, context, &try_catch);
    return 1;
  }

  MaybeLocal<Value> maybe_result;
  if (!module->InstantiateModule(context, ResolveModuleCallback)
           .FromMaybe(false)) {
    w->last_exception = ExceptionString(w->isolate, context, &try_catch);
    return 2;
  }

  maybe_result = module->Evaluate(context);
  Local<Value> result;
  if (!maybe_result.ToLocal(&result)) {
    w->last_exception = ExceptionString(w->isolate, context, &try_catch);
    return 3;
  }

  return 0;
}

int worker_load_script(worker* w, char* name_s, char* source_s) {
  Locker locker(w->isolate);
  Isolate::Scope isolate_scope(w->isolate);
  HandleScope handle_scope(w->isolate);

  Local<Context> context = Local<Context>::New(w->isolate, w->context);
  Context::Scope context_scope(context);

  TryCatch try_catch(w->isolate);

  Local<String> name = String::NewFromUtf8(w->isolate, name_s);
  Local<String> source = String::NewFromUtf8(w->isolate, source_s);

  ScriptOrigin origin(name);

  Local<Script> script = Script::Compile(source, &origin);

  if (script.IsEmpty()) {
    assert(try_catch.HasCaught());
    w->last_exception = ExceptionString(w->isolate, context, &try_catch);
    return 1;
  }

  Handle<Value> result = script->Run();

  if (result.IsEmpty()) {
    assert(try_catch.HasCaught());
    w->last_exception = ExceptionString(w->isolate, context, &try_catch);
    return 2;
  }

  return 0;
}

worker* worker_init(int id, int enable_print) {
  worker* w = new (worker);

  Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      ArrayBuffer::Allocator::NewDefaultAllocator();
  Isolate* isolate = Isolate::New(create_params);
  Locker locker(isolate);
  Isolate::Scope isolate_scope(isolate);
  HandleScope handle_scope(isolate);

  w->isolate = isolate;
  w->isolate->SetCaptureStackTraceForUncaughtExceptions(true);
  w->isolate->SetData(0, w);
  w->id = id;

  Local<ObjectTemplate> global = ObjectTemplate::New(w->isolate);

  if (enable_print) {
    global->Set(String::NewFromUtf8(w->isolate, "$print"),
                FunctionTemplate::New(w->isolate, Print));
  }

  global->Set(String::NewFromUtf8(w->isolate, "$recv"),
              FunctionTemplate::New(w->isolate, Recv));

  global->Set(String::NewFromUtf8(w->isolate, "$send"),
              FunctionTemplate::New(w->isolate, Send));

  global->Set(String::NewFromUtf8(w->isolate, "$sendSync"),
              FunctionTemplate::New(w->isolate, SendSync));

  global->Set(String::NewFromUtf8(w->isolate, "$recvSync"),
              FunctionTemplate::New(w->isolate, RecvSync));

  Local<Context> context = Context::New(w->isolate, NULL, global);
  w->context.Reset(w->isolate, context);

  InitModuleData(context);
  return w;
}

// Called from Go to send messages to JavaScript. It will call the callback
// registered with $recv. A non-zero return value indicates error. Check
// worker_last_exception().
int worker_send(worker* w, const char* msg) {
  Locker locker(w->isolate);
  Isolate::Scope isolate_scope(w->isolate);
  HandleScope handle_scope(w->isolate);

  Local<Context> context = Local<Context>::New(w->isolate, w->context);
  Context::Scope context_scope(context);

  TryCatch try_catch(w->isolate);

  Local<Function> recv = Local<Function>::New(w->isolate, w->recv);
  if (recv.IsEmpty()) {
    w->last_exception = "v8worker: callback not registered with $recv";
    return 1;
  }

  Local<Value> args[1];
  args[0] = String::NewFromUtf8(w->isolate, msg);

  assert(!try_catch.HasCaught());

  recv->Call(context->Global(), 1, args);

  if (try_catch.HasCaught()) {
    w->last_exception = ExceptionString(w->isolate, context, &try_catch);
    return 2;
  }

  return 0;
}

// Called from Go to send messages to JavaScript. It will call the callback
// registered with $recvSync and return its string value.
const char* worker_send_sync(worker* w, const char* msg) {
  std::string out;
  Locker locker(w->isolate);
  Isolate::Scope isolate_scope(w->isolate);
  HandleScope handle_scope(w->isolate);

  Local<Context> context = Local<Context>::New(w->isolate, w->context);
  Context::Scope context_scope(context);

  Local<Function> recv_sync_handler =
      Local<Function>::New(w->isolate, w->recv_sync_handler);
  if (recv_sync_handler.IsEmpty()) {
    out.append("v8worker: callback not registered with $recvSync");
    return CopyString(out);
  }

  Local<Value> args[1];
  args[0] = String::NewFromUtf8(w->isolate, msg);
  Local<Value> response_value =
      recv_sync_handler->Call(context->Global(), 1, args);

  if (response_value->IsString()) {
    String::Utf8Value response(response_value->ToString());
    out.append(*response);
  } else {
    out.append("v8worker: non-string return value");
  }
  return CopyString(out);
}

void worker_terminate_execution(worker* w) {
  w->isolate->TerminateExecution();
}

const char* worker_version() {
  return V8::GetVersion();
}
}

/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "fxScripting.h"
#include <ManifestVersion.h>

#include <ResourceCallbackComponent.h>

#ifdef IS_FXSERVER
#include <ServerInstanceBase.h>
#include <ServerInstanceBaseRef.h>
#endif

#include <chrono>
#include <sstream>
#include <stack>
#include <stdexcept>

#include <CoreConsole.h>
#include <SharedFunction.h>

#include "fxScriptBuffer.h"
#include <ScriptInvoker.h>

#include <v8-version.h>

using namespace fx::invoker;

#ifndef IS_FXSERVER
#include <CL2LaunchMode.h>
#include <CfxSubProcess.h>
#endif

#include <rapidjson/writer.h>
#include <MsgpackJson.h>

inline static std::chrono::milliseconds msec()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch());
}

// This copies behavior from mono
#ifdef WIN32
#include <windows.h>
// Gets scaled (max * 0.9) max memory in bytes.
size_t GetScaledPhysicalMemorySize()
{
	MEMORYSTATUSEX status;
	status.dwLength = sizeof(status);
	GlobalMemoryStatusEx(&status);
	return status.ullTotalPhys * 0.9;
}
#else 
#include <unistd.h>
// Gets scaled (max * 0.9) max memory in bytes.
size_t GetScaledPhysicalMemorySize()
{
	long pages = sysconf(_SC_PHYS_PAGES);
	long pageSize = sysconf(_SC_PAGE_SIZE);

	return (pages * pageSize) * 0.9;
}
#endif


inline bool UseNode()
{
#ifndef IS_FXSERVER
	// ensuring client doesn't accidentally run node
	return launch::IsSDK();
#else
	return true;
#endif
}

inline bool UseThis()
{
#ifndef V8_NODE
	if (UseNode())
	{
		return false;
	}
#else
	if (!UseNode())
	{
		return false;
	}
#endif

	return true;
}

#ifndef IS_FXSERVER
static constexpr std::pair<const char*, ManifestVersion> g_scriptVersionPairs[] = {
#if defined(IS_RDR3)
	{ "rdr3_universal.js", guid_t{ 0 } }
#else
	{ "natives_universal.js", guid_t{ 0 } }
#endif
};
#else
static constexpr std::pair<const char*, ManifestVersion> g_scriptVersionPairs[] = {
	{ "natives_server.js", guid_t{ 0 } }
};
#endif

#include <v8.h>
#include <v8-profiler.h>
#include <libplatform/libplatform.h>

#include <boost/algorithm/string.hpp>

#ifdef V8_NODE
#include <node.h>
#endif

#include "UvLoopManager.h"

#include <V8Debugger.h>

#include <om/OMComponent.h>

#include <Resource.h>

#include <fstream>

#include <tbb/concurrent_queue.h>

#include <Error.h>

extern int g_argc;
extern char** g_argv;

static const char* g_platformScripts[] = {
	"citizen:/scripting/v8/console.js",
	"citizen:/scripting/v8/timer.js",
	"citizen:/scripting/v8/msgpack.js",
	"citizen:/scripting/v8/eventemitter2.js",
	"citizen:/scripting/v8/main.js"
};

using namespace v8;

namespace fx
{
struct V8Boundary
{
	int hint;
};

Isolate* GetV8Isolate();

static Platform* GetV8Platform();

#ifdef V8_NODE
static node::IsolateData* GetNodeIsolate();
#endif

// this is *technically* per-isolate, but we only register the callback for our host isolate
static std::atomic<int> g_isV8InGc;

class V8ScriptRuntime : public OMClass<V8ScriptRuntime, IScriptRuntime, IScriptFileHandlingRuntime, IScriptTickRuntime, IScriptEventRuntime, IScriptRefRuntime, IScriptStackWalkingRuntime, IScriptWarningRuntime>
{
private:
	typedef std::function<void(const char*, const char*, size_t, const char*)> TEventRoutine;

	typedef std::function<fx::OMPtr<IScriptBuffer>(int32_t, const char*, size_t)> TCallRefRoutine;

	typedef std::function<int32_t(int32_t)> TDuplicateRefRoutine;

	typedef std::function<void(int32_t)> TDeleteRefRoutine;

	typedef std::function<void(void*, void*, char**, size_t*)> TStackTraceRoutine;

	using TUnhandledPromiseRejectionRoutine = std::function<void(v8::PromiseRejectMessage&)>;

private:
	UniquePersistent<Context> m_context;

	std::unique_ptr<v8::MicrotaskQueue> m_taskQueue;

#ifdef V8_NODE
	node::Environment* m_nodeEnvironment;
#endif

	std::function<void()> m_tickRoutine;

	TEventRoutine m_eventRoutine;

	TCallRefRoutine m_callRefRoutine;

	TDuplicateRefRoutine m_duplicateRefRoutine;

	TDeleteRefRoutine m_deleteRefRoutine;

	IScriptHost* m_scriptHost;

	IScriptHostWithResourceData* m_resourceHost;

	IScriptHostWithManifest* m_manifestHost;

	int m_instanceId;

	TStackTraceRoutine m_stackTraceRoutine;

	TUnhandledPromiseRejectionRoutine m_unhandledPromiseRejectionRoutine;

	void* m_parentObject;

	// string values, which need to be persisted across calls as well
	std::unique_ptr<String::Utf8Value> m_stringValues[50];

	int m_curStringValue;

private:
	result_t LoadFileInternal(OMPtr<fxIStream> stream, char* scriptFile, Local<Script>* outScript);

	result_t LoadHostFileInternal(char* scriptFile, Local<Script>* outScript);

	result_t LoadSystemFileInternal(char* scriptFile, Local<Script>* outScript);

	result_t RunFileInternal(char* scriptFile, std::function<result_t(char*, Local<Script>*)> loadFunction);

	result_t LoadSystemFile(char* scriptFile);

public:
	inline V8ScriptRuntime()
	{
		m_instanceId = rand() ^ 0x3e3;
		m_curStringValue = 0;
	}

	static const OMPtr<V8ScriptRuntime>& GetCurrent();

	inline Local<Context> GetContext()
	{
		return m_context.Get(GetV8Isolate());
	}

	inline void SetTickRoutine(const std::function<void()>& tickRoutine)
	{
		if (!m_tickRoutine)
		{
			m_tickRoutine = tickRoutine;
		}
	}

	inline void SetEventRoutine(const TEventRoutine& eventRoutine)
	{
		if (!m_eventRoutine)
		{
			m_eventRoutine = eventRoutine;
		}
	}

	inline void SetCallRefRoutine(const TCallRefRoutine& routine)
	{
		if (!m_callRefRoutine)
		{
			m_callRefRoutine = routine;
		}
	}

	inline void SetDuplicateRefRoutine(const TDuplicateRefRoutine& routine)
	{
		if (!m_duplicateRefRoutine)
		{
			m_duplicateRefRoutine = routine;
		}
	}

	inline void SetDeleteRefRoutine(const TDeleteRefRoutine& routine)
	{
		if (!m_deleteRefRoutine)
		{
			m_deleteRefRoutine = routine;
		}
	}

	inline void SetStackTraceRoutine(const TStackTraceRoutine& routine)
	{
		if (!m_stackTraceRoutine)
		{
			m_stackTraceRoutine = routine;
		}
	}

	void HandlePromiseRejection(v8::PromiseRejectMessage& message);

	inline void SetUnhandledPromiseRejectionRoutine(const TUnhandledPromiseRejectionRoutine& routine)
	{
		if (!m_unhandledPromiseRejectionRoutine)
		{
			m_unhandledPromiseRejectionRoutine = routine;
		}
	}

	inline OMPtr<IScriptHost> GetScriptHost()
	{
		return m_scriptHost;
	}

	inline const char* GetResourceName()
	{
		char* resourceName = "";
		m_resourceHost->GetResourceName(&resourceName);

		return resourceName;
	}

	void RunMicrotasks()
	{
		if (m_taskQueue)
		{
			if (g_isV8InGc == 0)
			{
				m_taskQueue->PerformCheckpoint(GetV8Isolate());
			}
		}
	}

	const char* AssignStringValue(const Local<Value>& value, size_t* length);

	NS_DECL_ISCRIPTRUNTIME;

	NS_DECL_ISCRIPTFILEHANDLINGRUNTIME;

	NS_DECL_ISCRIPTTICKRUNTIME;

	NS_DECL_ISCRIPTEVENTRUNTIME;

	NS_DECL_ISCRIPTREFRUNTIME;

	NS_DECL_ISCRIPTSTACKWALKINGRUNTIME;

	NS_DECL_ISCRIPTWARNINGRUNTIME;
};

static Local<Value> GetStackTrace(TryCatch& eh, V8ScriptRuntime* runtime)
{
	auto stackTraceHandle = eh.StackTrace(runtime->GetContext());

	if (stackTraceHandle.IsEmpty())
	{
		return String::NewFromUtf8(GetV8Isolate(), "<empty stack trace>").ToLocalChecked();
	}

	return stackTraceHandle.ToLocalChecked();
}

static thread_local OMPtr<V8ScriptRuntime> g_currentV8Runtime;

class FxMicrotaskScope
{
public:
	inline FxMicrotaskScope(V8ScriptRuntime* runtime)
		: m_runtime(runtime)
	{
	}

	inline ~FxMicrotaskScope()
	{
		m_runtime->RunMicrotasks();
	}

private:
	V8ScriptRuntime* m_runtime;
};

class ScopeDtor
{
public:
	explicit ScopeDtor(std::function<void()>&& fn)
		: fn(fn)
	{
	}

	~ScopeDtor()
	{
		fn();
	}

private:
	std::function<void()> fn;
};

template<typename TLocker = v8::Locker, typename TIsolateScope = v8::Isolate::Scope>
class V8PushEnvironment
{
private:
	TLocker m_locker;

	TIsolateScope m_isolateScope;

	fx::PushEnvironment m_pushEnvironment;

	HandleScope m_handleScope;

	Context::Scope m_contextScope;

	OMPtr<V8ScriptRuntime> m_lastV8Runtime;

	ScopeDtor m_scoped;

	FxMicrotaskScope m_microtaskScope;

public:
	inline V8PushEnvironment(V8ScriptRuntime* runtime)
		: m_locker(GetV8Isolate()), m_isolateScope(GetV8Isolate()), m_pushEnvironment(runtime), m_handleScope(GetV8Isolate()), m_contextScope(runtime->GetContext()), m_microtaskScope(runtime), m_scoped([this]()
		{
			g_currentV8Runtime = m_lastV8Runtime;
		})
	{
		m_lastV8Runtime = g_currentV8Runtime;
		g_currentV8Runtime = runtime;
	}

	inline ~V8PushEnvironment()
	{
	}
};

#ifdef V8_NODE
static std::unordered_map<const node::Environment*, V8ScriptRuntime*> g_envRuntimes;

static V8ScriptRuntime* GetEnvRuntime(const node::Environment* env)
{
	return g_envRuntimes[env];
}
#endif

class BasePushEnvironment
{
public:
	virtual ~BasePushEnvironment() = default;
};

#ifdef V8_NODE
class V8LitePushEnvironment : public BasePushEnvironment
{
private:
	Locker m_locker;

	Isolate::Scope m_isolateScope;

	fx::PushEnvironment m_pushEnvironment;

	OMPtr<V8ScriptRuntime> m_lastV8Runtime;

	ScopeDtor m_scoped;

	FxMicrotaskScope m_microtaskScope;

public:
	inline V8LitePushEnvironment(V8ScriptRuntime* runtime, const node::Environment* env)
		: m_locker(node::GetIsolate(env)), m_isolateScope(node::GetIsolate(env)), m_pushEnvironment(runtime), m_microtaskScope(runtime), m_scoped([this]()
		{
			g_currentV8Runtime = m_lastV8Runtime;
		})
	{
		m_lastV8Runtime = g_currentV8Runtime;
		g_currentV8Runtime = runtime;
	}

	inline V8LitePushEnvironment(PushEnvironment&& pushEnv, V8ScriptRuntime* runtime, const node::Environment* env)
		: m_locker(node::GetIsolate(env)), m_isolateScope(node::GetIsolate(env)), m_pushEnvironment(std::move(pushEnv)), m_microtaskScope(runtime), m_scoped([this]()
		{
			g_currentV8Runtime = m_lastV8Runtime;
		})
	{
		m_lastV8Runtime = g_currentV8Runtime;
		g_currentV8Runtime = runtime;
	}

	inline ~V8LitePushEnvironment()
	{
	}
};

class V8LiteNoRuntimePushEnvironment : public BasePushEnvironment
{
private:
	Locker m_locker;

	Isolate::Scope m_isolateScope;

public:
	inline V8LiteNoRuntimePushEnvironment(const node::Environment* env)
		: m_locker(node::GetIsolate(env)), m_isolateScope(node::GetIsolate(env))
	{
	}
};

class V8NoopPushEnvironment : public BasePushEnvironment
{
public:
	inline ~V8NoopPushEnvironment()
	{
	}
};
#endif

static V8ScriptRuntime* GetScriptRuntimeFromArgs(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	auto external = Local<External>::Cast(args.Data());

	return reinterpret_cast<V8ScriptRuntime*>(external->Value());
}

const OMPtr<V8ScriptRuntime>& V8ScriptRuntime::GetCurrent()
{
#if _DEBUG
	V8ScriptRuntime* v8Runtime;
	OMPtr<IScriptRuntime> runtime;

	assert(FX_SUCCEEDED(fx::GetCurrentScriptRuntime(&runtime)));
	assert(v8Runtime = dynamic_cast<V8ScriptRuntime*>(runtime.GetRef()));

	assert(v8Runtime == g_currentV8Runtime.GetRef());
#endif

	return g_currentV8Runtime;
}

void ScriptTraceV(const char* string, fmt::printf_args formatList)
{
	auto t = fmt::vsprintf(string, formatList);
	console::Printf(fmt::sprintf("script:%s", V8ScriptRuntime::GetCurrent()->GetResourceName()), "%s", t);

	V8ScriptRuntime::GetCurrent()->GetScriptHost()->ScriptTrace(const_cast<char*>(t.c_str()));
}

template<typename... TArgs>
void ScriptTrace(const char* string, const TArgs& ... args)
{
	ScriptTraceV(string, fmt::make_printf_args(args...));
}

static void V8_SetTickFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	Local<Function> tickFunction = Local<Function>::Cast(args[0]);
	UniquePersistent<Function> tickFunctionRef(GetV8Isolate(), tickFunction);

	runtime->SetTickRoutine(make_shared_function([runtime, tickFunctionRef{ std::move(tickFunctionRef) }] ()
	{
		Local<Function> tickFunction = tickFunctionRef.Get(GetV8Isolate());

		{
			TryCatch eh(GetV8Isolate());

			auto time = v8::Number::New(GetV8Isolate(), (double)msec().count());
			v8::Local<v8::Value> args[] = {
				time.As<v8::Value>()
			};

			MaybeLocal<Value> value = tickFunction->Call(runtime->GetContext(), Null(GetV8Isolate()), std::size(args), args);

			if (value.IsEmpty())
			{
				String::Utf8Value str(GetV8Isolate(), eh.Exception());
				String::Utf8Value stack(GetV8Isolate(), GetStackTrace(eh, runtime));

				ScriptTrace("Error calling system tick function in resource %s: %s\nstack:\n%s\n", runtime->GetResourceName(), *str, *stack);
			}
		}
	}));
}

static void V8_SetEventFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	Local<Function> function = Local<Function>::Cast(args[0]);
	UniquePersistent<Function> functionRef(GetV8Isolate(), function);

	runtime->SetEventRoutine(make_shared_function([runtime, functionRef{ std::move(functionRef) }](const char* eventName, const char* eventPayload, size_t payloadSize, const char* eventSource)
	{
		Local<Function> function = functionRef.Get(GetV8Isolate());

		{
			TryCatch eh(GetV8Isolate());

			Local<ArrayBuffer> inValueBuffer = ArrayBuffer::New(GetV8Isolate(), payloadSize);
			auto abs = inValueBuffer->GetBackingStore();
			memcpy(abs->Data(), eventPayload, payloadSize);

			Local<Value> arguments[3];
			arguments[0] = String::NewFromUtf8(GetV8Isolate(), eventName).ToLocalChecked();
			arguments[1] = Uint8Array::New(inValueBuffer, 0, payloadSize);
			arguments[2] = String::NewFromUtf8(GetV8Isolate(), eventSource).ToLocalChecked();

			MaybeLocal<Value> value = function->Call(runtime->GetContext(), Null(GetV8Isolate()), 3, arguments);

			if (eh.HasCaught())
			{
				String::Utf8Value str(GetV8Isolate(), eh.Exception());
				String::Utf8Value stack(GetV8Isolate(), GetStackTrace(eh, runtime));

				ScriptTrace("Error calling system event handling function in resource %s: %s\nstack:\n%s\n", runtime->GetResourceName(), *str, *stack);
			}
		}
	}));
}

static void V8_SetCallRefFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	Local<Function> function = Local<Function>::Cast(args[0]);
	UniquePersistent<Function> functionRef(GetV8Isolate(), function);

	runtime->SetCallRefRoutine(make_shared_function([runtime, functionRef{ std::move(functionRef) }](int32_t refId, const char* argsSerialized, size_t argsSize)
	{
		Local<Function> function = functionRef.Get(GetV8Isolate());

		{
			TryCatch eh(GetV8Isolate());

			Local<ArrayBuffer> inValueBuffer = ArrayBuffer::New(GetV8Isolate(), argsSize);
			auto abs = inValueBuffer->GetBackingStore();
			memcpy(abs->Data(), argsSerialized, argsSize);

			Local<Value> arguments[2];
			arguments[0] = Int32::New(GetV8Isolate(), refId);
			arguments[1] = Uint8Array::New(inValueBuffer, 0, argsSize);

			MaybeLocal<Value> maybeValue = function->Call(runtime->GetContext(), Null(GetV8Isolate()), 2, arguments);

			fx::OMPtr<IScriptBuffer> rv;

			if (eh.HasCaught())
			{
				String::Utf8Value str(GetV8Isolate(), eh.Exception());
				String::Utf8Value stack(GetV8Isolate(), GetStackTrace(eh, runtime));

				ScriptTrace("Error calling system call ref function in resource %s: %s\nstack:\n%s\n", runtime->GetResourceName(), *str, *stack);
			}
			else
			{
				Local<Value> value = maybeValue.ToLocalChecked();

				if (value->IsArrayBufferView())
				{
					Local<ArrayBufferView> abv = value.As<ArrayBufferView>();
					rv = fx::MemoryScriptBuffer::Make(abv->ByteLength());
					if (rv.GetRef() && rv->GetBytes())
					{
						abv->CopyContents(rv->GetBytes(), abv->ByteLength());
					}
				}
			}

			return rv;
		}
	}));
}

static void V8_SetDeleteRefFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	Local<Function> function = Local<Function>::Cast(args[0]);
	UniquePersistent<Function> functionRef(GetV8Isolate(), function);

	runtime->SetDeleteRefRoutine(make_shared_function([runtime, functionRef{ std::move(functionRef) }](int32_t refId)
	{
		Local<Function> function = functionRef.Get(GetV8Isolate());

		{
			TryCatch eh(GetV8Isolate());

			Local<Value> arguments[3];
			arguments[0] = Int32::New(GetV8Isolate(), refId);

			MaybeLocal<Value> value = function->Call(runtime->GetContext(), Null(GetV8Isolate()), 1, arguments);

			if (eh.HasCaught())
			{
				String::Utf8Value str(GetV8Isolate(), eh.Exception());
				String::Utf8Value stack(GetV8Isolate(), GetStackTrace(eh, runtime));

				ScriptTrace("Error calling system delete ref function in resource %s: %s\nstack:\n%s\n", runtime->GetResourceName(), *str, *stack);
			}
		}
	}));
}

static void V8_SetDuplicateRefFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	Local<Function> function = Local<Function>::Cast(args[0]);
	UniquePersistent<Function> functionRef(GetV8Isolate(), function);

	runtime->SetDuplicateRefRoutine(make_shared_function([runtime, functionRef{ std::move(functionRef) }](int32_t refId)
	{
		Local<Function> function = functionRef.Get(GetV8Isolate());

		{
			TryCatch eh(GetV8Isolate());

			Local<Value> arguments[3];
			arguments[0] = Int32::New(GetV8Isolate(), refId);

			MaybeLocal<Value> value = function->Call(runtime->GetContext(), Null(GetV8Isolate()), 1, arguments);

			if (eh.HasCaught())
			{
				String::Utf8Value str(GetV8Isolate(), eh.Exception());
				String::Utf8Value stack(GetV8Isolate(), GetStackTrace(eh, runtime));

				ScriptTrace("Error calling system duplicate ref function in resource %s: %s\nstack:\n%s\n", runtime->GetResourceName(), *str, *stack);
			}
			else
			{
				Local<Value> realValue;

				if (value.ToLocal(&realValue))
				{
					if (realValue->IsInt32())
					{
						return realValue->Int32Value(runtime->GetContext()).ToChecked();
					}
				}
			}

			return -1;
		}
	}));
}

static void V8_SetStackTraceRoutine(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	Local<Function> function = Local<Function>::Cast(args[0]);
	UniquePersistent<Function> functionRef(GetV8Isolate(), function);

	runtime->SetStackTraceRoutine(make_shared_function([runtime, functionRef{ std::move(functionRef) }](void* start, void* end, char** blob, size_t* size)
	{
		// static array for retval output (sadly)
		static std::vector<char> retvalArray(32768);

		Local<Function> function = functionRef.Get(GetV8Isolate());

		{
			TryCatch eh(GetV8Isolate());

			Local<Value> arguments[2];

			// push arguments on the stack
			if (start)
			{
				auto startRef = (V8Boundary*)start;
				arguments[0] = Int32::New(GetV8Isolate(), startRef->hint);
			}
			else
			{
				arguments[0] = Null(GetV8Isolate());
			}

			if (end)
			{
				auto endRef = (V8Boundary*)end;
				arguments[1] = Int32::New(GetV8Isolate(), endRef->hint);
			}
			else
			{
				arguments[1] = Null(GetV8Isolate());
			}

			MaybeLocal<Value> mv = function->Call(runtime->GetContext(), Null(GetV8Isolate()), 2, arguments);

			if (eh.HasCaught())
			{
				String::Utf8Value str(GetV8Isolate(), eh.Exception());
				String::Utf8Value stack(GetV8Isolate(), GetStackTrace(eh, runtime));

				ScriptTrace("Error calling system stack trace function in resource %s: %s\nstack:\n%s\n", runtime->GetResourceName(), *str, *stack);
			}
			else
			{
				Local<Value> value = mv.ToLocalChecked();

				if (!value->IsArrayBufferView())
				{
					return;
				}

				Local<ArrayBufferView> abv = value.As<ArrayBufferView>();
				*size = abv->ByteLength();

				if (*size > retvalArray.size())
				{
					retvalArray.resize(*size);
				}

				abv->CopyContents(retvalArray.data(), fwMin(retvalArray.size(), *size));
				*blob = retvalArray.data();
			}
		}
	}));
}

static void V8_SetUnhandledPromiseRejectionRoutine(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	Local<Function> function = Local<Function>::Cast(args[0]);
	UniquePersistent<Function> functionRef(GetV8Isolate(), function);

	runtime->SetUnhandledPromiseRejectionRoutine(make_shared_function([runtime, functionRef{ std::move(functionRef) }](v8::PromiseRejectMessage& message)
	{
		Local<Promise> promise = message.GetPromise();
		Isolate* isolate = promise->GetIsolate();
		Local<Value> value = message.GetValue();
		Local<Integer> event = Integer::New(isolate, message.GetEvent());

		if (value.IsEmpty())
		{
			value = Undefined(isolate);
		}

		Local<Function> function = functionRef.Get(GetV8Isolate());

		{
			TryCatch eh(GetV8Isolate());

			auto time = v8::Number::New(GetV8Isolate(), (double)msec().count());
			v8::Local<v8::Value> args[] = {
				event, promise, value
			};

			MaybeLocal<Value> value = function->Call(runtime->GetContext(), Null(GetV8Isolate()), std::size(args), args);

			if (value.IsEmpty())
			{
				String::Utf8Value str(GetV8Isolate(), eh.Exception());
				String::Utf8Value stack(GetV8Isolate(), GetStackTrace(eh, runtime));

				ScriptTrace("Unhandled error during handling of unhandled promise rejection in resource %s: %s\nstack:\n%s\n", runtime->GetResourceName(), *str, *stack);
			}
		}
	}));
}

void V8ScriptRuntime::HandlePromiseRejection(v8::PromiseRejectMessage& message)
{
	if (m_unhandledPromiseRejectionRoutine)
	{
		m_unhandledPromiseRejectionRoutine(message);
	}
}

static void V8_CanonicalizeRef(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	char* refString;
	result_t hr = runtime->GetScriptHost()->CanonicalizeRef(args[0]->Int32Value(runtime->GetContext()).ToChecked(), runtime->GetInstanceId(), &refString);

	args.GetReturnValue().Set(String::NewFromUtf8(GetV8Isolate(), refString).ToLocalChecked());
	fwFree(refString);
}

struct RefAndPersistent {
	fx::FunctionRef ref;
	Persistent<Function> handle;
	fx::OMPtr<V8ScriptRuntime> runtime;
	fx::OMPtr<IScriptHost> host;
};

static tbb::concurrent_queue<RefAndPersistent*> g_cleanUpFuncRefs;

static void V8_InvokeFunctionReference(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	auto external = Local<External>::Cast(args.Data());
	auto refData = reinterpret_cast<RefAndPersistent*>(external->Value());

	OMPtr<IScriptHost> scriptHost = refData->runtime->GetScriptHost();

	Local<ArrayBufferView> abv = Local<ArrayBufferView>::Cast(args[0]);

	std::vector<uint8_t> argsBuffer(abv->ByteLength());

	abv->CopyContents(argsBuffer.data(), argsBuffer.size());

	fx::OMPtr<IScriptBuffer> retvalBuffer;
	if (FX_FAILED(scriptHost->InvokeFunctionReference(const_cast<char*>(refData->ref.GetRef().c_str()), reinterpret_cast<char*>(argsBuffer.data()), argsBuffer.size(), retvalBuffer.GetAddressOf())))
	{
		char* error = "Unknown";
		scriptHost->GetLastErrorText(&error);

		auto throwException = [&](const std::string& exceptionString)
		{
			args.GetIsolate()->ThrowException(Exception::Error(String::NewFromUtf8(args.GetIsolate(), exceptionString.c_str()).ToLocalChecked()));
		};

		return throwException(error);
	}
	
	size_t retLength = (retvalBuffer.GetRef()) ? retvalBuffer->GetLength() : 0;

	// get return values
	Local<ArrayBuffer> outValueBuffer = ArrayBuffer::New(GetV8Isolate(), retLength);
	if (retLength > 0)
	{
		auto abs = outValueBuffer->GetBackingStore();
		memcpy(abs->Data(), retvalBuffer->GetBytes(), retLength);
	}

	Local<Uint8Array> outArray = Uint8Array::New(outValueBuffer, 0, retLength);
	args.GetReturnValue().Set(outArray);
}

static void FnRefWeakCallback(
	const v8::WeakCallbackInfo<RefAndPersistent>& data)
{
	v8::HandleScope handleScope(data.GetIsolate());
	v8::Local<Function> v = data.GetParameter()->handle.Get(data.GetIsolate());

	data.GetParameter()->handle.Reset();

	// defer this to the next tick so that we won't end up deadlocking (the V8 lock is held at GC interrupt time, but the runtime lock is not)
	g_cleanUpFuncRefs.push(data.GetParameter());
}

static void V8_MakeFunctionReference(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	std::string refString;

	if (args[0]->IsString())
	{
		Local<String> string = Local<String>::Cast(args[0]);
		String::Utf8Value utf8{ GetV8Isolate(), string };
		refString = *utf8;
	}
	else if (args[0]->IsUint8Array())
	{
		Local<Uint8Array> arr = Local<Uint8Array>::Cast(args[0]);

		std::vector<uint8_t> data(arr->ByteLength());
		arr->CopyContents(data.data(), data.size());

		refString = std::string(reinterpret_cast<char*>(data.data()), data.size());
	}

	RefAndPersistent* data = new RefAndPersistent;
	data->ref = fx::FunctionRef{ refString };
	data->runtime = runtime;
	data->host = runtime->GetScriptHost();

	MaybeLocal<Function> outFunction = Function::New(runtime->GetContext(), V8_InvokeFunctionReference, External::New(GetV8Isolate(), data));

	Local<Function> outFn;
	
	if (outFunction.ToLocal(&outFn))
	{
		data->handle.Reset(GetV8Isolate(), outFn);
		data->handle.SetWeak(data, FnRefWeakCallback, v8::WeakCallbackType::kParameter);

		args.GetReturnValue().Set(outFn);
	}
	else
	{
		delete data;
	}
}

static void V8_GetTickCount(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	args.GetReturnValue().Set((double)msec().count());
}

const char* V8ScriptRuntime::AssignStringValue(const Local<Value>& value, size_t* length)
{
	auto stringValue = std::make_unique<String::Utf8Value>(GetV8Isolate(), value);
	
	const char* str = **(stringValue.get());
	*length = stringValue->length();

	// take ownership
	m_stringValues[m_curStringValue] = std::move(stringValue);
	
	// increment the string value
	m_curStringValue = (m_curStringValue + 1) % _countof(m_stringValues);

	// return the string
	return str;
}

struct V8ScriptNativeContext final : ScriptNativeContext
{
	V8ScriptNativeContext(uint64_t hash, V8ScriptRuntime* runtime, v8::Isolate* isolate);

	void PushArgument(v8::Local<v8::Value> arg);

	template<typename T>
	v8::Local<v8::Value> ProcessResult(const T& value);

	v8::Isolate::Scope isolateScope;
	V8ScriptRuntime* runtime;
	v8::Isolate* isolate;
	v8::Local<v8::Context> cxt;
};

inline V8ScriptNativeContext::V8ScriptNativeContext(uint64_t hash, V8ScriptRuntime* runtime, v8::Isolate* isolate)
	: ScriptNativeContext(hash), isolateScope(GetV8Isolate()), runtime(runtime), isolate(isolate), cxt(runtime->GetContext())
{
}

void V8ScriptNativeContext::PushArgument(v8::Local<v8::Value> arg)
{
	if (arg->IsNumber())
	{
		double value = arg->NumberValue(cxt).ToChecked();
		int64_t intValue = static_cast<int64_t>(value);

		if (intValue == value)
		{
			return Push(intValue);
		}
		else
		{
			return Push(value);
		}
	}
	else if (arg->IsBoolean() || arg->IsBooleanObject())
	{
		return Push(arg->BooleanValue(isolate));
	}
	else if (arg->IsString())
	{
		// TODO: Should these be marked as immutable?
		size_t length = 0;
		const char* data = runtime->AssignStringValue(arg, &length);
		return Push(data, length);
	}
	// null/undefined: add '0'
	else if (arg->IsNull() || arg->IsUndefined())
	{
		return Push(0);
	}
	// metafield
	else if (arg->IsExternal())
	{
		uint8_t* ptr = reinterpret_cast<uint8_t*>(Local<External>::Cast(arg)->Value());

		return PushMetaPointer(ptr);
	}
	// placeholder vectors
	else if (arg->IsArray())
	{
		Local<Array> array = Local<Array>::Cast(arg);
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

		auto getNumber = [&](int idx)
		{
			Local<Value> value;

			if (!array->Get(cxt, idx).ToLocal(&value))
			{
				return NAN;
			}

			if (value.IsEmpty() || !value->IsNumber())
			{
				return NAN;
			}

			return static_cast<float>(value->NumberValue(cxt).ToChecked());
		};

		if (array->Length() < 2 || array->Length() > 4)
		{
			ScriptError("arrays should be vectors (wrong number of values)");
		}

		if (array->Length() >= 2)
		{
			x = getNumber(0);
			y = getNumber(1);

			if (x == NAN || y == NAN)
			{
				ScriptError("invalid vector array value");
			}

			Push(x);
			Push(y);
		}

		if (array->Length() >= 3)
		{
			z = getNumber(2);

			if (z == NAN)
			{
				ScriptError("invalid vector array value");
			}

			Push(z);
		}

		if (array->Length() >= 4)
		{
			w = getNumber(3);

			if (w == NAN)
			{
				ScriptError("invalid vector array value");
			}

			Push(w);
		}
	}
	else if (arg->IsArrayBufferView())
	{
		Local<ArrayBufferView> abv = arg.As<ArrayBufferView>();
		Local<ArrayBuffer> buffer = abv->Buffer();

		auto abs = buffer->GetBackingStore();
		return Push((uint8_t*)abs->Data() + abv->ByteOffset(), abs->ByteLength());
	}
	// this should be the last entry, I'd guess
	else if (arg->IsObject())
	{
		Local<Object> object = arg->ToObject(cxt).ToLocalChecked();
		Local<Value> data;

		if (!object->Get(cxt, String::NewFromUtf8(GetV8Isolate(), "__data").ToLocalChecked()).ToLocal(&data))
		{
			ScriptError("__data field does not contain a number");
		}

		if (!data.IsEmpty() && data->IsNumber())
		{
			auto n = data->ToNumber(runtime->GetContext());
			v8::Local<v8::Number> number;

			if (n.ToLocal(&number))
			{
				return Push(number->Int32Value(cxt).ToChecked());
			}
		}
		else
		{
			ScriptError("__data field does not contain a number");
		}
	}
	else
	{
		String::Utf8Value str(GetV8Isolate(), arg);
		ScriptErrorf("invalid V8 value: %s", *str);
	}
}

template<typename T>
CSCRC_INLINE v8::Local<v8::Value> V8ScriptNativeContext::ProcessResult(const T& value)
{
	if constexpr (std::is_same_v<T, bool>)
	{
		return Boolean::New(isolate, value);
	}
	else if constexpr (std::is_same_v<T, int32_t>)
	{
		return Int32::New(isolate, value);
	}
	else if constexpr (std::is_same_v<T, int64_t>)
	{
		return Number::New(isolate, (double) value);
	}
	else if constexpr (std::is_same_v<T, float>)
	{
		return Number::New(isolate, value);
	}
	else if constexpr (std::is_same_v<T, ScrVector>)
	{
		Local<Array> result = Array::New(isolate, 3);

		result->Set(cxt, 0, Number::New(isolate, value.x));
		result->Set(cxt, 1, Number::New(isolate, value.y));
		result->Set(cxt, 2, Number::New(isolate, value.z));

		return result;
	}
	else if constexpr (std::is_same_v<T, const char*>)
	{
		if (value)
		{
			return String::NewFromUtf8(isolate, value).ToLocalChecked();
		}
		else
		{
			return Null(isolate);
		}
	}
	else if constexpr (std::is_same_v<T, ScrString>)
	{
		// FIXME: This wasn't properly supported before. Should it return a Uint8Buffer buffer instead?
		return String::NewFromUtf8(isolate, value.str, v8::NewStringType::kNormal, (int) value.len).ToLocalChecked();
	}
	else if constexpr (std::is_same_v<T, ScrObject>)
	{
		try
		{
			msgpack::unpacked unpacked;
			msgpack::unpack(unpacked, value.data, value.length);

			// and convert to a rapidjson object
			rapidjson::Document document;
			ConvertToJSON(unpacked.get(), document, document.GetAllocator());

			// write as a json string
			rapidjson::StringBuffer sb;
			rapidjson::Writer<rapidjson::StringBuffer> writer(sb);

			if (document.Accept(writer))
			{
				if (sb.GetString() && sb.GetSize())
				{
					auto maybeString = String::NewFromUtf8(GetV8Isolate(), sb.GetString(), NewStringType::kNormal, sb.GetSize());
					Local<String> string;

					if (maybeString.ToLocal(&string))
					{
						auto maybeValue = v8::JSON::Parse(runtime->GetContext(), string);
						Local<Value> value;

						if (maybeValue.ToLocal(&value))
						{
							return value;
						}
					}
				}
			}
		}
		catch (std::exception& /*e*/)
		{
		}

		return Null(GetV8Isolate());
	}
	else
	{
		static_assert(always_false_v<T>, "Invalid return type");
	}
}

static void V8_InvokeNative(const v8::FunctionCallbackInfo<v8::Value>& args, uint64_t hash, int baseArgs)
{
	// get required entries
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	// variables to hold state
	V8ScriptNativeContext context(hash, runtime, args.GetIsolate());

	// get argument count for the loop
	int numArgs = args.Length();

	for (int i = baseArgs; i < numArgs; i++)
	{
		context.PushArgument(args[i]);
	}

	context.Invoke();

	// For a single result, return it directly.
	// For multiple results, store them in an array.
	Local<Value> returnValue = Undefined(context.isolate);
	int numResults = 0;

	context.ProcessResults([&](auto&& value) {
		Local<Value> val = context.ProcessResult(value);

		if (numResults == 0)
		{
			returnValue = val;
		}
		else
		{
			if (numResults == 1)
			{
				Local<Array> arrayValue = Array::New(context.isolate);
				arrayValue->Set(context.cxt, 0, returnValue);
				returnValue = arrayValue;
			}

			returnValue.As<Array>()->Set(context.cxt, numResults, val);
		}

		++numResults;
	});

	// and set the return value(s)
	args.GetReturnValue().Set(returnValue);
}

template<typename Func>
static void V8_TryCatch(const v8::FunctionCallbackInfo<v8::Value>& args, Func&& func)
{
	v8::Local<v8::Value> exception;

	try
	{
		return func(args);
	}
	catch (const std::exception& ex)
	{
		exception = Exception::Error(String::NewFromUtf8(args.GetIsolate(), ex.what()).ToLocalChecked());
	}

	// Throw the error after cleaning up the stack
	args.GetIsolate()->ThrowException(exception);
}

static void V8_InvokeNativeString(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8_TryCatch(args, [] (const v8::FunctionCallbackInfo<v8::Value>& args) {
		if (args.Length() < 1)
		{
			throw std::runtime_error("wrong argument count (needs at least a hash string)");
		}

		String::Utf8Value hashString(GetV8Isolate(), args[0]);
		uint64_t hash = strtoull(*hashString, nullptr, 16);
		V8_InvokeNative(args, hash, 1);
	});
}

static void V8_InvokeNativeHash(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8_TryCatch(args, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
		if (args.Length() < 2)
		{
			throw std::runtime_error("wrong argument count (needs at least two hash integers)");
		}

		auto scrt = V8ScriptRuntime::GetCurrent();
		uint64_t hash = (args[1]->Uint32Value(scrt->GetContext()).ToChecked() | (((uint64_t)args[0]->Uint32Value(scrt->GetContext()).ToChecked()) << 32));
		V8_InvokeNative(args, hash, 2);
	});
}

template<MetaField field>
static void V8_GetMetaField(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	args.GetReturnValue().Set(External::New(GetV8Isolate(), ScriptNativeContext::GetMetaField(field)));
}

template<MetaField field>
static void V8_GetPointerField(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);

	uintptr_t value = 0;

	auto arg = args[0];

	if constexpr (field == MetaField::PointerValueInteger)
	{
		value = (uint64_t)arg->IntegerValue(runtime->GetContext()).ToChecked();
	}
	else if constexpr (field == MetaField::PointerValueFloat)
	{
		float fvalue = static_cast<float>(arg->NumberValue(runtime->GetContext()).ToChecked());
		value = *reinterpret_cast<uint32_t*>(&value);
	}
	
	args.GetReturnValue().Set(External::New(GetV8Isolate(), ScriptNativeContext::GetPointerField(field, value)));
}

std::string SaveProfileToString(CpuProfile* profile);
static thread_local CpuProfiler* g_cpuProfiler;

static void V8_StartProfiling(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (g_cpuProfiler)
	{
		return;
	}

	CpuProfiler* profiler = v8::CpuProfiler::New(args.GetIsolate());
	profiler->StartProfiling((args.Length() == 0) ? String::Empty(args.GetIsolate()) : Local<String>::Cast(args[0]), true);

	g_cpuProfiler = profiler;
}

static void V8_StopProfiling(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (!g_cpuProfiler)
	{
		return;
	}

	auto profiler = g_cpuProfiler;
	CpuProfile* profile = profiler->StopProfiling((args.Length() == 0) ? String::Empty(args.GetIsolate()) : Local<String>::Cast(args[0]));

	std::string jsonString = SaveProfileToString(profile);

	profile->Delete();

#ifdef _WIN32
	SYSTEMTIME systemTime;
	GetSystemTime(&systemTime);

	{
		std::ofstream stream(MakeRelativeCitPath(va(L"v8-%04d%02d%02d-%02d%02d%02d.cpuprofile", systemTime.wYear, systemTime.wMonth, systemTime.wDay, systemTime.wHour, systemTime.wMinute, systemTime.wSecond)));
		stream << jsonString;
	}
#endif

	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);
	args.GetReturnValue().Set(JSON::Parse(runtime->GetContext(), String::NewFromUtf8(args.GetIsolate(), jsonString.c_str(), NewStringType::kNormal, jsonString.size()).ToLocalChecked()).ToLocalChecked());

	g_cpuProfiler->Dispose();
	g_cpuProfiler = nullptr;
}

static void V8_Trace(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	bool first = true;

	for (int i = 0; i < args.Length(); i++)
	{
		Locker locker(args.GetIsolate());
		Isolate::Scope isolateScope(args.GetIsolate());
		v8::HandleScope handle_scope(args.GetIsolate());

		if (first)
		{
			first = false;
		}
		else
		{
			ScriptTrace(" ");
		}

		v8::String::Utf8Value str(GetV8Isolate(), args[i]);
		ScriptTrace("%s", std::string_view{ *str, size_t(str.length()) });
	}

	ScriptTrace("\n");
}

static void V8_GetResourcePath(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	if (UseNode())
	{
		V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);
		auto path = ((fx::Resource*)runtime->GetParentObject())->GetPath();

		args.GetReturnValue().Set(String::NewFromUtf8(args.GetIsolate(), path.c_str(), NewStringType::kNormal, path.size()).ToLocalChecked());
	}
}

static void V8_SubmitBoundaryStart(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	// get required entries
	auto scrt = GetScriptRuntimeFromArgs(args);
	auto scriptHost = scrt->GetScriptHost();

	auto val = args[0]->IntegerValue(scrt->GetContext());

	V8Boundary b;
	b.hint = val.ToChecked();

	scriptHost->SubmitBoundaryStart((char*)& b, sizeof(b));
}

static void V8_SubmitBoundaryEnd(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	// get required entries
	auto scrt = GetScriptRuntimeFromArgs(args);
	auto scriptHost = scrt->GetScriptHost();

	auto val = args[0]->IntegerValue(scrt->GetContext());

	V8Boundary b;
	b.hint = val.ToChecked();

	scriptHost->SubmitBoundaryEnd((char*)&b, sizeof(b));
}

class FileOutputStream : public v8::OutputStream {
public:
	FileOutputStream(FILE* stream) : stream_(stream) {}

	virtual int GetChunkSize() {
		return 65536;  // big chunks == faster
	}

	virtual void EndOfStream() {}

	virtual WriteResult WriteAsciiChunk(char* data, int size) {
		const size_t len = static_cast<size_t>(size);
		size_t off = 0;

		while (off < len && !feof(stream_) && !ferror(stream_))
			off += fwrite(data + off, 1, len - off, stream_);

		return off == len ? kContinue : kAbort;
	}

private:
	FILE* stream_;
};

static void V8_Snap(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	FILE* fp = fopen("snap.heapsnapshot", "w");
	if (fp == NULL) return;
	const v8::HeapSnapshot* const snap = v8::Isolate::GetCurrent()->GetHeapProfiler()->TakeHeapSnapshot();
	FileOutputStream stream(fp);
	snap->Serialize(&stream, v8::HeapSnapshot::kJSON);
	int err = 0;
	fclose(fp);
	// Work around a deficiency in the API.  The HeapSnapshot object is const
	// but we cannot call HeapProfiler::DeleteAllHeapSnapshots() because that
	// invalidates _all_ snapshots, including those created by other tools.
	const_cast<v8::HeapSnapshot*>(snap)->Delete();
}

static std::pair<std::string, FunctionCallback> g_citizenFunctions[] =
{
	{ "trace", V8_Trace },
	{ "setTickFunction", V8_SetTickFunction },
	{ "setEventFunction", V8_SetEventFunction },
	// ref stuff
	{ "setCallRefFunction", V8_SetCallRefFunction },
	{ "setDeleteRefFunction", V8_SetDeleteRefFunction },
	{ "setDuplicateRefFunction", V8_SetDuplicateRefFunction },
	{ "canonicalizeRef", V8_CanonicalizeRef },
	{ "makeFunctionReference", V8_MakeFunctionReference },
	// internals
	{ "getTickCount", V8_GetTickCount },
	{ "invokeNative", V8_InvokeNativeString },
	{ "invokeNativeByHash", V8_InvokeNativeHash },

	{ "snap", V8_Snap },
	{ "startProfiling", V8_StartProfiling },
	{ "stopProfiling", V8_StopProfiling },
	{ "setUnhandledPromiseRejectionFunction", V8_SetUnhandledPromiseRejectionRoutine },
	// boundary
	{ "submitBoundaryStart", V8_SubmitBoundaryStart },
	{ "submitBoundaryEnd", V8_SubmitBoundaryEnd },
	{ "setStackTraceFunction", V8_SetStackTraceRoutine },
	// metafields
	{ "pointerValueIntInitialized", V8_GetPointerField<MetaField::PointerValueInteger> },
	{ "pointerValueFloatInitialized", V8_GetPointerField<MetaField::PointerValueFloat> },
	{ "pointerValueInt", V8_GetMetaField<MetaField::PointerValueInteger> },
	{ "pointerValueFloat", V8_GetMetaField<MetaField::PointerValueFloat> },
	{ "pointerValueVector", V8_GetMetaField<MetaField::PointerValueVector> },
	{ "returnResultAnyway", V8_GetMetaField<MetaField::ReturnResultAnyway> },
	{ "resultAsInteger", V8_GetMetaField<MetaField::ResultAsInteger> },
	{ "resultAsLong", V8_GetMetaField<MetaField::ResultAsLong> },
	{ "resultAsFloat", V8_GetMetaField<MetaField::ResultAsFloat> },
	{ "resultAsString", V8_GetMetaField<MetaField::ResultAsString> },
	{ "resultAsVector", V8_GetMetaField<MetaField::ResultAsVector> },
	{ "resultAsObject2", V8_GetMetaField<MetaField::ResultAsObject> },
	{ "getResourcePath", V8_GetResourcePath },
};

static v8::Handle<v8::Value> Throw(v8::Isolate* isolate, const char* message) {
	return isolate->ThrowException(v8::String::NewFromUtf8(isolate, message).ToLocalChecked());
}

static bool ReadFileData(const v8::FunctionCallbackInfo<v8::Value>& args, std::vector<char>* fileData)
{
	V8ScriptRuntime* runtime = GetScriptRuntimeFromArgs(args);
	OMPtr<IScriptHost> scriptHost = runtime->GetScriptHost();

	V8PushEnvironment pushed(runtime);
	String::Utf8Value filename(GetV8Isolate(), args[0]);

	OMPtr<fxIStream> stream;
	result_t hr = scriptHost->OpenHostFile(*filename, stream.GetAddressOf());

	if (FX_FAILED(hr))
	{
		Throw(args.GetIsolate(), "Error loading file");
		return false;
	}

	uint64_t length;
	stream->GetLength(&length);

	uint32_t bytesRead;
	fileData->resize(length);
	stream->Read(fileData->data(), length, &bytesRead);

	return true;
}

static void V8_Read(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	std::vector<char> fileData;

	if (!ReadFileData(args, &fileData))
	{
		return;
	}

	Handle<String> str = String::NewFromUtf8(args.GetIsolate(), fileData.data(), NewStringType::kNormal, fileData.size()).ToLocalChecked();
	args.GetReturnValue().Set(str);
}

struct DataAndPersistent {
	std::vector<char> data;
	int byte_length;
	Global<ArrayBuffer> handle;
};

static void ReadBufferWeakCallback(
	const v8::WeakCallbackInfo<DataAndPersistent>& data) {
	int byte_length = data.GetParameter()->byte_length;
	data.GetIsolate()->AdjustAmountOfExternalAllocatedMemory(
		-static_cast<intptr_t>(byte_length));

	data.GetParameter()->handle.Reset();
	delete data.GetParameter();
}

static void V8_ReadBuffer(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	std::vector<char> fileData;

	if (!ReadFileData(args, &fileData))
	{
		return;
	}

	auto isolate = args.GetIsolate();

	Local<v8::ArrayBuffer> buffer = ArrayBuffer::New(isolate, fileData.size());
	auto abs = buffer->GetBackingStore();
	memcpy(abs->Data(), fileData.data(), fileData.size());

	args.GetReturnValue().Set(buffer);
}

static std::pair<std::string, FunctionCallback> g_globalFunctions[] =
{
	{ "read", V8_Read },
	{ "readbuffer", V8_ReadBuffer },
};

result_t V8ScriptRuntime::Create(IScriptHost* scriptHost)
{
	// assign the script host
	m_scriptHost = scriptHost;

	{
		fx::OMPtr<IScriptHost> ptr(scriptHost);
		fx::OMPtr<IScriptHostWithResourceData> resourcePtr;
		ptr.As(&resourcePtr);

		m_resourceHost = resourcePtr.GetRef();

		fx::OMPtr<IScriptHostWithManifest> manifestPtr;
		ptr.As(&manifestPtr);

		m_manifestHost = manifestPtr.GetRef();
	}

	// create a scope to hold handles we use here
	Locker locker(GetV8Isolate());
	Isolate::Scope isolateScope(GetV8Isolate());
	HandleScope handleScope(GetV8Isolate());

	// create global state
	Local<ObjectTemplate> global = ObjectTemplate::New(GetV8Isolate());

	// add a 'print' function as alias for Citizen.trace for testing
	global->Set(String::NewFromUtf8(GetV8Isolate(), "print", NewStringType::kNormal).ToLocalChecked(),
	FunctionTemplate::New(GetV8Isolate(), V8_Trace));

	// create Citizen call routines
	Local<ObjectTemplate> citizenObject = ObjectTemplate::New(GetV8Isolate());

	// loop through routine list
	for (auto& routine : g_citizenFunctions)
	{
		citizenObject->Set(GetV8Isolate(), routine.first.c_str(), FunctionTemplate::New(GetV8Isolate(), routine.second, External::New(GetV8Isolate(), this)));
	}

	// set the Citizen object
	global->Set(String::NewFromUtf8(GetV8Isolate(), "Citizen", NewStringType::kNormal).ToLocalChecked(),
	citizenObject);

	// create a V8 context and store it
	m_taskQueue = v8::MicrotaskQueue::New(GetV8Isolate(), MicrotasksPolicy::kExplicit);

	Local<Context> context = Context::New(GetV8Isolate(), nullptr, global, {}, {}, m_taskQueue.get());
	m_context.Reset(GetV8Isolate(), context);

	// store the ScRT in the context
	context->SetEmbedderData(16, External::New(GetV8Isolate(), this));

	// run the following entries in the context scope
	Context::Scope scope(context);

#ifdef V8_NODE
	if (UseNode())
	{
		auto fxdkMode = console::GetDefaultContext()->GetVariableManager()->FindEntryRaw("sv_fxdkMode");

		if (console::GetDefaultContext()->GetVariableManager()->FindEntryRaw("txAdminServerMode") ||
			(fxdkMode && fxdkMode->GetValue() == "1"))
		{
			putenv("NODE_CFX_IS_MONITOR_MODE=1");
		}

#ifdef _WIN32
#ifdef IS_FXSERVER
		std::string selfPath = ToNarrow(MakeRelativeCitPath(_P("FXServer.exe")));
#else
		std::string selfPath = ToNarrow(MakeCfxSubProcess(L"FXNode.exe", L"chrome"));
#endif
#else
		std::string selfPath = MakeRelativeCitPath(_P("FXServer"));
#endif

		std::string rootPath = selfPath;
		boost::algorithm::replace_all(rootPath, "/opt/cfx-server/FXServer", "");

		auto libPath = fmt::sprintf("%s/usr/lib/v8/:%s/lib/:%s/usr/lib/",
		rootPath,
		rootPath,
		rootPath);

		const std::vector<std::string> execArgv = {
#ifndef _WIN32
			"--library-path",
			libPath.c_str(),
			"--",
			selfPath.c_str(),
#endif
			"--start-node",
		};

		const std::vector<std::string> argv = {
			selfPath.c_str()
		};

		node::InitializeContext(context);

		auto env = node::CreateEnvironment(GetNodeIsolate(), context, argv, execArgv);
		node::LoadEnvironment(
			env,
			R"(
function defineStream(name, getter) {
  Object.defineProperty(process, name, {
    configurable: true,
    enumerable: true,
    get: getter
  });
}

defineStream('stdin', getStdin);

let stdin;

function getStdin() {
  if (stdin) return stdin;
  const fd = 0;

  const { Readable } = require('stream');
  stdin = new Readable({ read() {} });
  stdin.push(null);

  stdin.fd = 0;

  return stdin;
}

const { addBuiltinLibsToObject } = require('internal/modules/cjs/helpers');
addBuiltinLibsToObject(global);

const Module = require('module');

const m = new Module('dummy.js');
m.filename = Citizen.getResourcePath() + '/dummy.js';
m.paths = Module._nodeModulePaths(Citizen.getResourcePath() + '/');

const script = 'module.exports = {require};';
const result = m._compile(script, 'dummy-wrapper');

global.require = m.exports.require;
)"
		);

		node::SetProcessExitHandler(env, [](node::Environment*, int exitCode)
		{
#ifdef IS_FXSERVER
			auto monitorVar = fx::ResourceManager::GetCurrent()->GetComponent<fx::ServerInstanceBaseRef>()->Get()->GetComponent<console::Context>()->GetVariableManager()->FindEntryRaw("monitorMode");
			if (monitorVar)
			{
#ifdef _WIN32
				TerminateProcess(GetCurrentProcess(), exitCode);
#else
				raise(SIGTERM);
#endif
				return;
			}
#endif
			FatalError("Node.js exiting (exit code %d)\nSee console for details", exitCode);
		});

		g_envRuntimes[env] = this;

		m_nodeEnvironment = env;
	}
#endif

	// set global IO functions
	for (auto& routine : g_globalFunctions)
	{
		context->Global()->Set(
			context,
			String::NewFromUtf8(GetV8Isolate(), routine.first.c_str(), NewStringType::kNormal).ToLocalChecked(),
			Function::New(context, routine.second, External::New(GetV8Isolate(), this)).ToLocalChecked());
	}

	bool isGreater;

	// set the 'window' variable to the global itself
	context->Global()->Set(context, String::NewFromUtf8(GetV8Isolate(), "global", NewStringType::kNormal).ToLocalChecked(), context->Global());

	if (FX_SUCCEEDED(m_manifestHost->IsManifestVersionV2Between("bodacious", "", &isGreater)) && !isGreater)
	{
		// set the 'window' variable to the global itself
		context->Global()->Set(context, String::NewFromUtf8(GetV8Isolate(), "window", NewStringType::kNormal).ToLocalChecked(), context->Global());
	}

	std::string nativesBuild = "natives_universal.js";

	{
		for (const auto& versionPair : g_scriptVersionPairs)
		{
			bool isGreater;

			if (FX_SUCCEEDED(m_manifestHost->IsManifestVersionBetween(std::get<ManifestVersion>(versionPair).guid, guid_t{ 0 }, &isGreater)) && isGreater)
			{
				nativesBuild = std::get<const char*>(versionPair);
			}
		}
	}

	// run system scripts
	result_t hr;

	// Loading natives
	if (FX_FAILED(hr = LoadSystemFile(const_cast<char*>(va("citizen:/scripting/v8/%s", nativesBuild)))))
	{
		return hr;
	}

	for (const char* platformScript : g_platformScripts)
	{
		if (FX_FAILED(hr = LoadSystemFile(const_cast<char*>(platformScript))))
		{
			return hr;
		}
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::Destroy()
{
	m_eventRoutine = TEventRoutine();
	m_tickRoutine = std::function<void()>();
	m_callRefRoutine = TCallRefRoutine();
	m_deleteRefRoutine = TDeleteRefRoutine();
	m_duplicateRefRoutine = TDuplicateRefRoutine();

#ifdef V8_NODE
	V8PushEnvironment pushed(this);

	if (UseNode())
	{
		g_envRuntimes.erase(m_nodeEnvironment);

		node::FreeEnvironment(m_nodeEnvironment);
	}
#else
	fx::PushEnvironment pushed(this);
#endif

	m_context.Reset();

	return FX_S_OK;
}

void* V8ScriptRuntime::GetParentObject()
{
	return m_parentObject;
}

void V8ScriptRuntime::SetParentObject(void* parentObject)
{
	m_parentObject = parentObject;
}

result_t V8ScriptRuntime::LoadFileInternal(OMPtr<fxIStream> stream, char* scriptFile, Local<Script>* outScript)
{
	// read file data
	uint64_t length;
	result_t hr;

	if (FX_FAILED(hr = stream->GetLength(&length)))
	{
		return hr;
	}

	std::vector<char> fileData(length + 1);
	if (FX_FAILED(hr = stream->Read(&fileData[0], length, nullptr)))
	{
		return hr;
	}

	fileData[length] = '\0';

	// create the script data
	Local<String> scriptText = String::NewFromUtf8(GetV8Isolate(), &fileData[0], NewStringType::kNormal).ToLocalChecked();
	Local<String> fileName = String::NewFromUtf8(GetV8Isolate(), scriptFile, NewStringType::kNormal).ToLocalChecked();

	// compile the script in a TryCatch
	{
		TryCatch eh(GetV8Isolate());
		v8::ScriptOrigin origin(fileName);
		MaybeLocal<Script> script = Script::Compile(GetContext(), scriptText, &origin);

		if (script.IsEmpty())
		{
			String::Utf8Value str(GetV8Isolate(), eh.Exception());

			ScriptTrace("Error parsing script %s in resource %s: %s\n", scriptFile, GetResourceName(), *str);

			// TODO: change?
			return FX_E_INVALIDARG;
		}

		*outScript = script.ToLocalChecked();
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::LoadHostFileInternal(char* scriptFile, Local<Script>* outScript)
{
	// open the file
	OMPtr<fxIStream> stream;

	result_t hr = m_scriptHost->OpenHostFile(scriptFile, stream.GetAddressOf());

	if (FX_FAILED(hr))
	{
		// TODO: log this?
		return hr;
	}

	char* resourceName;
	m_resourceHost->GetResourceName(&resourceName);

	return LoadFileInternal(stream, (scriptFile[0] != '@') ? const_cast<char*>(fmt::sprintf("@%s/%s", resourceName, scriptFile).c_str()) : scriptFile, outScript);
}

result_t V8ScriptRuntime::LoadSystemFileInternal(char* scriptFile, Local<Script>* outScript)
{
	// open the file
	OMPtr<fxIStream> stream;

	result_t hr = m_scriptHost->OpenSystemFile(scriptFile, stream.GetAddressOf());

	if (FX_FAILED(hr))
	{
		// TODO: log this?
		return hr;
	}

	return LoadFileInternal(stream, scriptFile, outScript);
}

result_t V8ScriptRuntime::RunFileInternal(char* scriptName, std::function<result_t(char*, Local<Script>*)> loadFunction)
{
	V8PushEnvironment pushed(this);

	result_t hr;
	Local<Script> script;

	if (FX_FAILED(hr = loadFunction(scriptName, &script)))
	{
		return hr;
	}

	{
		TryCatch eh(GetV8Isolate());
		MaybeLocal<Value> value = script->Run(GetContext());

		if (value.IsEmpty())
		{
			String::Utf8Value str(GetV8Isolate(), eh.Exception());
			String::Utf8Value stack(GetV8Isolate(), GetStackTrace(eh, this));

			ScriptTrace("Error loading script %s in resource %s: %s\nstack:\n%s\n", scriptName, GetResourceName(), *str, *stack);

			// TODO: change?
			return FX_E_INVALIDARG;
		}
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::LoadFile(char* scriptName)
{
	return RunFileInternal(scriptName, std::bind(&V8ScriptRuntime::LoadHostFileInternal, this, std::placeholders::_1, std::placeholders::_2));
}

result_t V8ScriptRuntime::LoadSystemFile(char* scriptName)
{
	return RunFileInternal(scriptName, std::bind(&V8ScriptRuntime::LoadSystemFileInternal, this, std::placeholders::_1, std::placeholders::_2));
}

int V8ScriptRuntime::GetInstanceId()
{
	return m_instanceId;
}

int32_t V8ScriptRuntime::HandlesFile(char* fileName, IScriptHostWithResourceData* metadata)
{
#ifdef V8_12_2
	constexpr bool isInLegacyRuntime = false;
#else
	constexpr bool isInLegacyRuntime = true;
#endif
	if (!UseThis())
	{
		return false;
	}

	const auto isJS = strstr(fileName, ".js");
	if (!isJS)
	{
		return false;
	}

	char* versionStr = "16";
	metadata->GetResourceMetaData("node_version", 0, &versionStr);

	const bool useLegacyRuntime = !strcmp("16", versionStr);

	if (useLegacyRuntime == isInLegacyRuntime)
	{
		return true;
	}
	return false;
}

struct FakeScope
{
	template<typename... TArgs>
	FakeScope(TArgs... args)
	{
	}
};

result_t V8ScriptRuntime::Tick()
{
	if (m_tickRoutine)
	{
#ifdef IS_FXSERVER
		if (!v8::Locker::IsLocked(GetV8Isolate()))
		{
			V8PushEnvironment pushed(this);
			m_tickRoutine();
		}
		else
#endif
		{
			V8PushEnvironment<FakeScope, FakeScope> pushed(this);
			m_tickRoutine();
		}
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::TriggerEvent(char* eventName, char* eventPayload, uint32_t payloadSize, char* eventSource)
{
	if (m_eventRoutine)
	{
		V8PushEnvironment pushed(this);

		m_eventRoutine(eventName, eventPayload, payloadSize, eventSource);
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsLength, IScriptBuffer** retval)
{
	*retval = nullptr;

	if (m_callRefRoutine)
	{
		V8PushEnvironment pushed(this);

		auto rv = m_callRefRoutine(refIdx, argsSerialized, argsLength);
		return rv.CopyTo(retval);
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::DuplicateRef(int32_t refIdx, int32_t* outRefIdx)
{
	*outRefIdx = -1;

	if (m_duplicateRefRoutine)
	{
		V8PushEnvironment pushed(this);

		*outRefIdx = m_duplicateRefRoutine(refIdx);
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::RemoveRef(int32_t refIdx)
{
	if (m_deleteRefRoutine)
	{
		V8PushEnvironment pushed(this);

		m_deleteRefRoutine(refIdx);
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::WalkStack(char* boundaryStart, uint32_t boundaryStartLength, char* boundaryEnd, uint32_t boundaryEndLength, IScriptStackWalkVisitor* visitor)
{
	if (m_stackTraceRoutine)
	{
		V8PushEnvironment pushed(this);

		char* out = nullptr;
		size_t outLen = 0;

		m_stackTraceRoutine(boundaryStart, boundaryEnd, &out, &outLen);

		if (out)
		{
			msgpack::unpacked up = msgpack::unpack(out, outLen);

			auto o = up.get().as<std::vector<msgpack::object>>();

			for (auto& e : o)
			{
				msgpack::sbuffer sb;
				msgpack::pack(sb, e);

				visitor->SubmitStackFrame(sb.data(), sb.size());
			}
		}
	}

	return FX_S_OK;
}

result_t V8ScriptRuntime::EmitWarning(char* channel, char* message)
{
	if (!m_context.IsEmpty())
	{
		auto context = m_context.Get(GetV8Isolate());
		auto consoleMaybe = context->Global()->Get(context, String::NewFromUtf8(GetV8Isolate(), "console").ToLocalChecked());
		v8::Local<v8::Value> console;

		if (consoleMaybe.ToLocal(&console))
		{
			auto consoleObject = console.As<v8::Object>();
			auto warnMaybe = consoleObject->Get(context, String::NewFromUtf8(GetV8Isolate(), "warn").ToLocalChecked());

			v8::Local<v8::Value> warn;

			if (warnMaybe.ToLocal(&warn))
			{
				auto messageStr = fmt::sprintf("[%s] %s", channel, message);

				// console.warn() will append a newline itself, so we remove any redundant newline
				auto length = messageStr.length();
				if (messageStr[length - 1] == '\n')
				{
					--length;
				}

				v8::Local<v8::Value> args[] = {
					String::NewFromUtf8(GetV8Isolate(), messageStr.c_str(), v8::NewStringType::kNormal, length).ToLocalChecked()
				};

				auto warnFunction = warn.As<v8::Function>();
				warnFunction->Call(context, Null(GetV8Isolate()), std::size(args), args);
			}
		}
	}

	return FX_S_OK;
}

class V8ScriptGlobals
{
private:
	Isolate* m_isolate;

#ifdef V8_NODE
	node::IsolateData* m_nodeData;
#endif

	std::vector<char> m_nativesBlob;

	std::vector<char> m_snapshotBlob;

	std::unique_ptr<Platform> m_platform;

	std::unique_ptr<v8::ArrayBuffer::Allocator> m_arrayBufferAllocator;

	std::unique_ptr<V8Debugger> m_debugger;

	bool m_inited;

public:
	V8ScriptGlobals();

	~V8ScriptGlobals();

	void Initialize();

	inline Platform* GetPlatform()
	{
		return m_platform.get();
	}

	inline Isolate* GetIsolate()
	{
		return m_isolate;
	}

#ifdef V8_NODE
	inline node::IsolateData* GetNodeIsolate()
	{
		return m_nodeData;
	}
#endif
};

static V8ScriptGlobals g_v8;

Isolate* GetV8Isolate()
{
	return g_v8.GetIsolate();
}

static Platform* GetV8Platform()
{
	return g_v8.GetPlatform();
}

#ifdef V8_NODE
static node::IsolateData* GetNodeIsolate()
{
	return g_v8.GetNodeIsolate();
}
#endif

static void OnMessage(Local<Message> message, Local<Value> error)
{
	v8::String::Utf8Value messageStr(GetV8Isolate(), message->Get());
	v8::String::Utf8Value errorStr(GetV8Isolate(), error);

	std::stringstream stack;
	auto stackTrace = message->GetStackTrace();

	for (int i = 0; i < stackTrace->GetFrameCount(); i++)
	{
		auto frame = stackTrace->GetFrame(GetV8Isolate(), i);

		v8::String::Utf8Value sourceStr(GetV8Isolate(), frame->GetScriptNameOrSourceURL());
		v8::String::Utf8Value functionStr(GetV8Isolate(), frame->GetFunctionName());
		
		stack << (*sourceStr ? *sourceStr : "(unknown)") << "(" << frame->GetLineNumber() << "," << frame->GetColumn() << "): " << (*functionStr ? *functionStr : "") << "\n";
	}

	ScriptTrace("%s\n%s\n%s\n", *messageStr, stack.str(), *errorStr);
}

V8ScriptGlobals::V8ScriptGlobals()
{
}

#ifdef V8_NODE
static thread_local std::stack<std::unique_ptr<BasePushEnvironment>> g_envStack;
#endif

void V8ScriptGlobals::Initialize()
{
#ifdef V8_NODE
	for (int i = 0; i < g_argc; ++i)
	{
		// Don't initialize anything if started in --start-node mode with Node20
		if (strcmp(g_argv[i], "--fork-node22") == 0)
		{
			return;
		}
	}
#endif

	if (m_inited)
	{
		return;
	}

	m_inited = true;

	// don't initialize the same V8 twice
	if (!UseThis())
	{
		return;
	}

// We don't need snapshots for new V8 because it's embedded
#ifndef V8_12_2
#ifdef _WIN32
	// initialize startup data
	auto readBlob = [=](const std::wstring& name, std::vector<char>& outBlob)
	{
		FILE* f = _wfopen(MakeRelativeCitPath(fmt::sprintf(L"citizen/scripting/v8/%d.%d/%s", V8_MAJOR_VERSION, V8_MINOR_VERSION, name)).c_str(), L"rb");

#ifndef IS_FXSERVER
		if (!f)
		{
			static HostSharedData<CfxState> hostData("CfxInitState");
			auto cli = va(L"\"%s\" -switchcl",
				hostData->gameExePath);

			STARTUPINFOW si = { 0 };
			si.cb = sizeof(si);

			PROCESS_INFORMATION pi;

			if (!CreateProcessW(NULL, const_cast<wchar_t*>(cli), NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi))
			{
				trace("failed to exit: %d\n", GetLastError());
			}

			_wunlink(MakeRelativeCitPath(L"content_index.xml").c_str());
			TerminateProcess(GetCurrentProcess(), 0);
		}
#else
		assert(f);
#endif

		fseek(f, 0, SEEK_END);
		outBlob.resize(ftell(f));

		fseek(f, 0, SEEK_SET);
		fread(outBlob.data(), 1, outBlob.size(), f);

		fclose(f);
	};


	readBlob(L"snapshot_blob.bin", m_snapshotBlob);

	static StartupData snapshotBlob;
	snapshotBlob.data = &m_snapshotBlob[0];
	snapshotBlob.raw_size = m_snapshotBlob.size();

	V8::SetSnapshotDataBlob(&snapshotBlob);
#endif

#endif

	std::vector<std::string> exec_argv;
	std::vector<std::string> errors;

#ifdef V8_NODE
	if (UseNode())
	{
		bool isStartNode = (g_argc >= 2 && strcmp(g_argv[1], "--start-node") == 0);
		if(isStartNode && g_argc > 2 && strcmp(g_argv[2], "--fork-node22") == 0)
		{
			isStartNode = false;
		}

		bool isFxNode = (g_argc >= 1 && strstr(g_argv[0], "FXNode.exe") != nullptr);

		if (isStartNode || isFxNode)
		{
			int ec = 0;

			// run in a thread so that pthread attributes take effect on musl-based Linux
			// (GNU stack size presets do not seem to work here)
			std::thread([&ec, isStartNode]
			{
			// TODO: code duplication with above
#ifdef _WIN32
#ifdef IS_FXSERVER
				std::string selfPath = ToNarrow(MakeRelativeCitPath(_P("FXServer.exe")));
#else
				std::string selfPath = ToNarrow(MakeCfxSubProcess(L"FXNode.exe", L"chrome"));
#endif
#else
				std::string selfPath = MakeRelativeCitPath(_P("FXServer"));
#endif

				std::string rootPath = selfPath;
				boost::algorithm::replace_all(rootPath, "/opt/cfx-server/FXServer", "");

				auto libPath = fmt::sprintf("%s/usr/lib/v8/:%s/lib/:%s/usr/lib/",
				rootPath,
				rootPath,
				rootPath);

				auto icuDataPath = MakeRelativeCitPath(fmt::sprintf(_P("citizen/scripting/v8/%d.%d/icudtl.dat"), V8_MAJOR_VERSION, V8_MINOR_VERSION));
				auto icuEnv = fmt::format(_P("CFX_ICU_PATH={}"), icuDataPath);

#ifdef _WIN32
				g_argv[0] = const_cast<char*>(selfPath.c_str());

				_wputenv(icuEnv.c_str());
#else
				putenv(const_cast<char*>(icuEnv.c_str()));
#endif

				const char* execArgv[] = {
#ifndef _WIN32
					"--library-path",
					libPath.c_str(),
					"--",
					selfPath.c_str(),
#endif
					"--start-node"
				};

				int nextArgc = g_argc;
				
				std::vector<char*> nextArgv(g_argc);
				memcpy(nextArgv.data(), g_argv, g_argc * sizeof(*g_argv));

				if (!isStartNode)
				{
					nextArgc++;
					nextArgv.resize(nextArgc);

					// make space for --start-node arg
					memmove(nextArgv.data() + 2, nextArgv.data() + 1, (g_argc - 1) * sizeof(*g_argv));
					nextArgv[1] = const_cast<char*>("--start-node");
				}

				ec = node::Start(nextArgc, nextArgv.data(), std::size(execArgv), const_cast<char**>(execArgv));
			})
			.join();

#ifdef _WIN32
			// newer Node won't play nice
			TerminateProcess(GetCurrentProcess(), ec);
#else
			exit(ec);
#endif
		}
	}
#endif

	// initialize platform
	if (!UseNode())
	{
		m_platform = std::unique_ptr<v8::Platform>(v8::platform::NewDefaultPlatform());
	}
#ifdef V8_NODE
	else
	{
		m_platform = node::MultiIsolatePlatform::Create(4);
	}
#endif

	V8::InitializePlatform(m_platform.get());

#if 0
	// set profiling disabled, but timer event logging enabled
	int argc = 4;
	const char* argv[] =
	{
		"",
		"--noprof-auto",
		"--prof",
		"--log-timer-events"
	};

	V8::SetFlagsFromCommandLine(&argc, (char**)argv, false);
#endif
	const char* flags = "--turbo-inline-js-wasm-calls --expose_gc --harmony-top-level-await";
	V8::SetFlagsFromString(flags, strlen(flags));

//icudtl.dat also embedded in our V8 12.2
#ifndef V8_12_2
	auto icuDataPath = MakeRelativeCitPath(fmt::sprintf(_P("citizen/scripting/v8/%d.%d/icudtl.dat"), V8_MAJOR_VERSION, V8_MINOR_VERSION));

#ifdef _WIN32
	V8::InitializeICUDefaultLocation(ToNarrow(MakeRelativeCitPath(L"dummy")).c_str(), ToNarrow(icuDataPath).c_str());
#else
	V8::InitializeICU(icuDataPath.c_str());
#endif
#endif

	// initialize global V8
	V8::Initialize();

	// create an array buffer allocator
	if (!UseNode())
	{
		m_arrayBufferAllocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
	}
#ifdef V8_NODE
	else
	{
		m_arrayBufferAllocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(node::ArrayBufferAllocator::Create());
	}
#endif

	// create an isolate
	Isolate::CreateParams params;
	params.array_buffer_allocator = m_arrayBufferAllocator.get();

	const auto kScaledMemory = GetScaledPhysicalMemorySize();

	auto constraints = ResourceConstraints{};

	constraints.ConfigureDefaultsFromHeapSize(0, kScaledMemory);

	params.constraints = constraints;

	m_isolate = Isolate::Allocate();

	m_isolate->AddGCPrologueCallback([](Isolate* isolate, GCType type,
									 GCCallbackFlags flags)
	{
		g_isV8InGc++;
	});

	m_isolate->AddGCEpilogueCallback([](Isolate* isolate, GCType type,
									 GCCallbackFlags flags)
	{
		g_isV8InGc--;
	});

#ifdef V8_NODE
	if (UseNode())
	{
		static_cast<node::MultiIsolatePlatform*>(m_platform.get())->RegisterIsolate(m_isolate, Instance<net::UvLoopManager>::Get()->GetOrCreate(std::string("svMain"))->GetLoop());
	}
#endif

#ifndef V8_NODE
	m_isolate->SetPromiseRejectCallback([](PromiseRejectMessage message)
	{
		Local<Promise> promise = message.GetPromise();
#ifndef V8_12_2
		Local<Context> context = promise->CreationContext();
#else
		Local<Context> context = promise->GetCreationContext().ToLocalChecked();
#endif

		auto embedderData = context->GetEmbedderData(16);

		if (embedderData.IsEmpty())
		{
			return;
		}

		auto external = Local<External>::Cast(embedderData);
		auto scRT = reinterpret_cast<V8ScriptRuntime*>(external->Value());

		scRT->HandlePromiseRejection(message);

	});
#else
	m_isolate->SetPromiseRejectCallback(node::PromiseRejectCallback);
#endif

	Isolate::Initialize(m_isolate, params);

	m_isolate->SetFatalErrorHandler([] (const char* location, const char* message)
	{
		FatalError("V8 error at %s: %s", location, message);
	});

	m_isolate->SetCaptureStackTraceForUncaughtExceptions(true);

	m_isolate->AddMessageListener(OnMessage);

	// initialize the debugger
	m_debugger = std::unique_ptr<V8Debugger>(CreateDebugger(m_isolate));

#ifdef V8_NODE
	if (UseNode())
	{
		// initialize Node.js
		Locker locker(m_isolate);
		Isolate::Scope isolateScope(m_isolate);
		v8::HandleScope handle_scope(m_isolate);
		node::MultiIsolatePlatform* nodePlatform = static_cast<node::MultiIsolatePlatform*>(m_platform.get());

		node::SetScopeHandler([](const node::Environment* env)
		{
			auto runtime = GetEnvRuntime(env);

			if (runtime)
			{
				// if already have current runtime on top - push noop env so we won't run microtasks on push env dtor
				// other approach would be to add refcount to topmost push env
				if (g_currentV8Runtime.GetRef() == runtime)
				{
					g_envStack.push(std::make_unique<V8NoopPushEnvironment>());
					return;
				}

				// since the V8 locker might be held by our caller, lock order for V8LitePushEnvironment might not be correct
				// instead, we will just dare to not push the runtime if we don't need to (and we're already locked)

				// this does mean any callbacks node.js will run on closing are more limited (can't call framework natives)
				if (v8::Locker::IsLocked(node::GetIsolate(env)))
				{
					PushEnvironment pe;

					if (PushEnvironment::TryPush(OMPtr{ runtime }, pe))
					{
						g_envStack.push(std::make_unique<V8LitePushEnvironment>(std::move(pe), runtime, env));
					}
					else
					{
						g_envStack.push(std::make_unique<V8LiteNoRuntimePushEnvironment>(env));
					}

					return;
				}

				g_envStack.push(std::make_unique<V8LitePushEnvironment>(runtime, env));
			}
			else
			{
				g_envStack.push(std::make_unique<V8LiteNoRuntimePushEnvironment>(env));
			}
		},
		[](const node::Environment* env)
		{
			g_envStack.pop();
		});

#ifndef IS_FXSERVER
		std::string selfPath = ToNarrow(MakeCfxSubProcess(L"FXNode.exe", L"chrome"));
#endif

		std::vector<std::string> args{
#ifndef IS_FXSERVER
			selfPath.c_str(),
#else
			"",
#endif
			"--expose-internals",
			"--unhandled-rejections=warn",
		};

		for (int i = 1; i < g_argc; i++)
		{
			// `-b` should be ignored for `-bXXXX` cross-build runtime
			if (g_argv[i] && g_argv[i][0] == '-' && strcmp(g_argv[i], "-fxdk") != 0 && strncmp(g_argv[i], "-b", 2) != 0)
			{
				args.push_back(g_argv[i]);
			}
		}

		node::InitializeNodeWithArgs(&args, &exec_argv, &errors);

		m_nodeData = node::CreateIsolateData(m_isolate, Instance<net::UvLoopManager>::Get()->GetOrCreate(std::string("svMain"))->GetLoop(), nodePlatform, (node::ArrayBufferAllocator*)m_arrayBufferAllocator.get());
	}
#endif
}

#ifdef _WIN32
}
#include <MinHook.h>
#include <uv.h>

static int uv_exepath_custom(char*, int)
{
	return -1;
}


#ifndef V8_12_2
static decltype(&fopen) g_origFopen;

static FILE* fopen_wrap(const char* name, const char* mode)
{
	static auto __wfopen = ((decltype(&_wfopen))GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "_wfopen"));

	if (name && strstr(name, "icudt"))
	{
		return __wfopen(ToWide(name).c_str(), ToWide(mode).c_str());
	}

	return g_origFopen(name, mode);
}
#endif

static decltype(&uv_spawn) uv_spawn_orig;

static int uv_spawn_custom(uv_loop_t* loop, uv_process_t* handle, const uv_process_options_t* options)
{
	uv_process_options_t options2 = *options;
	options2.flags |= UV_PROCESS_WINDOWS_HIDE_CONSOLE;

	return uv_spawn_orig(loop, handle, &options2);
}

void Component_RunPreInit()
{
	// since otherwise we'll invoke the game again and again and again
	auto ep = GetProcAddress(GetModuleHandleW(L"libuv.dll"), "uv_exepath");
	auto sp = GetProcAddress(GetModuleHandleW(L"libuv.dll"), "uv_spawn");

	MH_Initialize();

	MH_CreateHook(ep, uv_exepath_custom, NULL);
	MH_EnableHook(ep);
		
	MH_CreateHook(sp, uv_spawn_custom, (void**)&uv_spawn_orig);
	MH_EnableHook(sp);

#ifndef V8_12_2
	// fopen utf-8 bugfix (for icudt?.dat)
	auto fopen_ep = GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "fopen");
	MH_CreateHook(fopen_ep, fopen_wrap, (void**)&g_origFopen);
	MH_EnableHook(fopen_ep);
#endif

	fx::g_v8.Initialize();
}

namespace fx
{
#endif

static InitFunction initFunction([]()
{
	if (!UseThis())
	{
		return;
	}

	g_v8.Initialize();

	// trigger removing funcrefs on the *resource manager* so that it'll still happen when a runtime is destroyed
	ResourceManager::OnInitializeInstance.Connect([](ResourceManager* manager)
	{
		static uint8_t buffer[sizeof(v8::Locker)];
		static v8::Locker* locker = nullptr;
		static uint8_t isolateBuffer[sizeof(v8::Isolate::Scope)];
		static v8::Isolate::Scope* isolateScope = nullptr;

		manager->OnTick.Connect([]()
		{
			locker = new (buffer) Locker(GetV8Isolate());
			isolateScope = new (isolateBuffer) Isolate::Scope(GetV8Isolate());

			if (!UseNode())
			{
				v8::platform::PumpMessageLoop(GetV8Platform(), GetV8Isolate());
			}
		},
		INT32_MIN);

		manager->OnTick.Connect([]()
		{
			isolateScope->~Scope();
			locker->~Locker();
		},
		INT32_MAX);

		manager->OnTick.Connect([]()
		{
			RefAndPersistent* deleteRef;

			while (g_cleanUpFuncRefs.try_pop(deleteRef))
			{
				delete deleteRef;
			}
		});
	});
});

V8ScriptGlobals::~V8ScriptGlobals()
{
	// clean up V8
	/*m_isolate->Dispose();

	V8::Dispose();
	V8::ShutdownPlatform();*/

	// actually don't, this deadlocks from a global destructor
	if (UseNode())
	{
		m_platform.release();
	}
}

#ifndef V8_NODE
// {9C26844A-7AF4-4A3B-995A-3B1692E958AC}
#ifdef V8_12_2
FX_DEFINE_GUID(CLSID_V8ScriptRuntime, 0x9c26844A, 0x7af4, 0x4a3b, 0x99, 0x5a, 0x3b, 0x16, 0x92, 0xe9, 0x58, 0xad);
#else
FX_DEFINE_GUID(CLSID_V8ScriptRuntime, 0x9c26844A, 0x7af4, 0x4a3b, 0x99, 0x5a, 0x3b, 0x16, 0x92, 0xe9, 0x58, 0xac);
#endif
#else
// {9C26844B-7AF4-4A3B-995A-3B1692E958AC}
FX_DEFINE_GUID(CLSID_V8ScriptRuntime, 0x9c26844B, 0x7af4, 0x4a3b, 0x99, 0x5a, 0x3b, 0x16, 0x92, 0xe9, 0x58, 0xac);
#endif


FX_NEW_FACTORY(V8ScriptRuntime);

FX_IMPLEMENTS(CLSID_V8ScriptRuntime, IScriptRuntime);
FX_IMPLEMENTS(CLSID_V8ScriptRuntime, IScriptFileHandlingRuntime);
}

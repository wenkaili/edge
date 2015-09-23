/**
 * Portions Copyright (c) Microsoft Corporation. All rights reserved. 
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *  http://www.apache.org/licenses/LICENSE-2.0  
 *
 * THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS
 * OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION 
 * ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR 
 * PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT. 
 *
 * See the Apache Version 2.0 License for specific language governing 
 * permissions and limitations under the License.
 */
#include "edge.h"

ClrFuncInvokeContext::ClrFuncInvokeContext(Handle<v8::Value> callbackOrSync)
{
    DBG("ClrFuncInvokeContext::ClrFuncInvokeContext");
    if (callbackOrSync->IsFunction())
    {
        this->callback = new Persistent<Function>;
		Local<Function> callbackOrSyncFunction = Handle<Function>::Cast(callbackOrSync);
		(this->callback)->Reset(Isolate::GetCurrent(), callbackOrSyncFunction);
        this->Sync = false;
    }
    else 
    {
        this->Sync = callbackOrSync->BooleanValue();
    }

    this->uv_edge_async = NULL;
}

void ClrFuncInvokeContext::DisposeCallback()
{
    if (this->callback)
    {
        DBG("ClrFuncInvokeContext::DisposeCallback");
		this->callback->Reset();
        delete this->callback;
        this->callback = NULL;        
    }
}

void ClrFuncInvokeContext::CompleteOnCLRThread(System::Threading::Tasks::Task<System::Object^>^ task)
{
    DBG("ClrFuncInvokeContext::CompleteOnCLRThread");
    this->Task = task;
    V8SynchronizationContext::ExecuteAction(this->uv_edge_async);
}

void ClrFuncInvokeContext::InitializeAsyncOperation()
{
    // Create a uv_edge_async instance representing V8 async operation that will complete 
    // when the CLR function completes. The ClrActionContext is used to ensure the ClrFuncInvokeContext
    // remains GC-rooted while the CLR function executes.

    ClrActionContext* data = new ClrActionContext;
    data->action = gcnew System::Action(this, &ClrFuncInvokeContext::CompleteOnV8ThreadAsynchronous);
    this->uv_edge_async = V8SynchronizationContext::RegisterAction(ClrActionContext::ActionCallback, data);
}

void ClrFuncInvokeContext::CompleteOnV8ThreadAsynchronous()
{
    Nan::HandleScope scope;
    this->CompleteOnV8Thread();
}

Handle<v8::Value> ClrFuncInvokeContext::CompleteOnV8Thread()
{
    DBG("ClrFuncInvokeContext::CompleteOnV8Thread");

    Nan::EscapableHandleScope scope;

    // The uv_edge_async was already cleaned up in V8SynchronizationContext::ExecuteAction
    this->uv_edge_async = NULL;

    if (!this->Sync && !this->callback)
    {
        // this was an async call without callback specified
        DBG("ClrFuncInvokeContext::CompleteOnV8Thread - async without callback");
        return scope.Escape(Nan::Undefined());
    }

    Handle<Value> argv[] = { Nan::Undefined(), Nan::Undefined() };
    int argc = 1;

    switch (this->Task->Status) {
        default:
            argv[0] = Nan::New<v8::String>("The operation reported completion in an unexpected state.").ToLocalChecked();
        break;
        case TaskStatus::Faulted:
            if (this->Task->Exception != nullptr) {
                argv[0] = ClrFunc::MarshalCLRExceptionToV8(this->Task->Exception);
            }
            else {
                argv[0] = Nan::New<v8::String>("The operation has failed with an undetermined error.").ToLocalChecked();
            }
        break;
        case TaskStatus::Canceled:
            argv[0] = Nan::New<v8::String>("The operation was cancelled.").ToLocalChecked();
        break;
        case TaskStatus::RanToCompletion:
            argc = 2;
            try {
                argv[1] = ClrFunc::MarshalCLRToV8(this->Task->Result);
            }
            catch (System::Exception^ e) {
                argc = 1;
                argv[0] = ClrFunc::MarshalCLRExceptionToV8(e);
            }
        break;
    };

    if (!this->Sync)
    {
        // complete the asynchronous call to C# by invoking a callback in JavaScript
        TryCatch try_catch;
        Nan::New<v8::Function>(*(this->callback))->Call(Nan::GetCurrentContext()->Global(), argc, argv);
        this->DisposeCallback();
        if (try_catch.HasCaught()) 
        {
			node::FatalException(Isolate::GetCurrent(), try_catch);
        }        

        DBG("ClrFuncInvokeContext::CompleteOnV8Thread - async with callback");
        return scope.Escape(Nan::Undefined());
    }
    else if (1 == argc) 
    {
        DBG("ClrFuncInvokeContext::CompleteOnV8Thread - handleScope.Close(ThrowException(argv[0]))");
        // complete the synchronous call to C# by re-throwing the resulting exception
        Nan::ThrowError((v8::Local<v8::Value>)argv[0]);
        return scope.Escape<v8::Value>(argv[0]);
    }
    else
    {
        DBG("ClrFuncInvokeContext::CompleteOnV8Thread - handleScope.Close(argv[1])");
        // complete the synchronous call to C# by returning the result
        return scope.Escape<v8::Value>(argv[1]);
    }
}

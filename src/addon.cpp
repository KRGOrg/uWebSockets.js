/*
 * Authored by Alex Hultman, 2018-2026.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* We are only allowed to depend on µWS and V8 in this layer. */
#include "App.h"
#include "Http3App.h"

#include <iostream>
#include <vector>
#include <type_traits>

#include <v8.h>
using namespace v8;

#include "Utilities.h"
#include "WebSocketWrapper.h"
#include "HttpResponseWrapper.h"
#include "HttpRequestWrapper.h"
#include "AppWrapper.h"

#include <numeric>
#include <functional>

/* Todo: Apps should be freed once the GC says so BUT ALWAYS before freeing the loop */

#include "Multipart.h"

/* This function is somewhat of a simplifying wrapper that does not follow the C++ library.
 * It takes a POST:ed body and contentType, and returns an array of parts if
 * the request is a multipart request */
void uWS_getParts(const FunctionCallbackInfo<Value> &args) {

    /* Because we mutate the strings, it is important that we get mutable input like
     * ArrayBuffer or Buffer, not String! */
    Isolate *isolate = args.GetIsolate();

    NativeString body(args.GetIsolate(), args[0]);
    if (body.isInvalid(args)) {
        return;
    }

    NativeString contentType(args.GetIsolate(), args[1]);
    if (contentType.isInvalid(args)) {
        return;
    }

    uWS::MultipartParser mp(contentType.getString());
    if (mp.isValid()) {
        mp.setBody(body.getString());

        std::pair<std::string_view, std::string_view> headers[10];

        Local<Array> parts = Array::New(args.GetIsolate(), 0);

        while (true) {
            std::optional<std::string_view> optionalPart = mp.getNextPart(headers);
            if (!optionalPart.has_value()) {
                break;
            }

            std::string_view part = optionalPart.value();

            Local<ArrayBuffer> partArrayBuffer = ArrayBuffer_NewCopy(isolate, (void *) part.data(), part.length());
            /* Map is 30% faster in this case, but a static Object could be faster still */
            Local<Object> partMap = Object::New(isolate);
            partMap->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "data", NewStringType::kNormal).ToLocalChecked(), partArrayBuffer).IsNothing();

            for (int i = 0; headers[i].first.length(); i++) {
                /* We care about content-type and content-disposition */
                if (headers[i].first == "content-type") {
                    partMap->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "type", NewStringType::kNormal).ToLocalChecked(), String::NewFromUtf8(isolate, headers[i].second.data(), NewStringType::kNormal, headers[i].second.length()).ToLocalChecked()).IsNothing();
                } else if (headers[i].first == "content-disposition") {
                    /* Parse the parameters */
                    uWS::ParameterParser pp(headers[i].second);
                    while (true) {
                        auto [key, value] = pp.getKeyValue();
                        if (!key.length()) {
                            break;
                        }

                        // really anything that has both key and value and is not type or data?
                        if (key == "name" || key == "filename") {
                            partMap->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, key.data(), NewStringType::kNormal, key.length()).ToLocalChecked(), String::NewFromUtf8(isolate, value.data(), NewStringType::kNormal, value.length()).ToLocalChecked()).IsNothing();
                        }
                    }
                }
            }

            parts->Set(isolate->GetCurrentContext(), parts->Length(), partMap).IsNothing();
        }

        args.GetReturnValue().Set(parts);
    }

    /* We'll return undefined on error */
}

/* Faster setTimeout, clearTimeout */

//#include "FastTimers.h"

//UniquePersistent<Function> timerCallbacksJS[1000];

/* These three are registered exports but have no implementation in this build
 * (src/FastTimers.h is not compiled), so they used to hand back undefined and never fire
 * the callback - a silent no-op, which is the worst of the options. They now fail loudly;
 * removing the exports would be a breaking change, and a real implementation would need a
 * timer subsystem on the loop that is out of scope here. */
void uWS_arm(const FunctionCallbackInfo<Value> &args) {

    /* integer */

    Isolate *isolate = args.GetIsolate();
    args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "uWS.arm is not implemented in this build; use Node's own timers instead.", NewStringType::kNormal).ToLocalChecked())));
}

void uWS_setTimeout(const FunctionCallbackInfo<Value> &args) {

    /* Function, integer */

    Isolate *isolate = args.GetIsolate();
    args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "uWS.setTimeout is not implemented in this build; use Node's own timers instead.", NewStringType::kNormal).ToLocalChecked())));
}

void uWS_clearTimeout(const FunctionCallbackInfo<Value> &args) {

    /* Integer */

    Isolate *isolate = args.GetIsolate();
    args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "uWS.clearTimeout is not implemented in this build; use Node's own timers instead.", NewStringType::kNormal).ToLocalChecked())));
}

/* Pass various undocumented configs */
void uWS_cfg(const FunctionCallbackInfo<Value> &args) {
    NativeString key(args.GetIsolate(), args[0]);
    if (key.isInvalid(args)) {
        return;
    }

    /* Compare the key as a real string: the previous character-sum check treated any
     * anagram ("listen", "tinsel", "enlist") as "silent" and silenced the server. */
    if (key.getString() == "silent") {
        uWS::Loop::get()->setSilent(true);
    }
}

/* Both of the following take a socket handed to JavaScript as an External; anything else
 * would be cast to a bogus pointer and dereferenced, killing the process */
static inline bool invalidSocket(const FunctionCallbackInfo<Value> &args, const char *message) {
    if (!args[0]->IsExternal()) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), message, NewStringType::kNormal).ToLocalChecked())));
        return true;
    }
    return false;
}

/* todo: Put this function and all inits of it in its own header */
void uWS_us_listen_socket_close(const FunctionCallbackInfo<Value> &args) {
    // this should take int ssl first
    if (invalidSocket(args, "us_listen_socket_close must be passed the listen socket given by listen.")) {
        return;
    }
    us_listen_socket_close(0, (struct us_listen_socket_t *) External::Cast(*args[0])->Value());
}

void uWS_us_socket_local_port(const FunctionCallbackInfo<Value> &args) {
    // this should take int ssl first, but us_socket_local_port doesn't use it so it doesn't matter
    if (invalidSocket(args, "us_socket_local_port must be passed a socket or listen socket.")) {
        return;
    }
    int port = us_socket_local_port(0, (struct us_socket_t *) External::Cast(*args[0])->Value());
    args.GetReturnValue().Set(Integer::New(args.GetIsolate(), port));
}

/* Temporary KV store (doesn't belong here) */
#include <unordered_map>
#include <string>
#include <mutex>

std::unordered_map<std::string, std::unordered_map<std::string, std::string>> kvStoreString;
std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> kvStoreInteger;

/* The store itself is shared by every worker thread of the process; the JS-facing
 * uWS.lock()/uWS.unlock() pair was always cooperative (no KV function ever took it), so
 * each KV function guards the map access with its own mutex. Kept separate from the client
 * lock so a client lock left held can no longer wedge the store. */
std::mutex kvStoreMutex;

/* Re-entrant so a double uWS.lock() cannot self-deadlock, and mismatched uWS.unlock()
 * becomes a catchable Error instead of undefined behaviour */
std::recursive_mutex kvClientMutex;
thread_local int kvLockDepth = 0;

// getString(key, collection)
void uWS_getString(const FunctionCallbackInfo<Value> &args) {
    NativeString key(args.GetIsolate(), args[0]);
    if (key.isInvalid(args)) {
        return;
    }

    NativeString collection(args.GetIsolate(), args[1]);
    if (collection.isInvalid(args)) {
        return;
    }

    std::string value;
    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        value = kvStoreString[std::string(collection.getString())][std::string(key.getString())];
    }

    args.GetReturnValue().Set(String::NewFromUtf8(args.GetIsolate(), value.data(), NewStringType::kNormal, value.length()).ToLocalChecked());
}

void uWS_setString(const FunctionCallbackInfo<Value> &args) {
    NativeString key(args.GetIsolate(), args[0]);
    if (key.isInvalid(args)) {
        return;
    }
    NativeString value(args.GetIsolate(), args[1]);
    if (value.isInvalid(args)) {
        return;
    }

    NativeString collection(args.GetIsolate(), args[2]);
    if (collection.isInvalid(args)) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        kvStoreString[std::string(collection.getString())][std::string(key.getString())] = value.getString();
    }
}

void uWS_getInteger(const FunctionCallbackInfo<Value> &args) {
    NativeString key(args.GetIsolate(), args[0]);
    if (key.isInvalid(args)) {
        return;
    }

    NativeString collection(args.GetIsolate(), args[1]);
    if (collection.isInvalid(args)) {
        return;
    }

    uint32_t value;
    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        value = kvStoreInteger[std::string(collection.getString())][std::string(key.getString())];
    }

    args.GetReturnValue().Set(Integer::New(args.GetIsolate(), value));
}

void uWS_setInteger(const FunctionCallbackInfo<Value> &args) {
    NativeString key(args.GetIsolate(), args[0]);
    if (key.isInvalid(args)) {
        return;
    }

    /* Local<Integer>::Cast is unchecked in a release build, so a non-number (undefined included)
     * would become a bogus handle whose Value() reads whatever is there. */
    if (!args[1]->IsNumber()) {
        throwTypeError(args, "Passed argument is not a number.");
        return;
    }
    uint32_t value = Local<Integer>::Cast(args[1])->Uint32Value(args.GetIsolate()->GetCurrentContext()).ToChecked();

    NativeString collection(args.GetIsolate(), args[2]);
    if (collection.isInvalid(args)) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        kvStoreInteger[std::string(collection.getString())][std::string(key.getString())] = value;
    }
}

void uWS_incInteger(const FunctionCallbackInfo<Value> &args) {
    NativeString key(args.GetIsolate(), args[0]);
    if (key.isInvalid(args)) {
        return;
    }

    /* See uWS_setInteger */
    if (!args[1]->IsNumber()) {
        throwTypeError(args, "Passed argument is not a number.");
        return;
    }
    uint32_t change = Local<Integer>::Cast(args[1])->Uint32Value(args.GetIsolate()->GetCurrentContext()).ToChecked();

    NativeString collection(args.GetIsolate(), args[2]);
    if (collection.isInvalid(args)) {
        return;
    }

    uint32_t value;
    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        value = kvStoreInteger[std::string(collection.getString())][std::string(key.getString())] += change;
    }

    args.GetReturnValue().Set(Integer::New(args.GetIsolate(), value));
}

/* This one will spike memory usage for large stores */
void uWS_getStringKeys(const FunctionCallbackInfo<Value> &args) {

    NativeString collection(args.GetIsolate(), args[0]);
    if (collection.isInvalid(args)) {
        return;
    }

    /* Copy the keys out under the lock and build the V8 array only after releasing it:
     * Array::Set can run JS (the array is created with no own elements, so storing index 0..n-1
     * walks the prototype chain and can call a user accessor), and JS that runs while
     * kvStoreMutex is held can re-enter a KV function and block forever on this non-recursive
     * mutex. The lookup stays inside the guard: operator[] default-inserts the collection, which
     * must stay atomic and observable the same way it was. */
    std::vector<std::string> keysCopy;
    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        auto &keys = kvStoreString[std::string(collection.getString())];
        keysCopy.reserve(keys.size());
        for (auto &p : keys) {
            keysCopy.push_back(p.first);
        }
    }

    Local<Array> stringKeys = Array::New(args.GetIsolate(), keysCopy.size());

    int offset = 0;

    for (auto &key : keysCopy) {
        stringKeys->Set(args.GetIsolate()->GetCurrentContext(), offset++, String::NewFromUtf8(args.GetIsolate(), key.data(), NewStringType::kNormal, key.length()).ToLocalChecked()).IsNothing();
    }

    args.GetReturnValue().Set(stringKeys);
}

void uWS_getIntegerKeys(const FunctionCallbackInfo<Value> &args) {

    NativeString collection(args.GetIsolate(), args[0]);
    if (collection.isInvalid(args)) {
        return;
    }

    /* Same split as getStringKeys: copy under the lock, build the V8 array after it. */
    std::vector<std::string> keysCopy;
    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        auto &keys = kvStoreInteger[std::string(collection.getString())];
        keysCopy.reserve(keys.size());
        for (auto &p : keys) {
            keysCopy.push_back(p.first);
        }
    }

    Local<Array> integerKeys = Array::New(args.GetIsolate(), keysCopy.size());

    int offset = 0;

    for (auto &key : keysCopy) {
        integerKeys->Set(args.GetIsolate()->GetCurrentContext(), offset++, String::NewFromUtf8(args.GetIsolate(), key.data(), NewStringType::kNormal, key.length()).ToLocalChecked()).IsNothing();
    }

    args.GetReturnValue().Set(integerKeys);
}

void uWS_deleteString(const FunctionCallbackInfo<Value> &args) {

    NativeString key(args.GetIsolate(), args[0]);
    if (key.isInvalid(args)) {
        return;
    }

    NativeString collection(args.GetIsolate(), args[1]);
    if (collection.isInvalid(args)) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        kvStoreString[std::string(collection.getString())].erase(std::string(key.getString()));
    }

    //args.GetReturnValue().Set(Integer::New(args.GetIsolate(), value));
}

void uWS_deleteInteger(const FunctionCallbackInfo<Value> &args) {

    NativeString key(args.GetIsolate(), args[0]);
    if (key.isInvalid(args)) {
        return;
    }

    NativeString collection(args.GetIsolate(), args[1]);
    if (collection.isInvalid(args)) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        kvStoreInteger[std::string(collection.getString())].erase(std::string(key.getString()));
    }

    //args.GetReturnValue().Set(Integer::New(args.GetIsolate(), value));
}

void uWS_deleteStringCollection(const FunctionCallbackInfo<Value> &args) {

    NativeString collection(args.GetIsolate(), args[0]);
    if (collection.isInvalid(args)) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        kvStoreString.erase(std::string(collection.getString()));
    }

    //args.GetReturnValue().Set(integerKeys);
}

void uWS_deleteIntegerCollection(const FunctionCallbackInfo<Value> &args) {

    NativeString collection(args.GetIsolate(), args[0]);
    if (collection.isInvalid(args)) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(kvStoreMutex);
        kvStoreInteger.erase(std::string(collection.getString()));
    }

    //args.GetReturnValue().Set(integerKeys);
}

void uWS_lock(const FunctionCallbackInfo<Value> &args) {
    if (kvLockDepth++ == 0) {
        kvClientMutex.lock();
    }
}

void uWS_unlock(const FunctionCallbackInfo<Value> &args) {
    if (kvLockDepth == 0) {
        Isolate *isolate = args.GetIsolate();
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "uWS.unlock called without a matching uWS.lock().", NewStringType::kNormal).ToLocalChecked())));
        return;
    }
    if (--kvLockDepth == 0) {
        kvClientMutex.unlock();
    }
}

PerContextData *Main(Isolate *isolate, Local<Object> exports) {

    /* Init the template objects, SSL and non-SSL, store it in per context data */
    PerContextData *perContextData = new PerContextData;
    perContextData->isolate = isolate;
    /* Let callbacks reach the isolate without capturing it (see src/Utilities.h); this
     * pointer must stay valid until the cleanup hook deletes PerContextData */
    currentPerContextData = perContextData;
    perContextData->reqTemplate[0].Reset(isolate, HttpRequestWrapper::init<false>(isolate));
    perContextData->reqTemplate[1].Reset(isolate, HttpRequestWrapper::init<true>(isolate));
    perContextData->resTemplate[0].Reset(isolate, HttpResponseWrapper::init<0>(isolate));
    perContextData->resTemplate[1].Reset(isolate, HttpResponseWrapper::init<1>(isolate));
    perContextData->resTemplate[2].Reset(isolate, HttpResponseWrapper::init<2>(isolate));
    perContextData->resTemplate[3].Reset(isolate, HttpResponseWrapper::init<3>(isolate));
    perContextData->wsTemplate[0].Reset(isolate, WebSocketWrapper::init<0>(isolate));
    perContextData->wsTemplate[1].Reset(isolate, WebSocketWrapper::init<1>(isolate));

    /* Refer to per context data via External */
    Local<External> externalPerContextData = External::New(isolate, perContextData);

    /* uWS namespace */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "App", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App<uWS::App>, externalPerContextData)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "SSLApp", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App<uWS::SSLApp>, externalPerContextData)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();

    /* H3 experimental */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "H3App", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App<uWS::H3App>, externalPerContextData)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();

    /* Temporary KV store */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "getString", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_getString)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "setString", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_setString)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "getInteger", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_getInteger)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "setInteger", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_setInteger)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "incInteger", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_incInteger)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "lock", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_lock)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "unlock", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_unlock)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "getIntegerKeys", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_getIntegerKeys)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "getStringKeys", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_getStringKeys)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "deleteString", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_deleteString)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "deleteInteger", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_deleteInteger)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "deleteStringCollection", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_deleteStringCollection)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "deleteIntegerCollection", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_deleteIntegerCollection)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();

    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "setTimeout", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_setTimeout)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "clearTimeout", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_clearTimeout)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "arm", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_arm)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();

    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "_cfg", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_cfg)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "getParts", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_getParts)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    
    /* Expose some µSockets functions directly under uWS namespace */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "us_listen_socket_close", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_us_listen_socket_close)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "us_socket_local_port", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_us_socket_local_port)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();

    /* Compression enum */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DISABLED", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DISABLED)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "SHARED_COMPRESSOR", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::SHARED_COMPRESSOR)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "SHARED_DECOMPRESSOR", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::SHARED_DECOMPRESSOR)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_3KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_3KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_4KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_4KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_8KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_8KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_16KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_16KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_32KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_32KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_64KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_64KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_128KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_128KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_256KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_256KB)).ToChecked();

    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_32KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_32KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_16KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_16KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_8KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_8KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_4KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_4KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_2KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_2KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_1KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_1KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_512B", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_512B)).ToChecked();

    /* Listen options */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "LIBUS_LISTEN_EXCLUSIVE_PORT", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, LIBUS_LISTEN_EXCLUSIVE_PORT)).ToChecked();

    return perContextData;
}

/* This is required when building as a Node.js addon */
#ifndef ADDON_IS_HOST
#include <node.h>
extern "C" NODE_MODULE_EXPORT void
NODE_MODULE_INITIALIZER(Local<Object> exports, Local<Value> module, Local<Context> context) {
    Isolate *isolate = Isolate::GetCurrent();
    /* Integrate uSockets with existing libuv loop */
    uWS::Loop::get(node::GetCurrentEventLoop(isolate));
    /* Register vanilla V8 addon */
    PerContextData *perContextData = Main(isolate, exports);

    /* We cannot rely on process.exit or process.beforeExit when it comes to WorkerThreads */
    node::AddEnvironmentCleanupHook(isolate, [](void *arg) {

        PerContextData *perContextData = (PerContextData *) arg;

        /* Descriptors must stop being accepted before the apps they point to go away */
        uWS_unregisterLiveAppsFor(perContextData);

        /* Freeing apps here, it could be done earlier but not sooner */
        perContextData->apps.clear();
        perContextData->sslApps.clear();
        /* Freeing the loop here means we give time for our timers to close, etc */
        uWS::Loop::get()->free();

        /* We can safely delete this since we no longer can call uWS.free */
        delete perContextData;

    }, perContextData);
}
#endif

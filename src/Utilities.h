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

#ifndef ADDON_UTILITIES_H
#define ADDON_UTILITIES_H

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <v8.h>
using namespace v8;

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/* Getting internal pointer is different in recent V8 versions.
 * Both gates must be >= 14, not == 14: the one-arg GetAlignedPointerFromInternalField and the
 * deprecated int WriteUtf8/WriteOneByte pair are already gone from the V8 14.6 headers node 26
 * ships, so a V8 15 must not fall into the #else branch. */
#if (V8_MAJOR_VERSION >= 14)
    void *getInternalPointer(const Local<Object> &holder) {
        return holder->GetAlignedPointerFromInternalField(0, 0);
    }

    void setInternalPointer(const Local<Object> &holder, void *value) {
        holder->SetAlignedPointerInInternalField(0, value, 0);
    }
#else
    void *getInternalPointer(const Local<Object> &holder) {
        return holder->GetAlignedPointerFromInternalField(0);
    }

    void setInternalPointer(const Local<Object> &holder, void *value) {
        holder->SetAlignedPointerInInternalField(0, value);
    }
#endif

/* A Global<T> whose move constructor is noexcept. v8::Global's move constructor is declared
 * without noexcept even though it only relocates a handle cell, and ofats::any_detail (see
 * uWebSockets/src/MoveOnlyFunction.h) only keeps a closure in its 16-byte inline buffer when
 * the closure is nothrow-move-constructible. Wrapping the Global is what lets the per-request
 * response callbacks avoid one heap allocation each. */
template <class T>
struct NothrowGlobal {
    Global<T> g;
    NothrowGlobal() = default;
    NothrowGlobal(Isolate *isolate, Local<T> value) : g(isolate, value) {}
    NothrowGlobal(NothrowGlobal &&other) noexcept : g(std::move(other.g)) {}
    NothrowGlobal &operator=(NothrowGlobal &&other) noexcept {
        if (this != &other) {
            g = std::move(other.g);
        }
        return *this;
    }
    NothrowGlobal(const NothrowGlobal &) = delete;
    NothrowGlobal &operator=(const NothrowGlobal &) = delete;
    void Reset() { g.Reset(); }
    void Reset(Isolate *isolate, Local<T> value) { g.Reset(isolate, value); }
    bool IsEmpty() const { return g.IsEmpty(); }
    Local<T> Get(Isolate *isolate) const { return g.Get(isolate); }
};

/* Unfortunately we _have_ to depend on Node.js crap */
#include <node.h>

MaybeLocal<Value> CallJS(Isolate *isolate, Local<Function> f, int argc, Local<Value> *argv) {
    extern int calledIntoJS;
    extern thread_local int insideCorkCallback;
    /* All calls we do into JS are properly corked, except for res.cork, where we increase the counter explicitly */
    insideCorkCallback++;
    /* Slow path */
    auto ret = node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), f, argc, argv, {0, 0});
    insideCorkCallback--;
    return ret;
}

Local<v8::ArrayBuffer> ArrayBuffer_New(Isolate *isolate, void *data, size_t length) {
    std::unique_ptr<BackingStore> backingStore = ArrayBuffer::NewBackingStore(data, length, [](void* data, size_t length, void* deleter_data) {}, nullptr);
    return ArrayBuffer::New(isolate, std::shared_ptr<BackingStore>(backingStore.release()));
}

Local<v8::ArrayBuffer> ArrayBuffer_NewCopy(Isolate *isolate, void *data, size_t length) {
    Local<ArrayBuffer> ab = ArrayBuffer::New(isolate, length);
    memcpy(ab->GetBackingStore()->Data(), data, length);
    return ab;
}

struct PerSocketData {
    UniquePersistent<Object> socketPf;
};

/* Set once in Main() (src/addon.cpp) while the per-context data is alive; per-request callbacks read
 * the isolate from it instead of capturing it, which keeps their closures inside MoveOnlyFunction's
 * inline storage. Declared here so ~PerContextData can clear it, defined right after the struct. */
struct PerContextData;
extern thread_local PerContextData *currentPerContextData;

struct PerContextData {
    Isolate *isolate;
    UniquePersistent<Object> reqTemplate[2]; // 0 = non-SSL/SSL, 1 = Http3
    UniquePersistent<Object> resTemplate[4]; // 0 = non-SSL, 1 = SSL, 2 = Http3
    UniquePersistent<Object> wsTemplate[2];

    /* We hold all apps until free */
    std::vector<std::unique_ptr<uWS::App>> apps;
    std::vector<std::unique_ptr<uWS::SSLApp>> sslApps;

    /* One persistent handle per app that asked for a descriptor, so getDescriptor does not leak a
     * Global per call. Released when this PerContextData is deleted in the addon cleanup hook. */
    std::vector<std::pair<void *, std::unique_ptr<UniquePersistent<Object>>>> appDescriptors;

    /* The cleanup hook deletes this object as the isolate goes away; stop advertising it so a
     * callback that somehow still runs reads a null pointer instead of freed memory. */
    ~PerContextData() {
        if (currentPerContextData == this) {
            currentPerContextData = nullptr;
        }
    }
};

inline thread_local PerContextData *currentPerContextData = nullptr;

template <class APP>
static constexpr int getAppTypeIndex() {
    /* Returns 1 for SSLApp and 0 for App */
    //return std::is_same<APP, uWS::SSLApp>::value;

    /* Returns 2 for H3App */

    if constexpr (std::is_same<APP, uWS::App>::value) {
        return 0;
    } else if constexpr (std::is_same<APP, uWS::SSLApp>::value) {
        return 1;
    } else if constexpr (std::is_same<APP, uWS::H3App>::value) {
        return 2;
    } else {
        // why does this fail?
        //static_assert(false);
    }
}

static inline bool missingArguments(int length, const FunctionCallbackInfo<Value> &args) {
    if (args.Length() < length) {
        std::string message = "Function requires at least ";
        message += std::to_string(length);
        message += " arguments.";
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), message.c_str(), NewStringType::kNormal).ToLocalChecked())));
        return true;
    }
    return false;
}

static inline void throwTypeError(const FunctionCallbackInfo<Value> &args, const char *message) {
    Isolate *isolate = args.GetIsolate();
    args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::TypeError(String::NewFromUtf8(isolate, message, NewStringType::kNormal).ToLocalChecked())));
}

/* Header names must be a non-empty RFC 9110 token (tchar); ':', SP, HTAB, CR, LF and every byte
 * below 0x21 or at/above 0x7F are rejected, which is what Node rejects with ERR_INVALID_HTTP_TOKEN. */
static inline bool isValidHeaderName(std::string_view name) {
    if (name.length() == 0) {
        return false;
    }
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            continue;
        }
        switch (c) {
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
            case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                continue;
            default:
                return false;
        }
    }
    return true;
}

/* Field values allow HTAB, 0x20-0x7E and obs-text 0x80-0xFF verbatim; control bytes and 0x7F are
 * rejected, which is what Node rejects with ERR_INVALID_CHAR. The empty value is valid. */
static inline bool isValidHeaderValue(std::string_view value) {
    for (unsigned char c : value) {
        if (c == '\t') {
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

/* Three ASCII digits, then optionally a single SP and a reason phrase with field-value rules.
 * The space is deliberately not required: writeStatus('200') has always emitted "HTTP/1.1 200\r\n"
 * and that must not change. Values above 0xFF must be rejected before one-byte encoding. */
static inline bool isValidStatusLine(std::string_view status) {
    if (status.length() < 3) {
        return false;
    }
    for (size_t i = 0; i < 3; i++) {
        if (status[i] < '0' || status[i] > '9') {
            return false;
        }
    }
    if (status.length() == 3) {
        return true;
    }
    if (status[3] != ' ') {
        return false;
    }
    return isValidHeaderValue(status.substr(4));
}

/* True for a JS string that one-byte (latin-1) encoding can represent exactly. Checked BEFORE
 * one-byte encoding: WriteOneByte truncates a code unit above 0xFF instead of failing. */
static inline bool isLatin1String(Local<Value> value) {
    return value->IsString() && Local<String>::Cast(value)->ContainsOnlyOneByte();
}

enum class ArgReadResult {
    Ok,
    NotProvided,
    Failed
};

/* Checked numeric argument reader, used wherever an argument is narrowed to size_t.
 * NotProvided: the argument is absent, undefined or null - the caller keeps its optional-argument
 * behaviour. Failed: the caller must return without using the value; either an exception was
 * thrown here, or the value's own conversion (a throwing valueOf) already scheduled one. */
static inline ArgReadResult readSizeArg(const FunctionCallbackInfo<Value> &args, int index, double maxValue, size_t &out) {
    if (index >= args.Length() || args[index]->IsUndefined() || args[index]->IsNull()) {
        return ArgReadResult::NotProvided;
    }

    Isolate *isolate = args.GetIsolate();
    if (!args[index]->IsNumber()) {
        throwTypeError(args, "Passed argument is not a number.");
        return ArgReadResult::Failed;
    }

    double value;
    if (!args[index]->NumberValue(isolate->GetCurrentContext()).To(&value)) {
        /* Conversion of a primitive number cannot fail (measured: IsNumber() is false for Number
         * objects in V8 14, so a valueOf can never run here), but if it ever did, its own exception
         * is pending - return without replacing it with ours. */
        return ArgReadResult::Failed;
    }

    if (!std::isfinite(value) || value < 0 || value > maxValue) {
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::RangeError(String::NewFromUtf8(isolate, "Passed argument is out of range.", NewStringType::kNormal).ToLocalChecked())));
        return ArgReadResult::Failed;
    }

    out = (size_t) value;
    return ArgReadResult::Ok;
}

struct Callback {
    bool invalid = false;
    UniquePersistent<Function> f;
    Callback(Isolate *isolate, const Local<Value> &value) {

        if (!value->IsFunction()) {
            invalid = true;
            return;
        }

        f.Reset(isolate, Local<Function>::Cast(value));
    }

    bool isInvalid(const FunctionCallbackInfo<Value> &args) {
        if (invalid) {
            args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Passed callback is not a valid function.", NewStringType::kNormal).ToLocalChecked())));
        }
        return invalid;
    }

    UniquePersistent<Function> &&getFunction() {
        return std::move(f);
    }
};

template <bool AllowStringView = false>
class NativeString {
    char *data;
    size_t length;
    bool allocated = false;
    bool invalid = false;

    // Static thread-local state shared by all NativeString instances on this thread
    inline static thread_local std::vector<char> pool = std::vector<char>(128 * 1024);
    inline static thread_local size_t pool_offset = 0;
    inline static thread_local int ref_count = 0;

    static char* alloc(size_t size) {
        // Ensure size is a multiple of 8
        size = (size + 7) & ~7;

        // Fallback for allocations larger than the remaining pool space
        if (pool_offset + size > pool.size()) {
            // Mark for external cleanup if using instance-based logic
            // (Note: In a pure static alloc, you'd need a way to track this)
            return (char*)std::malloc(size);
        }

        char* ptr = pool.data() + pool_offset;
        pool_offset += size;
        return ptr;
    }

    // Provided for completeness, though the "pool" doesn't actually free individual slices
    static void free(char* ptr) {
        if (ptr < pool.data() || ptr >= pool.data() + pool.size()) {
            ::free(ptr);
        }
    }

public:
    NativeString(Isolate *isolate, const Local<Value> &value) {
        if (ref_count == 0) {
            pool_offset = 0; // Reset the "stack" when entering the first scope
        }
        ref_count++;

        if (value->IsUndefined()) {
            data = nullptr;
            length = 0;
        } else if (value->IsString()) {
            Local<String> string = Local<String>::Cast(value);

            /* Bodies are UTF-8. REPLACE_INVALID_UTF8 turns an unpaired surrogate into U+FFFD
             * (both are 3 bytes, so Utf8Length/Utf8LengthV2 stays correct). The AllowStringView
             * template parameter is retained only for source compatibility; it no longer selects
             * an encoding - writeStatus/writeHeader use NativeStringOneByte instead. */

            #if (V8_MAJOR_VERSION >= 14)
                // Fallback
                length = string->Utf8LengthV2(isolate);
                data = alloc(length);
                allocated = true;
                string->WriteUtf8V2(isolate, data, length, String::WriteFlags::kReplaceInvalidUtf8);
            #else
                // Fallback
                length = string->Utf8Length(isolate);
                data = alloc(length);
                allocated = true;
                string->WriteUtf8(isolate, data, length, nullptr, String::WriteOptions::REPLACE_INVALID_UTF8 | String::WriteOptions::NO_NULL_TERMINATION);
            #endif


        } else if (value->IsArrayBufferView()) { /* DataView or TypedArray */
            Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(value);
            auto contents = arrayBufferView->Buffer()->GetBackingStore();
            length = arrayBufferView->ByteLength();
            data = (char *) contents->Data() + arrayBufferView->ByteOffset();
        } else if (value->IsArrayBuffer()) {
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
        } else if (value->IsSharedArrayBuffer()) {
            Local<SharedArrayBuffer> arrayBuffer = Local<SharedArrayBuffer>::Cast(value);
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
        } else {
            invalid = true;
        }
    }

    bool isInvalid(const FunctionCallbackInfo<Value> &args) {
        if (invalid) {
            args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Text and data can only be passed by String, ArrayBuffer or ArrayBufferView.", NewStringType::kNormal).ToLocalChecked())));
        }
        return invalid;
    }

    std::string_view getString() {
        return {data, length};
    }

    ~NativeString() {
        ref_count--;
        if (allocated) {
            free(data);
        }
    }
};

/* One-byte (latin-1) sibling of NativeString, for the wire paths that Node writes as latin-1:
 * the status line and header names/values. One code unit becomes exactly one byte, so a value
 * above U+00FF would be silently truncated - callers must reject such strings with
 * isLatin1String() before encoding. ArrayBuffer/SharedArrayBuffer/ArrayBufferView pass through
 * unchanged, which is how callers hand over raw header bytes. Own pool and ref count: it must not
 * share bookkeeping with NativeString because the two are used in different scopes. */
class NativeStringOneByte {
    char *data;
    size_t length;
    bool allocated = false;
    bool invalid = false;

    // Static thread-local state shared by all NativeStringOneByte instances on this thread
    inline static thread_local std::vector<char> pool = std::vector<char>(128 * 1024);
    inline static thread_local size_t pool_offset = 0;
    inline static thread_local int ref_count = 0;

    static char* alloc(size_t size) {
        // Ensure size is a multiple of 8
        size = (size + 7) & ~7;

        // Fallback for allocations larger than the remaining pool space
        if (pool_offset + size > pool.size()) {
            return (char*)std::malloc(size);
        }

        char* ptr = pool.data() + pool_offset;
        pool_offset += size;
        return ptr;
    }

    static void free(char* ptr) {
        if (ptr < pool.data() || ptr >= pool.data() + pool.size()) {
            ::free(ptr);
        }
    }

public:
    NativeStringOneByte(Isolate *isolate, const Local<Value> &value) {
        if (ref_count == 0) {
            pool_offset = 0; // Reset the "stack" when entering the first scope
        }
        ref_count++;

        if (value->IsUndefined()) {
            data = nullptr;
            length = 0;
        } else if (value->IsString()) {
            Local<String> string = Local<String>::Cast(value);

            length = string->Length();
            data = alloc(length);
            allocated = true;

            #if (V8_MAJOR_VERSION >= 14)
                string->WriteOneByteV2(isolate, 0, (uint32_t) string->Length(), (uint8_t *) data);
            #else
                string->WriteOneByte(isolate, (uint8_t *) data, 0, length, String::WriteOptions::NO_NULL_TERMINATION);
            #endif
        } else if (value->IsArrayBufferView()) { /* DataView or TypedArray */
            Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(value);
            auto contents = arrayBufferView->Buffer()->GetBackingStore();
            length = arrayBufferView->ByteLength();
            data = (char *) contents->Data() + arrayBufferView->ByteOffset();
        } else if (value->IsArrayBuffer()) {
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
        } else if (value->IsSharedArrayBuffer()) {
            Local<SharedArrayBuffer> arrayBuffer = Local<SharedArrayBuffer>::Cast(value);
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
        } else {
            invalid = true;
        }
    }

    bool isInvalid(const FunctionCallbackInfo<Value> &args) {
        if (invalid) {
            args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Text and data can only be passed by String, ArrayBuffer or ArrayBufferView.", NewStringType::kNormal).ToLocalChecked())));
        }
        return invalid;
    }

    std::string_view getString() {
        return {data, length};
    }

    ~NativeStringOneByte() {
        ref_count--;
        if (allocated) {
            free(data);
        }
    }
};

// Utility function to extract raw certificate data
std::string extractX509PemCertificate(SSL* ssl) {
    std::string pemCertificate;

    if (!ssl) {
        return pemCertificate;
    }

    // Get the peer certificate
    X509* peerCertificate = SSL_get_peer_certificate(ssl);
    if (!peerCertificate) {
        // No peer certificate available
        return pemCertificate;
    }

    // Convert X509 certificate to PEM format
    BIO* bio = BIO_new(BIO_s_mem());
    if(bio) {
        if (PEM_write_bio_X509(bio, peerCertificate)) {
            char* buffer;
            long length = BIO_get_mem_data(bio, &buffer);
            pemCertificate.assign(buffer, length);
        }
        BIO_free(bio);
    }

    // Free the peer certificate
    X509_free(peerCertificate);
    return pemCertificate;
}

#endif

/* Copyright (C) 2016 The Android Open Source Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This file implements interfaces from the file jvmti.h. This implementation
 * is licensed under the same terms as the file jvmti.h.  The
 * copyright and license information for the file jvmti.h follows.
 *
 * Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "ti_method.h"

#include "art_jvmti.h"
#include "art_method-inl.h"
#include "base/enums.h"
#include "jni_internal.h"
#include "modifiers.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-inl.h"

namespace openjdkjvmti {

jvmtiError MethodUtil::GetMethodName(jvmtiEnv* env,
                                     jmethodID method,
                                     char** name_ptr,
                                     char** signature_ptr,
                                     char** generic_ptr) {
  art::ScopedObjectAccess soa(art::Thread::Current());
  art::ArtMethod* art_method = art::jni::DecodeArtMethod(method);
  art_method = art_method->GetInterfaceMethodIfProxy(art::kRuntimePointerSize);

  JvmtiUniquePtr name_copy;
  if (name_ptr != nullptr) {
    const char* method_name = art_method->GetName();
    if (method_name == nullptr) {
      method_name = "<error>";
    }
    unsigned char* tmp;
    jvmtiError ret = CopyString(env, method_name, &tmp);
    if (ret != ERR(NONE)) {
      return ret;
    }
    name_copy = MakeJvmtiUniquePtr(env, tmp);
    *name_ptr = reinterpret_cast<char*>(tmp);
  }

  JvmtiUniquePtr signature_copy;
  if (signature_ptr != nullptr) {
    const art::Signature sig = art_method->GetSignature();
    std::string str = sig.ToString();
    unsigned char* tmp;
    jvmtiError ret = CopyString(env, str.c_str(), &tmp);
    if (ret != ERR(NONE)) {
      return ret;
    }
    signature_copy = MakeJvmtiUniquePtr(env, tmp);
    *signature_ptr = reinterpret_cast<char*>(tmp);
  }

  // TODO: Support generic signature.
  if (generic_ptr != nullptr) {
    *generic_ptr = nullptr;
  }

  // Everything is fine, release the buffers.
  name_copy.release();
  signature_copy.release();

  return ERR(NONE);
}

jvmtiError MethodUtil::GetMethodDeclaringClass(jvmtiEnv* env ATTRIBUTE_UNUSED,
                                               jmethodID method,
                                               jclass* declaring_class_ptr) {
  if (declaring_class_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  art::ArtMethod* art_method = art::jni::DecodeArtMethod(method);
  // Note: No GetInterfaceMethodIfProxy, we want to actual class.

  art::ScopedObjectAccess soa(art::Thread::Current());
  art::mirror::Class* klass = art_method->GetDeclaringClass();
  *declaring_class_ptr = soa.AddLocalReference<jclass>(klass);

  return ERR(NONE);
}

jvmtiError MethodUtil::GetMethodModifiers(jvmtiEnv* env ATTRIBUTE_UNUSED,
                                          jmethodID method,
                                          jint* modifiers_ptr) {
  if (modifiers_ptr == nullptr) {
    return ERR(NULL_POINTER);
  }

  art::ArtMethod* art_method = art::jni::DecodeArtMethod(method);
  uint32_t modifiers = art_method->GetAccessFlags();

  // Note: Keep this code in sync with Executable.fixMethodFlags.
  if ((modifiers & art::kAccAbstract) != 0) {
    modifiers &= ~art::kAccNative;
  }
  modifiers &= ~art::kAccSynchronized;
  if ((modifiers & art::kAccDeclaredSynchronized) != 0) {
    modifiers |= art::kAccSynchronized;
  }
  modifiers &= art::kAccJavaFlagsMask;

  *modifiers_ptr = modifiers;
  return ERR(NONE);
}

using LineNumberContext = std::vector<jvmtiLineNumberEntry>;

static bool CollectLineNumbers(void* void_context, const art::DexFile::PositionInfo& entry) {
  LineNumberContext* context = reinterpret_cast<LineNumberContext*>(void_context);
  jvmtiLineNumberEntry jvmti_entry = { static_cast<jlocation>(entry.address_),
                                       static_cast<jint>(entry.line_) };
  context->push_back(jvmti_entry);
  return false;  // Collect all, no early exit.
}

jvmtiError MethodUtil::GetLineNumberTable(jvmtiEnv* env,
                                          jmethodID method,
                                          jint* entry_count_ptr,
                                          jvmtiLineNumberEntry** table_ptr) {
  if (method == nullptr) {
    return ERR(NULL_POINTER);
  }
  art::ArtMethod* art_method = art::jni::DecodeArtMethod(method);
  DCHECK(!art_method->IsRuntimeMethod());

  const art::DexFile::CodeItem* code_item;
  const art::DexFile* dex_file;
  {
    art::ScopedObjectAccess soa(art::Thread::Current());

    if (art_method->IsProxyMethod()) {
      return ERR(ABSENT_INFORMATION);
    }
    if (art_method->IsNative()) {
      return ERR(NATIVE_METHOD);
    }
    if (entry_count_ptr == nullptr || table_ptr == nullptr) {
      return ERR(NULL_POINTER);
    }

    code_item = art_method->GetCodeItem();
    dex_file = art_method->GetDexFile();
    DCHECK(code_item != nullptr) << art_method->PrettyMethod() << " " << dex_file->GetLocation();
  }

  LineNumberContext context;
  bool success = dex_file->DecodeDebugPositionInfo(code_item, CollectLineNumbers, &context);
  if (!success) {
    return ERR(ABSENT_INFORMATION);
  }

  unsigned char* data;
  jlong mem_size = context.size() * sizeof(jvmtiLineNumberEntry);
  jvmtiError alloc_error = env->Allocate(mem_size, &data);
  if (alloc_error != ERR(NONE)) {
    return alloc_error;
  }
  *table_ptr = reinterpret_cast<jvmtiLineNumberEntry*>(data);
  memcpy(*table_ptr, context.data(), mem_size);
  *entry_count_ptr = static_cast<jint>(context.size());

  return ERR(NONE);
}

}  // namespace openjdkjvmti
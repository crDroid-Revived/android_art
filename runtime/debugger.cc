/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "debugger.h"

#include <sys/uio.h>

#include <functional>
#include <memory>
#include <set>
#include <vector>

#include "android-base/stringprintf.h"

#include "arch/context.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/enums.h"
#include "base/safe_map.h"
#include "base/strlcpy.h"
#include "base/time_utils.h"
#include "class_linker-inl.h"
#include "class_linker.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_annotations.h"
#include "dex/dex_file_types.h"
#include "dex/dex_instruction.h"
#include "dex/utf.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/allocation_record.h"
#include "gc/gc_cause.h"
#include "gc/scoped_gc_critical_section.h"
#include "gc/space/bump_pointer_space-walk-inl.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "handle_scope-inl.h"
#include "jdwp/jdwp_priv.h"
#include "jdwp/object_registry-inl.h"
#include "jni/jni_internal.h"
#include "jvalue-inl.h"
#include "mirror/array-alloc-inl.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-alloc-inl.h"
#include "mirror/string-inl.h"
#include "mirror/throwable.h"
#include "nativehelper/scoped_local_ref.h"
#include "nativehelper/scoped_primitive_array.h"
#include "oat_file.h"
#include "obj_ptr-inl.h"
#include "reflection.h"
#include "runtime-inl.h"
#include "scoped_thread_state_change-inl.h"
#include "stack.h"
#include "thread_list.h"
#include "well_known_classes.h"

namespace art {

using android::base::StringPrintf;

// The key identifying the debugger to update instrumentation.
static constexpr const char* kDbgInstrumentationKey = "Debugger";

// Limit alloc_record_count to the 2BE value (64k-1) that is the limit of the current protocol.
static uint16_t CappedAllocRecordCount(size_t alloc_record_count) {
  const size_t cap = 0xffff;
  if (alloc_record_count > cap) {
    return cap;
  }
  return alloc_record_count;
}

class Breakpoint : public ValueObject {
 public:
  Breakpoint(ArtMethod* method, uint32_t dex_pc, DeoptimizationRequest::Kind deoptimization_kind)
    : method_(method->GetCanonicalMethod(kRuntimePointerSize)),
      dex_pc_(dex_pc),
      deoptimization_kind_(deoptimization_kind) {
    CHECK(deoptimization_kind_ == DeoptimizationRequest::kNothing ||
          deoptimization_kind_ == DeoptimizationRequest::kSelectiveDeoptimization ||
          deoptimization_kind_ == DeoptimizationRequest::kFullDeoptimization);
  }

  Breakpoint(const Breakpoint& other) REQUIRES_SHARED(Locks::mutator_lock_)
    : method_(other.method_),
      dex_pc_(other.dex_pc_),
      deoptimization_kind_(other.deoptimization_kind_) {}

  // Method() is called from root visiting, do not use ScopedObjectAccess here or it can cause
  // GC to deadlock if another thread tries to call SuspendAll while the GC is in a runnable state.
  ArtMethod* Method() const {
    return method_;
  }

  uint32_t DexPc() const {
    return dex_pc_;
  }

  DeoptimizationRequest::Kind GetDeoptimizationKind() const {
    return deoptimization_kind_;
  }

  // Returns true if the method of this breakpoint and the passed in method should be considered the
  // same. That is, they are either the same method or they are copied from the same method.
  bool IsInMethod(ArtMethod* m) const REQUIRES_SHARED(Locks::mutator_lock_) {
    return method_ == m->GetCanonicalMethod(kRuntimePointerSize);
  }

 private:
  // The location of this breakpoint.
  ArtMethod* method_;
  uint32_t dex_pc_;

  // Indicates whether breakpoint needs full deoptimization or selective deoptimization.
  DeoptimizationRequest::Kind deoptimization_kind_;
};

static std::ostream& operator<<(std::ostream& os, const Breakpoint& rhs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  os << StringPrintf("Breakpoint[%s @%#x]", ArtMethod::PrettyMethod(rhs.Method()).c_str(),
                     rhs.DexPc());
  return os;
}

class DebugInstrumentationListener final : public instrumentation::InstrumentationListener {
 public:
  DebugInstrumentationListener() {}
  virtual ~DebugInstrumentationListener() {}

  void MethodEntered(Thread* thread,
                     Handle<mirror::Object> this_object,
                     ArtMethod* method,
                     uint32_t dex_pc)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    if (method->IsNative()) {
      // TODO: post location events is a suspension point and native method entry stubs aren't.
      return;
    }
    if (IsListeningToDexPcMoved()) {
      // We also listen to kDexPcMoved instrumentation event so we know the DexPcMoved method is
      // going to be called right after us. To avoid sending JDWP events twice for this location,
      // we report the event in DexPcMoved. However, we must remind this is method entry so we
      // send the METHOD_ENTRY event. And we can also group it with other events for this location
      // like BREAKPOINT or SINGLE_STEP (or even METHOD_EXIT if this is a RETURN instruction).
      thread->SetDebugMethodEntry();
    } else if (IsListeningToMethodExit() && IsReturn(method, dex_pc)) {
      // We also listen to kMethodExited instrumentation event and the current instruction is a
      // RETURN so we know the MethodExited method is going to be called right after us. To avoid
      // sending JDWP events twice for this location, we report the event(s) in MethodExited.
      // However, we must remind this is method entry so we send the METHOD_ENTRY event. And we can
      // also group it with other events for this location like BREAKPOINT or SINGLE_STEP.
      thread->SetDebugMethodEntry();
    } else {
      Dbg::UpdateDebugger(thread, this_object.Get(), method, 0, Dbg::kMethodEntry, nullptr);
    }
  }

  void MethodExited(Thread* thread,
                    Handle<mirror::Object> this_object,
                    ArtMethod* method,
                    uint32_t dex_pc,
                    const JValue& return_value)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    if (method->IsNative()) {
      // TODO: post location events is a suspension point and native method entry stubs aren't.
      return;
    }
    uint32_t events = Dbg::kMethodExit;
    if (thread->IsDebugMethodEntry()) {
      // It is also the method entry.
      DCHECK(IsReturn(method, dex_pc));
      events |= Dbg::kMethodEntry;
      thread->ClearDebugMethodEntry();
    }
    Dbg::UpdateDebugger(thread, this_object.Get(), method, dex_pc, events, &return_value);
  }

  void MethodUnwind(Thread* thread ATTRIBUTE_UNUSED,
                    Handle<mirror::Object> this_object ATTRIBUTE_UNUSED,
                    ArtMethod* method,
                    uint32_t dex_pc)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    // We're not recorded to listen to this kind of event, so complain.
    LOG(ERROR) << "Unexpected method unwind event in debugger " << ArtMethod::PrettyMethod(method)
               << " " << dex_pc;
  }

  void DexPcMoved(Thread* thread,
                  Handle<mirror::Object> this_object,
                  ArtMethod* method,
                  uint32_t new_dex_pc)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    if (IsListeningToMethodExit() && IsReturn(method, new_dex_pc)) {
      // We also listen to kMethodExited instrumentation event and the current instruction is a
      // RETURN so we know the MethodExited method is going to be called right after us. Like in
      // MethodEntered, we delegate event reporting to MethodExited.
      // Besides, if this RETURN instruction is the only one in the method, we can send multiple
      // JDWP events in the same packet: METHOD_ENTRY, METHOD_EXIT, BREAKPOINT and/or SINGLE_STEP.
      // Therefore, we must not clear the debug method entry flag here.
    } else {
      uint32_t events = 0;
      if (thread->IsDebugMethodEntry()) {
        // It is also the method entry.
        events = Dbg::kMethodEntry;
        thread->ClearDebugMethodEntry();
      }
      Dbg::UpdateDebugger(thread, this_object.Get(), method, new_dex_pc, events, nullptr);
    }
  }

  void FieldRead(Thread* thread ATTRIBUTE_UNUSED,
                 Handle<mirror::Object> this_object,
                 ArtMethod* method,
                 uint32_t dex_pc,
                 ArtField* field)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    Dbg::PostFieldAccessEvent(method, dex_pc, this_object.Get(), field);
  }

  void FieldWritten(Thread* thread ATTRIBUTE_UNUSED,
                    Handle<mirror::Object> this_object,
                    ArtMethod* method,
                    uint32_t dex_pc,
                    ArtField* field,
                    const JValue& field_value)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    Dbg::PostFieldModificationEvent(method, dex_pc, this_object.Get(), field, &field_value);
  }

  void ExceptionThrown(Thread* thread ATTRIBUTE_UNUSED,
                       Handle<mirror::Throwable> exception_object)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    Dbg::PostException(exception_object.Get());
  }

  // We only care about branches in the Jit.
  void Branch(Thread* /*thread*/, ArtMethod* method, uint32_t dex_pc, int32_t dex_pc_offset)
      override REQUIRES_SHARED(Locks::mutator_lock_) {
    LOG(ERROR) << "Unexpected branch event in debugger " << ArtMethod::PrettyMethod(method)
               << " " << dex_pc << ", " << dex_pc_offset;
  }

  // TODO Might be worth it to post ExceptionCatch event.
  void ExceptionHandled(Thread* thread ATTRIBUTE_UNUSED,
                        Handle<mirror::Throwable> throwable ATTRIBUTE_UNUSED) override {
    LOG(ERROR) << "Unexpected exception handled event in debugger";
  }

  // TODO Might be worth it to implement this.
  void WatchedFramePop(Thread* thread ATTRIBUTE_UNUSED,
                       const ShadowFrame& frame ATTRIBUTE_UNUSED) override {
    LOG(ERROR) << "Unexpected WatchedFramePop event in debugger";
  }

 private:
  static bool IsReturn(ArtMethod* method, uint32_t dex_pc) REQUIRES_SHARED(Locks::mutator_lock_) {
    return method->DexInstructions().InstructionAt(dex_pc).IsReturn();
  }

  static bool IsListeningToDexPcMoved() REQUIRES_SHARED(Locks::mutator_lock_) {
    return IsListeningTo(instrumentation::Instrumentation::kDexPcMoved);
  }

  static bool IsListeningToMethodExit() REQUIRES_SHARED(Locks::mutator_lock_) {
    return IsListeningTo(instrumentation::Instrumentation::kMethodExited);
  }

  static bool IsListeningTo(instrumentation::Instrumentation::InstrumentationEvent event)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return (Dbg::GetInstrumentationEvents() & event) != 0;
  }

  DISALLOW_COPY_AND_ASSIGN(DebugInstrumentationListener);
} gDebugInstrumentationListener;

// JDWP is allowed unless the Zygote forbids it.
static bool gJdwpAllowed = true;

// Was there a -Xrunjdwp or -agentlib:jdwp= argument on the command line?
static bool gJdwpConfigured = false;

// JDWP options for debugging. Only valid if IsJdwpConfigured() is true.
static JDWP::JdwpOptions gJdwpOptions;

// Runtime JDWP state.
static JDWP::JdwpState* gJdwpState = nullptr;
static bool gDebuggerConnected;  // debugger or DDMS is connected.

static bool gDdmThreadNotification = false;

// DDMS GC-related settings.
static Dbg::HpifWhen gDdmHpifWhen = Dbg::HPIF_WHEN_NEVER;
static Dbg::HpsgWhen gDdmHpsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmHpsgWhat;
static Dbg::HpsgWhen gDdmNhsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmNhsgWhat;

bool Dbg::gDebuggerActive = false;
bool Dbg::gDisposed = false;
ObjectRegistry* Dbg::gRegistry = nullptr;
DebuggerActiveMethodInspectionCallback Dbg::gDebugActiveCallback;
DebuggerDdmCallback Dbg::gDebugDdmCallback;
InternalDebuggerControlCallback Dbg::gDebuggerControlCallback;

// Deoptimization support.
std::vector<DeoptimizationRequest> Dbg::deoptimization_requests_;
size_t Dbg::full_deoptimization_event_count_ = 0;

// Instrumentation event reference counters.
size_t Dbg::dex_pc_change_event_ref_count_ = 0;
size_t Dbg::method_enter_event_ref_count_ = 0;
size_t Dbg::method_exit_event_ref_count_ = 0;
size_t Dbg::field_read_event_ref_count_ = 0;
size_t Dbg::field_write_event_ref_count_ = 0;
size_t Dbg::exception_catch_event_ref_count_ = 0;
uint32_t Dbg::instrumentation_events_ = 0;

Dbg::DbgThreadLifecycleCallback Dbg::thread_lifecycle_callback_;
Dbg::DbgClassLoadCallback Dbg::class_load_callback_;

void DebuggerDdmCallback::DdmPublishChunk(uint32_t type, const ArrayRef<const uint8_t>& data) {
  if (gJdwpState == nullptr) {
    VLOG(jdwp) << "Debugger thread not active, ignoring DDM send: " << type;
  } else {
    iovec vec[1];
    vec[0].iov_base = reinterpret_cast<void*>(const_cast<uint8_t*>(data.data()));
    vec[0].iov_len = data.size();
    gJdwpState->DdmSendChunkV(type, vec, 1);
  }
}

bool DebuggerActiveMethodInspectionCallback::IsMethodBeingInspected(ArtMethod* m ATTRIBUTE_UNUSED) {
  return Dbg::IsDebuggerActive();
}

bool DebuggerActiveMethodInspectionCallback::IsMethodSafeToJit(ArtMethod* m) {
  return !Dbg::MethodHasAnyBreakpoints(m);
}

bool DebuggerActiveMethodInspectionCallback::MethodNeedsDebugVersion(
    ArtMethod* m ATTRIBUTE_UNUSED) {
  return Dbg::IsDebuggerActive();
}

void InternalDebuggerControlCallback::StartDebugger() {
  // Release the mutator lock.
  ScopedThreadStateChange stsc(art::Thread::Current(), kNative);
  Dbg::StartJdwp();
}

void InternalDebuggerControlCallback::StopDebugger() {
  Dbg::StopJdwp();
}

bool InternalDebuggerControlCallback::IsDebuggerConfigured() {
  return Dbg::IsJdwpConfigured();
}

// Breakpoints.
static std::vector<Breakpoint> gBreakpoints GUARDED_BY(Locks::breakpoint_lock_);

void DebugInvokeReq::VisitRoots(RootVisitor* visitor, const RootInfo& root_info) {
  receiver.VisitRootIfNonNull(visitor, root_info);  // null for static method call.
  klass.VisitRoot(visitor, root_info);
}

void SingleStepControl::AddDexPc(uint32_t dex_pc) {
  dex_pcs_.insert(dex_pc);
}

bool SingleStepControl::ContainsDexPc(uint32_t dex_pc) const {
  return dex_pcs_.find(dex_pc) == dex_pcs_.end();
}

static bool IsBreakpoint(ArtMethod* m, uint32_t dex_pc)
    REQUIRES(!Locks::breakpoint_lock_)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ReaderMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
  for (size_t i = 0, e = gBreakpoints.size(); i < e; ++i) {
    if (gBreakpoints[i].DexPc() == dex_pc && gBreakpoints[i].IsInMethod(m)) {
      VLOG(jdwp) << "Hit breakpoint #" << i << ": " << gBreakpoints[i];
      return true;
    }
  }
  return false;
}

static bool IsSuspendedForDebugger(ScopedObjectAccessUnchecked& soa, Thread* thread)
    REQUIRES(!Locks::thread_suspend_count_lock_) {
  MutexLock mu(soa.Self(), *Locks::thread_suspend_count_lock_);
  // A thread may be suspended for GC; in this code, we really want to know whether
  // there's a debugger suspension active.
  return thread->IsSuspended() && thread->GetDebugSuspendCount() > 0;
}

static ObjPtr<mirror::Array> DecodeNonNullArray(JDWP::RefTypeId id, JDWP::JdwpError* error)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Object> o = Dbg::GetObjectRegistry()->Get<mirror::Object>(id, error);
  if (o == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
  }
  if (!o->IsArrayInstance()) {
    *error = JDWP::ERR_INVALID_ARRAY;
    return nullptr;
  }
  *error = JDWP::ERR_NONE;
  return o->AsArray();
}

static ObjPtr<mirror::Class> DecodeClass(JDWP::RefTypeId id, JDWP::JdwpError* error)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Object> o = Dbg::GetObjectRegistry()->Get<mirror::Object>(id, error);
  if (o == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
  }
  if (!o->IsClass()) {
    *error = JDWP::ERR_INVALID_CLASS;
    return nullptr;
  }
  *error = JDWP::ERR_NONE;
  return o->AsClass();
}

static Thread* DecodeThread(ScopedObjectAccessUnchecked& soa, JDWP::ObjectId thread_id,
                            JDWP::JdwpError* error)
    REQUIRES_SHARED(Locks::mutator_lock_)
    REQUIRES(!Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_) {
  ObjPtr<mirror::Object> thread_peer =
      Dbg::GetObjectRegistry()->Get<mirror::Object>(thread_id, error);
  if (thread_peer == nullptr) {
    // This isn't even an object.
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
  }

  ObjPtr<mirror::Class> java_lang_Thread =
      soa.Decode<mirror::Class>(WellKnownClasses::java_lang_Thread);
  if (!java_lang_Thread->IsAssignableFrom(thread_peer->GetClass())) {
    // This isn't a thread.
    *error = JDWP::ERR_INVALID_THREAD;
    return nullptr;
  }

  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  Thread* thread = Thread::FromManagedThread(soa, thread_peer);
  // If thread is null then this a java.lang.Thread without a Thread*. Must be a un-started or a
  // zombie.
  *error = (thread == nullptr) ? JDWP::ERR_THREAD_NOT_ALIVE : JDWP::ERR_NONE;
  return thread;
}

static JDWP::JdwpTag BasicTagFromDescriptor(const char* descriptor) {
  // JDWP deliberately uses the descriptor characters' ASCII values for its enum.
  // Note that by "basic" we mean that we don't get more specific than JT_OBJECT.
  return static_cast<JDWP::JdwpTag>(descriptor[0]);
}

static JDWP::JdwpTag BasicTagFromClass(ObjPtr<mirror::Class> klass)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string temp;
  const char* descriptor = klass->GetDescriptor(&temp);
  return BasicTagFromDescriptor(descriptor);
}

static JDWP::JdwpTag TagFromClass(const ScopedObjectAccessUnchecked& soa, ObjPtr<mirror::Class> c)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  CHECK(c != nullptr);
  if (c->IsArrayClass()) {
    return JDWP::JT_ARRAY;
  }
  if (c->IsStringClass()) {
    return JDWP::JT_STRING;
  }
  if (c->IsClassClass()) {
    return JDWP::JT_CLASS_OBJECT;
  }
  {
    ObjPtr<mirror::Class> thread_class =
        soa.Decode<mirror::Class>(WellKnownClasses::java_lang_Thread);
    if (thread_class->IsAssignableFrom(c)) {
      return JDWP::JT_THREAD;
    }
  }
  {
    ObjPtr<mirror::Class> thread_group_class =
        soa.Decode<mirror::Class>(WellKnownClasses::java_lang_ThreadGroup);
    if (thread_group_class->IsAssignableFrom(c)) {
      return JDWP::JT_THREAD_GROUP;
    }
  }
  {
    ObjPtr<mirror::Class> class_loader_class =
        soa.Decode<mirror::Class>(WellKnownClasses::java_lang_ClassLoader);
    if (class_loader_class->IsAssignableFrom(c)) {
      return JDWP::JT_CLASS_LOADER;
    }
  }
  return JDWP::JT_OBJECT;
}

/*
 * Objects declared to hold Object might actually hold a more specific
 * type.  The debugger may take a special interest in these (e.g. it
 * wants to display the contents of Strings), so we want to return an
 * appropriate tag.
 *
 * Null objects are tagged JT_OBJECT.
 */
JDWP::JdwpTag Dbg::TagFromObject(const ScopedObjectAccessUnchecked& soa, ObjPtr<mirror::Object> o) {
  return (o == nullptr) ? JDWP::JT_OBJECT : TagFromClass(soa, o->GetClass());
}

static bool IsPrimitiveTag(JDWP::JdwpTag tag) {
  switch (tag) {
  case JDWP::JT_BOOLEAN:
  case JDWP::JT_BYTE:
  case JDWP::JT_CHAR:
  case JDWP::JT_FLOAT:
  case JDWP::JT_DOUBLE:
  case JDWP::JT_INT:
  case JDWP::JT_LONG:
  case JDWP::JT_SHORT:
  case JDWP::JT_VOID:
    return true;
  default:
    return false;
  }
}

void Dbg::StartJdwp() {
  if (!gJdwpAllowed || !IsJdwpConfigured()) {
    // No JDWP for you!
    return;
  }

  CHECK(gRegistry == nullptr);
  gRegistry = new ObjectRegistry;

  {
    // Setup the Ddm listener
    ScopedObjectAccess soa(Thread::Current());
    Runtime::Current()->GetRuntimeCallbacks()->AddDdmCallback(&gDebugDdmCallback);
  }

  // Init JDWP if the debugger is enabled. This may connect out to a
  // debugger, passively listen for a debugger, or block waiting for a
  // debugger.
  gJdwpState = JDWP::JdwpState::Create(&gJdwpOptions);
  if (gJdwpState == nullptr) {
    // We probably failed because some other process has the port already, which means that
    // if we don't abort the user is likely to think they're talking to us when they're actually
    // talking to that other process.
    LOG(FATAL) << "Debugger thread failed to initialize";
  }

  // If a debugger has already attached, send the "welcome" message.
  // This may cause us to suspend all threads.
  if (gJdwpState->IsActive()) {
    ScopedObjectAccess soa(Thread::Current());
    gJdwpState->PostVMStart();
  }
}

void Dbg::StopJdwp() {
  // Post VM_DEATH event before the JDWP connection is closed (either by the JDWP thread or the
  // destruction of gJdwpState).
  if (gJdwpState != nullptr && gJdwpState->IsActive()) {
    gJdwpState->PostVMDeath();
  }
  // Prevent the JDWP thread from processing JDWP incoming packets after we close the connection.
  Dispose();
  delete gJdwpState;
  gJdwpState = nullptr;
  delete gRegistry;
  gRegistry = nullptr;
}

void Dbg::GcDidFinish() {
  if (gDdmHpifWhen != HPIF_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Sending heap info to DDM";
    DdmSendHeapInfo(gDdmHpifWhen);
  }
  if (gDdmHpsgWhen != HPSG_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Dumping heap to DDM";
    DdmSendHeapSegments(false);
  }
  if (gDdmNhsgWhen != HPSG_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Dumping native heap to DDM";
    DdmSendHeapSegments(true);
  }
}

void Dbg::SetJdwpAllowed(bool allowed) {
  gJdwpAllowed = allowed;
}

bool Dbg::IsJdwpAllowed() {
  return gJdwpAllowed;
}

DebugInvokeReq* Dbg::GetInvokeReq() {
  return Thread::Current()->GetInvokeReq();
}

Thread* Dbg::GetDebugThread() {
  return (gJdwpState != nullptr) ? gJdwpState->GetDebugThread() : nullptr;
}

void Dbg::ClearWaitForEventThread() {
  gJdwpState->ReleaseJdwpTokenForEvent();
}

void Dbg::Connected() {
  CHECK(!gDebuggerConnected);
  VLOG(jdwp) << "JDWP has attached";
  gDebuggerConnected = true;
  gDisposed = false;
}

bool Dbg::RequiresDeoptimization() {
  // We don't need deoptimization if everything runs with interpreter after
  // enabling -Xint mode.
  return !Runtime::Current()->GetInstrumentation()->IsForcedInterpretOnly();
}

void Dbg::GoActive() {
  // Enable all debugging features, including scans for breakpoints.
  // This is a no-op if we're already active.
  // Only called from the JDWP handler thread.
  if (IsDebuggerActive()) {
    return;
  }

  Thread* const self = Thread::Current();
  {
    // TODO: dalvik only warned if there were breakpoints left over. clear in Dbg::Disconnected?
    ReaderMutexLock mu(self, *Locks::breakpoint_lock_);
    CHECK_EQ(gBreakpoints.size(), 0U);
  }

  {
    MutexLock mu(self, *Locks::deoptimization_lock_);
    CHECK_EQ(deoptimization_requests_.size(), 0U);
    CHECK_EQ(full_deoptimization_event_count_, 0U);
    CHECK_EQ(dex_pc_change_event_ref_count_, 0U);
    CHECK_EQ(method_enter_event_ref_count_, 0U);
    CHECK_EQ(method_exit_event_ref_count_, 0U);
    CHECK_EQ(field_read_event_ref_count_, 0U);
    CHECK_EQ(field_write_event_ref_count_, 0U);
    CHECK_EQ(exception_catch_event_ref_count_, 0U);
  }

  Runtime* runtime = Runtime::Current();
  // Best effort deoptimization if the runtime is non-Java debuggable. This happens when
  // ro.debuggable is set, but the application is not debuggable, or when a standalone
  // dalvikvm invocation is not passed the debuggable option (-Xcompiler-option --debuggable).
  //
  // The performance cost of this is non-negligible during native-debugging due to the
  // forced JIT, so we keep the AOT code in that case in exchange for limited native debugging.
  ScopedSuspendAll ssa(__FUNCTION__);
  if (!runtime->IsJavaDebuggable() &&
      !runtime->GetInstrumentation()->IsForcedInterpretOnly() &&
      !runtime->IsNativeDebuggable()) {
    runtime->DeoptimizeBootImage();
  }

  if (RequiresDeoptimization()) {
    runtime->GetInstrumentation()->EnableDeoptimization();
  }
  instrumentation_events_ = 0;
  Runtime::DoAndMaybeSwitchInterpreter([=](){ gDebuggerActive = true; });
  runtime->GetRuntimeCallbacks()->AddClassLoadCallback(Dbg::GetClassLoadCallback());
  runtime->GetRuntimeCallbacks()->AddMethodInspectionCallback(&gDebugActiveCallback);
  LOG(INFO) << "Debugger is active";
}

void Dbg::Disconnected() {
  CHECK(gDebuggerConnected);

  LOG(INFO) << "Debugger is no longer active";

  // Suspend all threads and exclusively acquire the mutator lock. Remove the debugger as a listener
  // and clear the object registry.
  Runtime* runtime = Runtime::Current();
  Thread* self = Thread::Current();
  {
    // Required for DisableDeoptimization.
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa(__FUNCTION__);
    // Debugger may not be active at this point.
    if (IsDebuggerActive()) {
      {
        // Since we're going to disable deoptimization, we clear the deoptimization requests queue.
        // This prevents us from having any pending deoptimization request when the debugger attaches
        // to us again while no event has been requested yet.
        MutexLock mu(self, *Locks::deoptimization_lock_);
        deoptimization_requests_.clear();
        full_deoptimization_event_count_ = 0U;
      }
      if (instrumentation_events_ != 0) {
        runtime->GetInstrumentation()->RemoveListener(&gDebugInstrumentationListener,
                                                      instrumentation_events_);
        instrumentation_events_ = 0;
      }
      if (RequiresDeoptimization()) {
        runtime->GetInstrumentation()->DisableDeoptimization(kDbgInstrumentationKey);
      }
      Runtime::DoAndMaybeSwitchInterpreter([=](){ gDebuggerActive = false; });
      runtime->GetRuntimeCallbacks()->RemoveClassLoadCallback(Dbg::GetClassLoadCallback());
      runtime->GetRuntimeCallbacks()->RemoveMethodInspectionCallback(
          &gDebugActiveCallback);
    }
  }

  {
    ScopedObjectAccess soa(self);
    gRegistry->Clear();
  }

  gDebuggerConnected = false;
}

void Dbg::ConfigureJdwp(const JDWP::JdwpOptions& jdwp_options) {
  CHECK_NE(jdwp_options.transport, JDWP::kJdwpTransportUnknown);
  gJdwpOptions = jdwp_options;
  gJdwpConfigured = true;
  Runtime::Current()->GetRuntimeCallbacks()->AddDebuggerControlCallback(&gDebuggerControlCallback);
}

bool Dbg::IsJdwpConfigured() {
  return gJdwpConfigured;
}

int64_t Dbg::LastDebuggerActivity() {
  return gJdwpState->LastDebuggerActivity();
}

void Dbg::UndoDebuggerSuspensions() {
  Runtime::Current()->GetThreadList()->UndoDebuggerSuspensions();
}

std::string Dbg::GetClassName(JDWP::RefTypeId class_id) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> o = gRegistry->Get<mirror::Object>(class_id, &error);
  if (o == nullptr) {
    if (error == JDWP::ERR_NONE) {
      return "null";
    } else {
      return StringPrintf("invalid object %p", reinterpret_cast<void*>(class_id));
    }
  }
  if (!o->IsClass()) {
    return StringPrintf("non-class %p", o.Ptr());  // This is only used for debugging output anyway.
  }
  return GetClassName(o->AsClass());
}

std::string Dbg::GetClassName(ObjPtr<mirror::Class> klass) {
  if (klass == nullptr) {
    return "null";
  }
  std::string temp;
  return DescriptorToName(klass->GetDescriptor(&temp));
}

JDWP::JdwpError Dbg::GetClassObject(JDWP::RefTypeId id, JDWP::ObjectId* class_object_id) {
  JDWP::JdwpError status;
  ObjPtr<mirror::Class> c = DecodeClass(id, &status);
  if (c == nullptr) {
    *class_object_id = 0;
    return status;
  }
  *class_object_id = gRegistry->Add(c);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSuperclass(JDWP::RefTypeId id, JDWP::RefTypeId* superclass_id) {
  JDWP::JdwpError status;
  ObjPtr<mirror::Class> c = DecodeClass(id, &status);
  if (c == nullptr) {
    *superclass_id = 0;
    return status;
  }
  if (c->IsInterface()) {
    // http://code.google.com/p/android/issues/detail?id=20856
    *superclass_id = 0;
  } else {
    *superclass_id = gRegistry->Add(c->GetSuperClass());
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetClassLoader(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(id, &error);
  if (c == nullptr) {
    return error;
  }
  expandBufAddObjectId(pReply, gRegistry->Add(c->GetClassLoader()));
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetModifiers(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(id, &error);
  if (c == nullptr) {
    return error;
  }

  uint32_t access_flags = c->GetAccessFlags() & kAccJavaFlagsMask;

  // Set ACC_SUPER. Dex files don't contain this flag but only classes are supposed to have it set,
  // not interfaces.
  // Class.getModifiers doesn't return it, but JDWP does, so we set it here.
  if ((access_flags & kAccInterface) == 0) {
    access_flags |= kAccSuper;
  }

  expandBufAdd4BE(pReply, access_flags);

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetMonitorInfo(JDWP::ObjectId object_id, JDWP::ExpandBuf* reply) {
  JDWP::JdwpError error;
  Thread* self = Thread::Current();
  StackHandleScope<1u> hs(self);
  Handle<mirror::Object> o = hs.NewHandle(gRegistry->Get<mirror::Object>(object_id, &error));
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }

  // Ensure all threads are suspended while we read objects' lock words.
  CHECK_EQ(self->GetState(), kRunnable);

  MonitorInfo monitor_info;
  {
    ScopedThreadSuspension sts(self, kSuspended);
    ScopedSuspendAll ssa(__FUNCTION__);
    monitor_info = MonitorInfo(o.Get());
  }
  if (monitor_info.owner_ != nullptr) {
    expandBufAddObjectId(reply, gRegistry->Add(monitor_info.owner_->GetPeerFromOtherThread()));
  } else {
    expandBufAddObjectId(reply, gRegistry->Add(nullptr));
  }
  expandBufAdd4BE(reply, monitor_info.entry_count_);
  expandBufAdd4BE(reply, monitor_info.waiters_.size());
  for (size_t i = 0; i < monitor_info.waiters_.size(); ++i) {
    expandBufAddObjectId(reply, gRegistry->Add(monitor_info.waiters_[i]->GetPeerFromOtherThread()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetOwnedMonitors(JDWP::ObjectId thread_id,
                                      std::vector<JDWP::ObjectId>* monitors,
                                      std::vector<uint32_t>* stack_depths) {
  struct OwnedMonitorVisitor : public StackVisitor {
    OwnedMonitorVisitor(Thread* thread, Context* context,
                        std::vector<JDWP::ObjectId>* monitor_vector,
                        std::vector<uint32_t>* stack_depth_vector)
        REQUIRES_SHARED(Locks::mutator_lock_)
      : StackVisitor(thread, context, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        current_stack_depth(0),
        monitors(monitor_vector),
        stack_depths(stack_depth_vector) {}

    // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
    // annotalysis.
    bool VisitFrame() override NO_THREAD_SAFETY_ANALYSIS {
      if (!GetMethod()->IsRuntimeMethod()) {
        Monitor::VisitLocks(this, AppendOwnedMonitors, this);
        ++current_stack_depth;
      }
      return true;
    }

    static void AppendOwnedMonitors(ObjPtr<mirror::Object> owned_monitor, void* arg)
        REQUIRES_SHARED(Locks::mutator_lock_) {
      OwnedMonitorVisitor* visitor = reinterpret_cast<OwnedMonitorVisitor*>(arg);
      visitor->monitors->push_back(gRegistry->Add(owned_monitor));
      visitor->stack_depths->push_back(visitor->current_stack_depth);
    }

    size_t current_stack_depth;
    std::vector<JDWP::ObjectId>* const monitors;
    std::vector<uint32_t>* const stack_depths;
  };

  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (thread == nullptr) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }
  std::unique_ptr<Context> context(Context::Create());
  OwnedMonitorVisitor visitor(thread, context.get(), monitors, stack_depths);
  visitor.WalkStack();
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetContendedMonitor(JDWP::ObjectId thread_id,
                                         JDWP::ObjectId* contended_monitor) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  *contended_monitor = 0;
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (thread == nullptr) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }
  ObjPtr<mirror::Object> contended_monitor_obj = Monitor::GetContendedMonitor(thread);
  // Add() requires the thread_list_lock_ not held to avoid the lock
  // level violation.
  *contended_monitor = gRegistry->Add(contended_monitor_obj);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetInstanceCounts(const std::vector<JDWP::RefTypeId>& class_ids,
                                       std::vector<uint64_t>* counts) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->CollectGarbage(/* clear_soft_references= */ false, gc::GcCause::kGcCauseDebugger);
  VariableSizedHandleScope hs(Thread::Current());
  std::vector<Handle<mirror::Class>> classes;
  counts->clear();
  for (size_t i = 0; i < class_ids.size(); ++i) {
    JDWP::JdwpError error;
    ObjPtr<mirror::Class> c = DecodeClass(class_ids[i], &error);
    if (c == nullptr) {
      return error;
    }
    classes.push_back(hs.NewHandle(c));
    counts->push_back(0);
  }
  heap->CountInstances(classes, false, &(*counts)[0]);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetInstances(JDWP::RefTypeId class_id, int32_t max_count,
                                  std::vector<JDWP::ObjectId>* instances) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // We only want reachable instances, so do a GC.
  heap->CollectGarbage(/* clear_soft_references= */ false, gc::GcCause::kGcCauseDebugger);
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  VariableSizedHandleScope hs(Thread::Current());
  std::vector<Handle<mirror::Object>> raw_instances;
  Runtime::Current()->GetHeap()->GetInstances(hs,
                                              hs.NewHandle(c),
                                              /* use_is_assignable_from= */ false,
                                              max_count,
                                              raw_instances);
  for (size_t i = 0; i < raw_instances.size(); ++i) {
    instances->push_back(gRegistry->Add(raw_instances[i].Get()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetReferringObjects(JDWP::ObjectId object_id, int32_t max_count,
                                         std::vector<JDWP::ObjectId>* referring_objects) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->CollectGarbage(/* clear_soft_references= */ false, gc::GcCause::kGcCauseDebugger);
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> o = gRegistry->Get<mirror::Object>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  VariableSizedHandleScope hs(Thread::Current());
  std::vector<Handle<mirror::Object>> raw_instances;
  heap->GetReferringObjects(hs, hs.NewHandle(o), max_count, raw_instances);
  for (size_t i = 0; i < raw_instances.size(); ++i) {
    referring_objects->push_back(gRegistry->Add(raw_instances[i].Get()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::DisableCollection(JDWP::ObjectId object_id) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> o = gRegistry->Get<mirror::Object>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  gRegistry->DisableCollection(object_id);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::EnableCollection(JDWP::ObjectId object_id) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> o = gRegistry->Get<mirror::Object>(object_id, &error);
  // Unlike DisableCollection, JDWP specs do not state an invalid object causes an error. The RI
  // also ignores these cases and never return an error. However it's not obvious why this command
  // should behave differently from DisableCollection and IsCollected commands. So let's be more
  // strict and return an error if this happens.
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  gRegistry->EnableCollection(object_id);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::IsCollected(JDWP::ObjectId object_id, bool* is_collected) {
  *is_collected = true;
  if (object_id == 0) {
    // Null object id is invalid.
    return JDWP::ERR_INVALID_OBJECT;
  }
  // JDWP specs state an INVALID_OBJECT error is returned if the object ID is not valid. However
  // the RI seems to ignore this and assume object has been collected.
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> o = gRegistry->Get<mirror::Object>(object_id, &error);
  if (o != nullptr) {
    *is_collected = gRegistry->IsCollected(object_id);
  }
  return JDWP::ERR_NONE;
}

void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count) {
  gRegistry->DisposeObject(object_id, reference_count);
}

JDWP::JdwpTypeTag Dbg::GetTypeTag(ObjPtr<mirror::Class> klass) {
  DCHECK(klass != nullptr);
  if (klass->IsArrayClass()) {
    return JDWP::TT_ARRAY;
  } else if (klass->IsInterface()) {
    return JDWP::TT_INTERFACE;
  } else {
    return JDWP::TT_CLASS;
  }
}

JDWP::JdwpError Dbg::GetReflectedType(JDWP::RefTypeId class_id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }

  JDWP::JdwpTypeTag type_tag = GetTypeTag(c);
  expandBufAdd1(pReply, type_tag);
  expandBufAddRefTypeId(pReply, class_id);
  return JDWP::ERR_NONE;
}

// Get the complete list of reference classes (i.e. all classes except
// the primitive types).
// Returns a newly-allocated buffer full of RefTypeId values.
class ClassListCreator : public ClassVisitor {
 public:
  explicit ClassListCreator(std::vector<JDWP::RefTypeId>* classes) : classes_(classes) {}

  bool operator()(ObjPtr<mirror::Class> c) override REQUIRES_SHARED(Locks::mutator_lock_) {
    if (!c->IsPrimitive()) {
      classes_->push_back(Dbg::GetObjectRegistry()->AddRefType(c));
    }
    return true;
  }

 private:
  std::vector<JDWP::RefTypeId>* const classes_;
};

void Dbg::GetClassList(std::vector<JDWP::RefTypeId>* classes) {
  ClassListCreator clc(classes);
  Runtime::Current()->GetClassLinker()->VisitClassesWithoutClassesLock(&clc);
}

JDWP::JdwpError Dbg::GetClassInfo(JDWP::RefTypeId class_id, JDWP::JdwpTypeTag* pTypeTag,
                                  uint32_t* pStatus, std::string* pDescriptor) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }

  if (c->IsArrayClass()) {
    *pStatus = JDWP::CS_VERIFIED | JDWP::CS_PREPARED;
    *pTypeTag = JDWP::TT_ARRAY;
  } else {
    if (c->IsErroneous()) {
      *pStatus = JDWP::CS_ERROR;
    } else {
      *pStatus = JDWP::CS_VERIFIED | JDWP::CS_PREPARED | JDWP::CS_INITIALIZED;
    }
    *pTypeTag = c->IsInterface() ? JDWP::TT_INTERFACE : JDWP::TT_CLASS;
  }

  if (pDescriptor != nullptr) {
    std::string temp;
    *pDescriptor = c->GetDescriptor(&temp);
  }
  return JDWP::ERR_NONE;
}

void Dbg::FindLoadedClassBySignature(const char* descriptor, std::vector<JDWP::RefTypeId>* ids) {
  std::vector<ObjPtr<mirror::Class>> classes;
  Runtime::Current()->GetClassLinker()->LookupClasses(descriptor, classes);
  ids->clear();
  for (ObjPtr<mirror::Class> c : classes) {
    ids->push_back(gRegistry->Add(c));
  }
}

JDWP::JdwpError Dbg::GetReferenceType(JDWP::ObjectId object_id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> o = gRegistry->Get<mirror::Object>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }

  JDWP::JdwpTypeTag type_tag = GetTypeTag(o->GetClass());
  JDWP::RefTypeId type_id = gRegistry->AddRefType(o->GetClass());

  expandBufAdd1(pReply, type_tag);
  expandBufAddRefTypeId(pReply, type_id);

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSignature(JDWP::RefTypeId class_id, std::string* signature) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  std::string temp;
  *signature = c->GetDescriptor(&temp);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSourceDebugExtension(JDWP::RefTypeId class_id,
                                             std::string* extension_data) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> klass(hs.NewHandle(c));
  const char* data = annotations::GetSourceDebugExtension(klass);
  if (data == nullptr) {
    return JDWP::ERR_ABSENT_INFORMATION;
  }
  *extension_data = data;
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSourceFile(JDWP::RefTypeId class_id, std::string* result) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  const char* source_file = c->GetSourceFile();
  if (source_file == nullptr) {
    return JDWP::ERR_ABSENT_INFORMATION;
  }
  *result = source_file;
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetObjectTag(JDWP::ObjectId object_id, uint8_t* tag) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> o = gRegistry->Get<mirror::Object>(object_id, &error);
  if (error != JDWP::ERR_NONE) {
    *tag = JDWP::JT_VOID;
    return error;
  }
  *tag = TagFromObject(soa, o);
  return JDWP::ERR_NONE;
}

size_t Dbg::GetTagWidth(JDWP::JdwpTag tag) {
  switch (tag) {
  case JDWP::JT_VOID:
    return 0;
  case JDWP::JT_BYTE:
  case JDWP::JT_BOOLEAN:
    return 1;
  case JDWP::JT_CHAR:
  case JDWP::JT_SHORT:
    return 2;
  case JDWP::JT_FLOAT:
  case JDWP::JT_INT:
    return 4;
  case JDWP::JT_ARRAY:
  case JDWP::JT_OBJECT:
  case JDWP::JT_STRING:
  case JDWP::JT_THREAD:
  case JDWP::JT_THREAD_GROUP:
  case JDWP::JT_CLASS_LOADER:
  case JDWP::JT_CLASS_OBJECT:
    return sizeof(JDWP::ObjectId);
  case JDWP::JT_DOUBLE:
  case JDWP::JT_LONG:
    return 8;
  default:
    LOG(FATAL) << "Unknown tag " << tag;
    UNREACHABLE();
  }
}

JDWP::JdwpError Dbg::GetArrayLength(JDWP::ObjectId array_id, int32_t* length) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Array> a = DecodeNonNullArray(array_id, &error);
  if (a == nullptr) {
    return error;
  }
  *length = a->GetLength();
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputArray(JDWP::ObjectId array_id,
                                 int offset,
                                 int count,
                                 JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Array> a = DecodeNonNullArray(array_id, &error);
  if (a == nullptr) {
    return error;
  }

  if (offset < 0 || count < 0 || offset > a->GetLength() || a->GetLength() - offset < count) {
    LOG(WARNING) << __FUNCTION__ << " access out of bounds: offset=" << offset << "; count=" << count;
    return JDWP::ERR_INVALID_LENGTH;
  }
  JDWP::JdwpTag element_tag = BasicTagFromClass(a->GetClass()->GetComponentType());
  expandBufAdd1(pReply, element_tag);
  expandBufAdd4BE(pReply, count);

  if (IsPrimitiveTag(element_tag)) {
    size_t width = GetTagWidth(element_tag);
    uint8_t* dst = expandBufAddSpace(pReply, count * width);
    if (width == 8) {
      const uint64_t* src8 = reinterpret_cast<uint64_t*>(a->GetRawData(sizeof(uint64_t), 0));
      for (int i = 0; i < count; ++i) JDWP::Write8BE(&dst, src8[offset + i]);
    } else if (width == 4) {
      const uint32_t* src4 = reinterpret_cast<uint32_t*>(a->GetRawData(sizeof(uint32_t), 0));
      for (int i = 0; i < count; ++i) JDWP::Write4BE(&dst, src4[offset + i]);
    } else if (width == 2) {
      const uint16_t* src2 = reinterpret_cast<uint16_t*>(a->GetRawData(sizeof(uint16_t), 0));
      for (int i = 0; i < count; ++i) JDWP::Write2BE(&dst, src2[offset + i]);
    } else {
      const uint8_t* src = reinterpret_cast<uint8_t*>(a->GetRawData(sizeof(uint8_t), 0));
      memcpy(dst, &src[offset * width], count * width);
    }
  } else {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    ObjPtr<mirror::ObjectArray<mirror::Object>> oa = a->AsObjectArray<mirror::Object>();
    for (int i = 0; i < count; ++i) {
      ObjPtr<mirror::Object> element = oa->Get(offset + i);
      JDWP::JdwpTag specific_tag = (element != nullptr) ? TagFromObject(soa, element)
                                                        : element_tag;
      expandBufAdd1(pReply, specific_tag);
      expandBufAddObjectId(pReply, gRegistry->Add(element));
    }
  }

  return JDWP::ERR_NONE;
}

template <typename T>
static void CopyArrayData(ObjPtr<mirror::Array> a, JDWP::Request* src, int offset, int count)
    NO_THREAD_SAFETY_ANALYSIS {
  // TODO: fix when annotalysis correctly handles non-member functions.
  DCHECK(a->GetClass()->IsPrimitiveArray());

  T* dst = reinterpret_cast<T*>(a->GetRawData(sizeof(T), offset));
  for (int i = 0; i < count; ++i) {
    *dst++ = src->ReadValue(sizeof(T));
  }
}

JDWP::JdwpError Dbg::SetArrayElements(JDWP::ObjectId array_id, int offset, int count,
                                      JDWP::Request* request) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Array> dst = DecodeNonNullArray(array_id, &error);
  if (dst == nullptr) {
    return error;
  }

  if (offset < 0 || count < 0 || offset > dst->GetLength() || dst->GetLength() - offset < count) {
    LOG(WARNING) << __FUNCTION__ << " access out of bounds: offset=" << offset << "; count=" << count;
    return JDWP::ERR_INVALID_LENGTH;
  }
  JDWP::JdwpTag element_tag = BasicTagFromClass(dst->GetClass()->GetComponentType());

  if (IsPrimitiveTag(element_tag)) {
    size_t width = GetTagWidth(element_tag);
    if (width == 8) {
      CopyArrayData<uint64_t>(dst, request, offset, count);
    } else if (width == 4) {
      CopyArrayData<uint32_t>(dst, request, offset, count);
    } else if (width == 2) {
      CopyArrayData<uint16_t>(dst, request, offset, count);
    } else {
      CopyArrayData<uint8_t>(dst, request, offset, count);
    }
  } else {
    ObjPtr<mirror::ObjectArray<mirror::Object>> oa = dst->AsObjectArray<mirror::Object>();
    for (int i = 0; i < count; ++i) {
      JDWP::ObjectId id = request->ReadObjectId();
      ObjPtr<mirror::Object> o = gRegistry->Get<mirror::Object>(id, &error);
      if (error != JDWP::ERR_NONE) {
        return error;
      }
      // Check if the object's type is compatible with the array's type.
      if (o != nullptr && !o->InstanceOf(oa->GetClass()->GetComponentType())) {
        return JDWP::ERR_TYPE_MISMATCH;
      }
      oa->Set<false>(offset + i, o);
    }
  }

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::CreateString(const std::string& str, JDWP::ObjectId* new_string_id) {
  Thread* self = Thread::Current();
  ObjPtr<mirror::String> new_string = mirror::String::AllocFromModifiedUtf8(self, str.c_str());
  if (new_string == nullptr) {
    DCHECK(self->IsExceptionPending());
    self->ClearException();
    LOG(ERROR) << "Could not allocate string";
    *new_string_id = 0;
    return JDWP::ERR_OUT_OF_MEMORY;
  }
  *new_string_id = gRegistry->Add(new_string);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::CreateObject(JDWP::RefTypeId class_id, JDWP::ObjectId* new_object_id) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    *new_object_id = 0;
    return error;
  }
  Thread* self = Thread::Current();
  ObjPtr<mirror::Object> new_object;
  if (c->IsStringClass()) {
    // Special case for java.lang.String.
    gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
    new_object = mirror::String::AllocEmptyString(self, allocator_type);
  } else {
    new_object = c->AllocObject(self);
  }
  if (new_object == nullptr) {
    DCHECK(self->IsExceptionPending());
    self->ClearException();
    LOG(ERROR) << "Could not allocate object of type " << mirror::Class::PrettyDescriptor(c);
    *new_object_id = 0;
    return JDWP::ERR_OUT_OF_MEMORY;
  }
  *new_object_id = gRegistry->Add(new_object);
  return JDWP::ERR_NONE;
}

/*
 * Used by Eclipse's "Display" view to evaluate "new byte[5]" to get "(byte[]) [0, 0, 0, 0, 0]".
 */
JDWP::JdwpError Dbg::CreateArrayObject(JDWP::RefTypeId array_class_id, uint32_t length,
                                       JDWP::ObjectId* new_array_id) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(array_class_id, &error);
  if (c == nullptr) {
    *new_array_id = 0;
    return error;
  }
  Thread* self = Thread::Current();
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  ObjPtr<mirror::Array> new_array =
      mirror::Array::Alloc(self, c, length, c->GetComponentSizeShift(), allocator_type);
  if (new_array == nullptr) {
    DCHECK(self->IsExceptionPending());
    self->ClearException();
    LOG(ERROR) << "Could not allocate array of type " << mirror::Class::PrettyDescriptor(c);
    *new_array_id = 0;
    return JDWP::ERR_OUT_OF_MEMORY;
  }
  *new_array_id = gRegistry->Add(new_array);
  return JDWP::ERR_NONE;
}

JDWP::FieldId Dbg::ToFieldId(const ArtField* f) {
  return static_cast<JDWP::FieldId>(reinterpret_cast<uintptr_t>(f));
}

static JDWP::MethodId ToMethodId(ArtMethod* m)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return static_cast<JDWP::MethodId>(
      reinterpret_cast<uintptr_t>(m->GetCanonicalMethod(kRuntimePointerSize)));
}

static ArtField* FromFieldId(JDWP::FieldId fid)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return reinterpret_cast<ArtField*>(static_cast<uintptr_t>(fid));
}

static ArtMethod* FromMethodId(JDWP::MethodId mid)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return reinterpret_cast<ArtMethod*>(static_cast<uintptr_t>(mid));
}

bool Dbg::MatchThread(JDWP::ObjectId expected_thread_id, Thread* event_thread) {
  CHECK(event_thread != nullptr);
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> expected_thread_peer = gRegistry->Get<mirror::Object>(
      expected_thread_id, &error);
  return expected_thread_peer == event_thread->GetPeerFromOtherThread();
}

bool Dbg::MatchLocation(const JDWP::JdwpLocation& expected_location,
                        const JDWP::EventLocation& event_location) {
  if (expected_location.dex_pc != event_location.dex_pc) {
    return false;
  }
  ArtMethod* m = FromMethodId(expected_location.method_id);
  return m == event_location.method;
}

bool Dbg::MatchType(ObjPtr<mirror::Class> event_class, JDWP::RefTypeId class_id) {
  if (event_class == nullptr) {
    return false;
  }
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> expected_class = DecodeClass(class_id, &error);
  CHECK(expected_class != nullptr);
  return expected_class->IsAssignableFrom(event_class);
}

bool Dbg::MatchField(JDWP::RefTypeId expected_type_id,
                     JDWP::FieldId expected_field_id,
                     ArtField* event_field) {
  ArtField* expected_field = FromFieldId(expected_field_id);
  if (expected_field != event_field) {
    return false;
  }
  return Dbg::MatchType(event_field->GetDeclaringClass(), expected_type_id);
}

bool Dbg::MatchInstance(JDWP::ObjectId expected_instance_id,
                        ObjPtr<mirror::Object> event_instance) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> modifier_instance =
      gRegistry->Get<mirror::Object>(expected_instance_id, &error);
  return modifier_instance == event_instance;
}

void Dbg::SetJdwpLocation(JDWP::JdwpLocation* location, ArtMethod* m, uint32_t dex_pc) {
  if (m == nullptr) {
    memset(location, 0, sizeof(*location));
  } else {
    ObjPtr<mirror::Class> c = m->GetDeclaringClass();
    location->type_tag = GetTypeTag(c);
    location->class_id = gRegistry->AddRefType(c);
    // The RI Seems to return 0 for all obsolete methods. For compatibility we shall do the same.
    location->method_id = m->IsObsolete() ? 0 : ToMethodId(m);
    location->dex_pc = (m->IsNative() || m->IsProxyMethod()) ? static_cast<uint64_t>(-1) : dex_pc;
  }
}

std::string Dbg::GetMethodName(JDWP::MethodId method_id) {
  ArtMethod* m = FromMethodId(method_id);
  if (m == nullptr) {
    return "null";
  }
  return m->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetName();
}

bool Dbg::IsMethodObsolete(JDWP::MethodId method_id) {
  ArtMethod* m = FromMethodId(method_id);
  if (m == nullptr) {
    // NB Since we return 0 as MID for obsolete methods we want to default to true here.
    return true;
  }
  return m->IsObsolete();
}

std::string Dbg::GetFieldName(JDWP::FieldId field_id) {
  ArtField* f = FromFieldId(field_id);
  if (f == nullptr) {
    return "null";
  }
  return f->GetName();
}

/*
 * Augment the access flags for synthetic methods and fields by setting
 * the (as described by the spec) "0xf0000000 bit".  Also, strip out any
 * flags not specified by the Java programming language.
 */
static uint32_t MangleAccessFlags(uint32_t accessFlags) {
  accessFlags &= kAccJavaFlagsMask;
  if ((accessFlags & kAccSynthetic) != 0) {
    accessFlags |= 0xf0000000;
  }
  return accessFlags;
}

/*
 * Circularly shifts registers so that arguments come first. Debuggers
 * expect slots to begin with arguments, but dex code places them at
 * the end.
 */
static uint16_t MangleSlot(uint16_t slot, ArtMethod* m)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  CodeItemDataAccessor accessor(m->DexInstructionData());
  if (!accessor.HasCodeItem()) {
    // We should not get here for a method without code (native, proxy or abstract). Log it and
    // return the slot as is since all registers are arguments.
    LOG(WARNING) << "Trying to mangle slot for method without code " << m->PrettyMethod();
    return slot;
  }
  uint16_t ins_size = accessor.InsSize();
  uint16_t locals_size = accessor.RegistersSize() - ins_size;
  if (slot >= locals_size) {
    return slot - locals_size;
  } else {
    return slot + ins_size;
  }
}

static size_t GetMethodNumArgRegistersIncludingThis(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  uint32_t num_registers = ArtMethod::NumArgRegisters(method->GetShorty());
  if (!method->IsStatic()) {
    ++num_registers;
  }
  return num_registers;
}

/*
 * Circularly shifts registers so that arguments come last. Reverts
 * slots to dex style argument placement.
 */
static uint16_t DemangleSlot(uint16_t slot, ArtMethod* m, JDWP::JdwpError* error)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  CodeItemDataAccessor accessor(m->DexInstructionData());
  if (!accessor.HasCodeItem()) {
    // We should not get here for a method without code (native, proxy or abstract). Log it and
    // return the slot as is since all registers are arguments.
    LOG(WARNING) << "Trying to demangle slot for method without code "
                 << m->PrettyMethod();
    uint16_t vreg_count = GetMethodNumArgRegistersIncludingThis(m);
    if (slot < vreg_count) {
      *error = JDWP::ERR_NONE;
      return slot;
    }
  } else {
    if (slot < accessor.RegistersSize()) {
      uint16_t ins_size = accessor.InsSize();
      uint16_t locals_size = accessor.RegistersSize() - ins_size;
      *error = JDWP::ERR_NONE;
      return (slot < ins_size) ? slot + locals_size : slot - ins_size;
    }
  }

  // Slot is invalid in the method.
  LOG(ERROR) << "Invalid local slot " << slot << " for method " << m->PrettyMethod();
  *error = JDWP::ERR_INVALID_SLOT;
  return DexFile::kDexNoIndex16;
}

JDWP::JdwpError Dbg::OutputDeclaredFields(JDWP::RefTypeId class_id, bool with_generic,
                                          JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }

  size_t instance_field_count = c->NumInstanceFields();
  size_t static_field_count = c->NumStaticFields();

  expandBufAdd4BE(pReply, instance_field_count + static_field_count);

  for (size_t i = 0; i < instance_field_count + static_field_count; ++i) {
    ArtField* f = (i < instance_field_count) ? c->GetInstanceField(i) :
        c->GetStaticField(i - instance_field_count);
    expandBufAddFieldId(pReply, ToFieldId(f));
    expandBufAddUtf8String(pReply, f->GetName());
    expandBufAddUtf8String(pReply, f->GetTypeDescriptor());
    if (with_generic) {
      static const char genericSignature[1] = "";
      expandBufAddUtf8String(pReply, genericSignature);
    }
    expandBufAdd4BE(pReply, MangleAccessFlags(f->GetAccessFlags()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputDeclaredMethods(JDWP::RefTypeId class_id, bool with_generic,
                                           JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }

  expandBufAdd4BE(pReply, c->NumMethods());

  auto* cl = Runtime::Current()->GetClassLinker();
  auto ptr_size = cl->GetImagePointerSize();
  for (ArtMethod& m : c->GetMethods(ptr_size)) {
    expandBufAddMethodId(pReply, ToMethodId(&m));
    expandBufAddUtf8String(pReply, m.GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetName());
    expandBufAddUtf8String(
        pReply, m.GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetSignature().ToString());
    if (with_generic) {
      const char* generic_signature = "";
      expandBufAddUtf8String(pReply, generic_signature);
    }
    expandBufAdd4BE(pReply, MangleAccessFlags(m.GetAccessFlags()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputDeclaredInterfaces(JDWP::RefTypeId class_id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  Thread* self = Thread::Current();
  ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  size_t interface_count = c->NumDirectInterfaces();
  expandBufAdd4BE(pReply, interface_count);
  for (size_t i = 0; i < interface_count; ++i) {
    ObjPtr<mirror::Class> interface = mirror::Class::GetDirectInterface(self, c, i);
    DCHECK(interface != nullptr);
    expandBufAddRefTypeId(pReply, gRegistry->AddRefType(interface));
  }
  return JDWP::ERR_NONE;
}

void Dbg::OutputLineTable(JDWP::RefTypeId, JDWP::MethodId method_id, JDWP::ExpandBuf* pReply) {
  ArtMethod* m = FromMethodId(method_id);
  CodeItemDebugInfoAccessor accessor(m->DexInstructionDebugInfo());
  uint64_t start, end;
  if (!accessor.HasCodeItem()) {
    DCHECK(m->IsNative() || m->IsProxyMethod());
    start = -1;
    end = -1;
  } else {
    start = 0;
    // Return the index of the last instruction
    end = accessor.InsnsSizeInCodeUnits() - 1;
  }

  expandBufAdd8BE(pReply, start);
  expandBufAdd8BE(pReply, end);

  // Add numLines later
  size_t numLinesOffset = expandBufGetLength(pReply);
  expandBufAdd4BE(pReply, 0);

  int numItems = 0;
  accessor.DecodeDebugPositionInfo([&](const DexFile::PositionInfo& entry) {
    expandBufAdd8BE(pReply, entry.address_);
    expandBufAdd4BE(pReply, entry.line_);
    numItems++;
    return false;
  });

  JDWP::Set4BE(expandBufGetBuffer(pReply) + numLinesOffset, numItems);
}

void Dbg::OutputVariableTable(JDWP::RefTypeId, JDWP::MethodId method_id, bool with_generic,
                              JDWP::ExpandBuf* pReply) {
  ArtMethod* m = FromMethodId(method_id);
  CodeItemDebugInfoAccessor accessor(m->DexInstructionDebugInfo());

  // arg_count considers doubles and longs to take 2 units.
  // variable_count considers everything to take 1 unit.
  expandBufAdd4BE(pReply, GetMethodNumArgRegistersIncludingThis(m));

  // We don't know the total number of variables yet, so leave a blank and update it later.
  size_t variable_count_offset = expandBufGetLength(pReply);
  expandBufAdd4BE(pReply, 0);

  size_t variable_count = 0;

  if (accessor.HasCodeItem()) {
    accessor.DecodeDebugLocalInfo(m->IsStatic(),
                                  m->GetDexMethodIndex(),
                                  [&](const DexFile::LocalInfo& entry)
        REQUIRES_SHARED(Locks::mutator_lock_) {
      uint16_t slot = entry.reg_;
      VLOG(jdwp) << StringPrintf("    %2zd: %d(%d) '%s' '%s' '%s' actual slot=%d mangled slot=%d",
                                 variable_count,
                                 entry.start_address_,
                                 entry.end_address_ - entry.start_address_,
                                 entry.name_,
                                 entry.descriptor_, entry.signature_,
                                 slot,
                                 MangleSlot(slot, m));

      slot = MangleSlot(slot, m);

      expandBufAdd8BE(pReply, entry.start_address_);
      expandBufAddUtf8String(pReply, entry.name_);
      expandBufAddUtf8String(pReply, entry.descriptor_);
      if (with_generic) {
        expandBufAddUtf8String(pReply, entry.signature_);
      }
      expandBufAdd4BE(pReply, entry.end_address_- entry.start_address_);
      expandBufAdd4BE(pReply, slot);

      ++variable_count;
    });
  }

  JDWP::Set4BE(expandBufGetBuffer(pReply) + variable_count_offset, variable_count);
}

void Dbg::OutputMethodReturnValue(JDWP::MethodId method_id, const JValue* return_value,
                                  JDWP::ExpandBuf* pReply) {
  ArtMethod* m = FromMethodId(method_id);
  JDWP::JdwpTag tag = BasicTagFromDescriptor(m->GetShorty());
  OutputJValue(tag, return_value, pReply);
}

void Dbg::OutputFieldValue(JDWP::FieldId field_id, const JValue* field_value,
                           JDWP::ExpandBuf* pReply) {
  ArtField* f = FromFieldId(field_id);
  JDWP::JdwpTag tag = BasicTagFromDescriptor(f->GetTypeDescriptor());
  OutputJValue(tag, field_value, pReply);
}

JDWP::JdwpError Dbg::GetBytecodes(JDWP::RefTypeId, JDWP::MethodId method_id,
                                  std::vector<uint8_t>* bytecodes) {
  ArtMethod* m = FromMethodId(method_id);
  if (m == nullptr) {
    return JDWP::ERR_INVALID_METHODID;
  }
  CodeItemDataAccessor accessor(m->DexInstructionData());
  size_t byte_count = accessor.InsnsSizeInCodeUnits() * 2;
  const uint8_t* begin = reinterpret_cast<const uint8_t*>(accessor.Insns());
  const uint8_t* end = begin + byte_count;
  for (const uint8_t* p = begin; p != end; ++p) {
    bytecodes->push_back(*p);
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpTag Dbg::GetFieldBasicTag(JDWP::FieldId field_id) {
  return BasicTagFromDescriptor(FromFieldId(field_id)->GetTypeDescriptor());
}

JDWP::JdwpTag Dbg::GetStaticFieldBasicTag(JDWP::FieldId field_id) {
  return BasicTagFromDescriptor(FromFieldId(field_id)->GetTypeDescriptor());
}

static JValue GetArtFieldValue(ArtField* f, ObjPtr<mirror::Object> o)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Primitive::Type fieldType = f->GetTypeAsPrimitiveType();
  JValue field_value;
  switch (fieldType) {
    case Primitive::kPrimBoolean:
      field_value.SetZ(f->GetBoolean(o));
      return field_value;

    case Primitive::kPrimByte:
      field_value.SetB(f->GetByte(o));
      return field_value;

    case Primitive::kPrimChar:
      field_value.SetC(f->GetChar(o));
      return field_value;

    case Primitive::kPrimShort:
      field_value.SetS(f->GetShort(o));
      return field_value;

    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      // Int and Float must be treated as 32-bit values in JDWP.
      field_value.SetI(f->GetInt(o));
      return field_value;

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      // Long and Double must be treated as 64-bit values in JDWP.
      field_value.SetJ(f->GetLong(o));
      return field_value;

    case Primitive::kPrimNot:
      field_value.SetL(f->GetObject(o));
      return field_value;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Attempt to read from field of type 'void'";
      UNREACHABLE();
  }
  LOG(FATAL) << "Attempt to read from field of unknown type";
  UNREACHABLE();
}

static JDWP::JdwpError GetFieldValueImpl(JDWP::RefTypeId ref_type_id, JDWP::ObjectId object_id,
                                         JDWP::FieldId field_id, JDWP::ExpandBuf* pReply,
                                         bool is_static)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Class> c = DecodeClass(ref_type_id, &error);
  if (ref_type_id != 0 && c == nullptr) {
    return error;
  }

  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  MutableHandle<mirror::Object>
      o(hs.NewHandle(Dbg::GetObjectRegistry()->Get<mirror::Object>(object_id, &error)));
  if ((!is_static && o == nullptr) || error != JDWP::ERR_NONE) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  ArtField* f = FromFieldId(field_id);

  ObjPtr<mirror::Class> receiver_class = c;
  if (receiver_class == nullptr && o != nullptr) {
    receiver_class = o->GetClass();
  }

  // TODO: should we give up now if receiver_class is null?
  if (receiver_class != nullptr && !f->GetDeclaringClass()->IsAssignableFrom(receiver_class)) {
    LOG(INFO) << "ERR_INVALID_FIELDID: " << f->PrettyField() << " "
              << receiver_class->PrettyClass();
    return JDWP::ERR_INVALID_FIELDID;
  }

  // Ensure the field's class is initialized.
  Handle<mirror::Class> klass(hs.NewHandle(f->GetDeclaringClass()));
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, klass, true, false)) {
    LOG(WARNING) << "Not able to initialize class for SetValues: "
                 << mirror::Class::PrettyClass(klass.Get());
  }

  // The RI only enforces the static/non-static mismatch in one direction.
  // TODO: should we change the tests and check both?
  if (is_static) {
    if (!f->IsStatic()) {
      return JDWP::ERR_INVALID_FIELDID;
    }
  } else {
    if (f->IsStatic()) {
      LOG(WARNING) << "Ignoring non-nullptr receiver for ObjectReference.GetValues"
                   << " on static field " << f->PrettyField();
    }
  }
  if (f->IsStatic()) {
    o.Assign(f->GetDeclaringClass());
  }

  JValue field_value(GetArtFieldValue(f, o.Get()));
  JDWP::JdwpTag tag = BasicTagFromDescriptor(f->GetTypeDescriptor());
  Dbg::OutputJValue(tag, &field_value, pReply);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetFieldValue(JDWP::ObjectId object_id, JDWP::FieldId field_id,
                                   JDWP::ExpandBuf* pReply) {
  return GetFieldValueImpl(0, object_id, field_id, pReply, false);
}

JDWP::JdwpError Dbg::GetStaticFieldValue(JDWP::RefTypeId ref_type_id, JDWP::FieldId field_id,
                                         JDWP::ExpandBuf* pReply) {
  return GetFieldValueImpl(ref_type_id, 0, field_id, pReply, true);
}

static JDWP::JdwpError SetArtFieldValue(ArtField* f,
                                        ObjPtr<mirror::Object> o,
                                        uint64_t value,
                                        int width)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Primitive::Type fieldType = f->GetTypeAsPrimitiveType();
  // Debugging only happens at runtime so we know we are not running in a transaction.
  static constexpr bool kNoTransactionMode = false;
  switch (fieldType) {
    case Primitive::kPrimBoolean:
      CHECK_EQ(width, 1);
      f->SetBoolean<kNoTransactionMode>(o, static_cast<uint8_t>(value));
      return JDWP::ERR_NONE;

    case Primitive::kPrimByte:
      CHECK_EQ(width, 1);
      f->SetByte<kNoTransactionMode>(o, static_cast<uint8_t>(value));
      return JDWP::ERR_NONE;

    case Primitive::kPrimChar:
      CHECK_EQ(width, 2);
      f->SetChar<kNoTransactionMode>(o, static_cast<uint16_t>(value));
      return JDWP::ERR_NONE;

    case Primitive::kPrimShort:
      CHECK_EQ(width, 2);
      f->SetShort<kNoTransactionMode>(o, static_cast<int16_t>(value));
      return JDWP::ERR_NONE;

    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      CHECK_EQ(width, 4);
      // Int and Float must be treated as 32-bit values in JDWP.
      f->SetInt<kNoTransactionMode>(o, static_cast<int32_t>(value));
      return JDWP::ERR_NONE;

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      CHECK_EQ(width, 8);
      // Long and Double must be treated as 64-bit values in JDWP.
      f->SetLong<kNoTransactionMode>(o, value);
      return JDWP::ERR_NONE;

    case Primitive::kPrimNot: {
      JDWP::JdwpError error;
      ObjPtr<mirror::Object> v = Dbg::GetObjectRegistry()->Get<mirror::Object>(value, &error);
      if (error != JDWP::ERR_NONE) {
        return JDWP::ERR_INVALID_OBJECT;
      }
      if (v != nullptr) {
        ObjPtr<mirror::Class> field_type;
        {
          StackHandleScope<2> hs(Thread::Current());
          HandleWrapperObjPtr<mirror::Object> h_v(hs.NewHandleWrapper(&v));
          HandleWrapperObjPtr<mirror::Object> h_o(hs.NewHandleWrapper(&o));
          field_type = f->ResolveType();
        }
        if (!field_type->IsAssignableFrom(v->GetClass())) {
          return JDWP::ERR_INVALID_OBJECT;
        }
      }
      f->SetObject<kNoTransactionMode>(o, v);
      return JDWP::ERR_NONE;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Attempt to write to field of type 'void'";
      UNREACHABLE();
  }
  LOG(FATAL) << "Attempt to write to field of unknown type";
  UNREACHABLE();
}

static JDWP::JdwpError SetFieldValueImpl(JDWP::ObjectId object_id, JDWP::FieldId field_id,
                                         uint64_t value, int width, bool is_static)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  JDWP::JdwpError error;
  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  MutableHandle<mirror::Object>
      o(hs.NewHandle(Dbg::GetObjectRegistry()->Get<mirror::Object>(object_id, &error)));
  if ((!is_static && o == nullptr) || error != JDWP::ERR_NONE) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  ArtField* f = FromFieldId(field_id);

  // Ensure the field's class is initialized.
  Handle<mirror::Class> klass(hs.NewHandle(f->GetDeclaringClass()));
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, klass, true, false)) {
    LOG(WARNING) << "Not able to initialize class for SetValues: "
                 << mirror::Class::PrettyClass(klass.Get());
  }

  // The RI only enforces the static/non-static mismatch in one direction.
  // TODO: should we change the tests and check both?
  if (is_static) {
    if (!f->IsStatic()) {
      return JDWP::ERR_INVALID_FIELDID;
    }
  } else {
    if (f->IsStatic()) {
      LOG(WARNING) << "Ignoring non-nullptr receiver for ObjectReference.SetValues"
                   << " on static field " << f->PrettyField();
    }
  }
  if (f->IsStatic()) {
    o.Assign(f->GetDeclaringClass());
  }
  return SetArtFieldValue(f, o.Get(), value, width);
}

JDWP::JdwpError Dbg::SetFieldValue(JDWP::ObjectId object_id, JDWP::FieldId field_id, uint64_t value,
                                   int width) {
  return SetFieldValueImpl(object_id, field_id, value, width, false);
}

JDWP::JdwpError Dbg::SetStaticFieldValue(JDWP::FieldId field_id, uint64_t value, int width) {
  return SetFieldValueImpl(0, field_id, value, width, true);
}

JDWP::JdwpError Dbg::StringToUtf8(JDWP::ObjectId string_id, std::string* str) {
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> obj = gRegistry->Get<mirror::Object>(string_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (obj == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    ObjPtr<mirror::Class> java_lang_String =
        soa.Decode<mirror::Class>(WellKnownClasses::java_lang_String);
    if (!java_lang_String->IsAssignableFrom(obj->GetClass())) {
      // This isn't a string.
      return JDWP::ERR_INVALID_STRING;
    }
  }
  *str = obj->AsString()->ToModifiedUtf8();
  return JDWP::ERR_NONE;
}

void Dbg::OutputJValue(JDWP::JdwpTag tag, const JValue* return_value, JDWP::ExpandBuf* pReply) {
  if (IsPrimitiveTag(tag)) {
    expandBufAdd1(pReply, tag);
    if (tag == JDWP::JT_BOOLEAN || tag == JDWP::JT_BYTE) {
      expandBufAdd1(pReply, return_value->GetI());
    } else if (tag == JDWP::JT_CHAR || tag == JDWP::JT_SHORT) {
      expandBufAdd2BE(pReply, return_value->GetI());
    } else if (tag == JDWP::JT_FLOAT || tag == JDWP::JT_INT) {
      expandBufAdd4BE(pReply, return_value->GetI());
    } else if (tag == JDWP::JT_DOUBLE || tag == JDWP::JT_LONG) {
      expandBufAdd8BE(pReply, return_value->GetJ());
    } else {
      CHECK_EQ(tag, JDWP::JT_VOID);
    }
  } else {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    ObjPtr<mirror::Object> value = return_value->GetL();
    expandBufAdd1(pReply, TagFromObject(soa, value));
    expandBufAddObjectId(pReply, gRegistry->Add(value));
  }
}

JDWP::JdwpError Dbg::GetThreadName(JDWP::ObjectId thread_id, std::string* name) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE && error != JDWP::ERR_THREAD_NOT_ALIVE) {
    return error;
  }

  // We still need to report the zombie threads' names, so we can't just call Thread::GetThreadName.
  ObjPtr<mirror::Object> thread_object = gRegistry->Get<mirror::Object>(thread_id, &error);
  CHECK(thread_object != nullptr) << error;
  ArtField* java_lang_Thread_name_field =
      jni::DecodeArtField(WellKnownClasses::java_lang_Thread_name);
  ObjPtr<mirror::String> s(java_lang_Thread_name_field->GetObject(thread_object)->AsString());
  if (s != nullptr) {
    *name = s->ToModifiedUtf8();
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetThreadGroup(JDWP::ObjectId thread_id, JDWP::ExpandBuf* pReply) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> thread_object = gRegistry->Get<mirror::Object>(thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  ScopedAssertNoThreadSuspension ants("Debugger: GetThreadGroup");
  // Okay, so it's an object, but is it actually a thread?
  DecodeThread(soa, thread_id, &error);
  if (error == JDWP::ERR_THREAD_NOT_ALIVE) {
    // Zombie threads are in the null group.
    expandBufAddObjectId(pReply, JDWP::ObjectId(0));
    error = JDWP::ERR_NONE;
  } else if (error == JDWP::ERR_NONE) {
    ObjPtr<mirror::Class> c = soa.Decode<mirror::Class>(WellKnownClasses::java_lang_Thread);
    CHECK(c != nullptr);
    ArtField* f = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_group);
    CHECK(f != nullptr);
    ObjPtr<mirror::Object> group = f->GetObject(thread_object);
    CHECK(group != nullptr);
    JDWP::ObjectId thread_group_id = gRegistry->Add(group);
    expandBufAddObjectId(pReply, thread_group_id);
  }
  return error;
}

static ObjPtr<mirror::Object> DecodeThreadGroup(ScopedObjectAccessUnchecked& soa,
                                                JDWP::ObjectId thread_group_id,
                                                JDWP::JdwpError* error)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Object> thread_group =
      Dbg::GetObjectRegistry()->Get<mirror::Object>(thread_group_id, error);
  if (*error != JDWP::ERR_NONE) {
    return nullptr;
  }
  if (thread_group == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
  }
  ObjPtr<mirror::Class> c =
      soa.Decode<mirror::Class>(WellKnownClasses::java_lang_ThreadGroup);
  CHECK(c != nullptr);
  if (!c->IsAssignableFrom(thread_group->GetClass())) {
    // This is not a java.lang.ThreadGroup.
    *error = JDWP::ERR_INVALID_THREAD_GROUP;
    return nullptr;
  }
  *error = JDWP::ERR_NONE;
  return thread_group;
}

JDWP::JdwpError Dbg::GetThreadGroupName(JDWP::ObjectId thread_group_id, JDWP::ExpandBuf* pReply) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> thread_group = DecodeThreadGroup(soa, thread_group_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  ScopedAssertNoThreadSuspension ants("Debugger: GetThreadGroupName");
  ArtField* f = jni::DecodeArtField(WellKnownClasses::java_lang_ThreadGroup_name);
  CHECK(f != nullptr);
  ObjPtr<mirror::String> s = f->GetObject(thread_group)->AsString();

  std::string thread_group_name(s->ToModifiedUtf8());
  expandBufAddUtf8String(pReply, thread_group_name);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetThreadGroupParent(JDWP::ObjectId thread_group_id, JDWP::ExpandBuf* pReply) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> thread_group = DecodeThreadGroup(soa, thread_group_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  ObjPtr<mirror::Object> parent;
  {
    ScopedAssertNoThreadSuspension ants("Debugger: GetThreadGroupParent");
    ArtField* f = jni::DecodeArtField(WellKnownClasses::java_lang_ThreadGroup_parent);
    CHECK(f != nullptr);
    parent = f->GetObject(thread_group);
  }
  JDWP::ObjectId parent_group_id = gRegistry->Add(parent);
  expandBufAddObjectId(pReply, parent_group_id);
  return JDWP::ERR_NONE;
}

static void GetChildThreadGroups(ObjPtr<mirror::Object> thread_group,
                                 std::vector<JDWP::ObjectId>* child_thread_group_ids)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  CHECK(thread_group != nullptr);

  // Get the int "ngroups" count of this thread group...
  ArtField* ngroups_field = jni::DecodeArtField(WellKnownClasses::java_lang_ThreadGroup_ngroups);
  CHECK(ngroups_field != nullptr);
  const int32_t size = ngroups_field->GetInt(thread_group);
  if (size == 0) {
    return;
  }

  // Get the ThreadGroup[] "groups" out of this thread group...
  ArtField* groups_field = jni::DecodeArtField(WellKnownClasses::java_lang_ThreadGroup_groups);
  ObjPtr<mirror::Object> groups_array = groups_field->GetObject(thread_group);

  CHECK(groups_array != nullptr);
  CHECK(groups_array->IsObjectArray());

  ObjPtr<mirror::ObjectArray<mirror::Object>> groups_array_as_array =
      groups_array->AsObjectArray<mirror::Object>();

  // Copy the first 'size' elements out of the array into the result.
  ObjectRegistry* registry = Dbg::GetObjectRegistry();
  for (int32_t i = 0; i < size; ++i) {
    child_thread_group_ids->push_back(registry->Add(groups_array_as_array->Get(i)));
  }
}

JDWP::JdwpError Dbg::GetThreadGroupChildren(JDWP::ObjectId thread_group_id,
                                            JDWP::ExpandBuf* pReply) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> thread_group = DecodeThreadGroup(soa, thread_group_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }

  // Add child threads.
  {
    std::vector<JDWP::ObjectId> child_thread_ids;
    GetThreads(thread_group, &child_thread_ids);
    expandBufAdd4BE(pReply, child_thread_ids.size());
    for (JDWP::ObjectId child_thread_id : child_thread_ids) {
      expandBufAddObjectId(pReply, child_thread_id);
    }
  }

  // Add child thread groups.
  {
    std::vector<JDWP::ObjectId> child_thread_groups_ids;
    GetChildThreadGroups(thread_group, &child_thread_groups_ids);
    expandBufAdd4BE(pReply, child_thread_groups_ids.size());
    for (JDWP::ObjectId child_thread_group_id : child_thread_groups_ids) {
      expandBufAddObjectId(pReply, child_thread_group_id);
    }
  }

  return JDWP::ERR_NONE;
}

JDWP::ObjectId Dbg::GetSystemThreadGroupId() {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  ArtField* f = jni::DecodeArtField(WellKnownClasses::java_lang_ThreadGroup_systemThreadGroup);
  ObjPtr<mirror::Object> group = f->GetObject(f->GetDeclaringClass());
  return gRegistry->Add(group);
}

JDWP::JdwpThreadStatus Dbg::ToJdwpThreadStatus(ThreadState state) {
  switch (state) {
    case kBlocked:
      return JDWP::TS_MONITOR;
    case kNative:
    case kRunnable:
    case kSuspended:
      return JDWP::TS_RUNNING;
    case kSleeping:
      return JDWP::TS_SLEEPING;
    case kStarting:
    case kTerminated:
      return JDWP::TS_ZOMBIE;
    case kTimedWaiting:
    case kWaitingForTaskProcessor:
    case kWaitingForLockInflation:
    case kWaitingForCheckPointsToRun:
    case kWaitingForDebuggerSend:
    case kWaitingForDebuggerSuspension:
    case kWaitingForDebuggerToAttach:
    case kWaitingForDeoptimization:
    case kWaitingForGcToComplete:
    case kWaitingForGetObjectsAllocated:
    case kWaitingForJniOnLoad:
    case kWaitingForMethodTracingStart:
    case kWaitingForSignalCatcherOutput:
    case kWaitingForVisitObjects:
    case kWaitingInMainDebuggerLoop:
    case kWaitingInMainSignalCatcherLoop:
    case kWaitingPerformingGc:
    case kWaitingWeakGcRootRead:
    case kWaitingForGcThreadFlip:
    case kNativeForAbort:
    case kWaiting:
      return JDWP::TS_WAIT;
      // Don't add a 'default' here so the compiler can spot incompatible enum changes.
  }
  LOG(FATAL) << "Unknown thread state: " << state;
  UNREACHABLE();
}

JDWP::JdwpError Dbg::GetThreadStatus(JDWP::ObjectId thread_id, JDWP::JdwpThreadStatus* pThreadStatus,
                                     JDWP::JdwpSuspendStatus* pSuspendStatus) {
  ScopedObjectAccess soa(Thread::Current());

  *pSuspendStatus = JDWP::SUSPEND_STATUS_NOT_SUSPENDED;

  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    if (error == JDWP::ERR_THREAD_NOT_ALIVE) {
      *pThreadStatus = JDWP::TS_ZOMBIE;
      return JDWP::ERR_NONE;
    }
    return error;
  }

  if (IsSuspendedForDebugger(soa, thread)) {
    *pSuspendStatus = JDWP::SUSPEND_STATUS_SUSPENDED;
  }

  *pThreadStatus = ToJdwpThreadStatus(thread->GetState());
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetThreadDebugSuspendCount(JDWP::ObjectId thread_id, JDWP::ExpandBuf* pReply) {
  ScopedObjectAccess soa(Thread::Current());
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  MutexLock mu2(soa.Self(), *Locks::thread_suspend_count_lock_);
  expandBufAdd4BE(pReply, thread->GetDebugSuspendCount());
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::Interrupt(JDWP::ObjectId thread_id) {
  ScopedObjectAccess soa(Thread::Current());
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  thread->Interrupt(soa.Self());
  return JDWP::ERR_NONE;
}

static bool IsInDesiredThreadGroup(ObjPtr<mirror::Object> desired_thread_group,
                                   ObjPtr<mirror::Object> peer)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Do we want threads from all thread groups?
  if (desired_thread_group == nullptr) {
    return true;
  }
  ArtField* thread_group_field = jni::DecodeArtField(WellKnownClasses::java_lang_Thread_group);
  DCHECK(thread_group_field != nullptr);
  ObjPtr<mirror::Object> group = thread_group_field->GetObject(peer);
  return (group == desired_thread_group);
}

void Dbg::GetThreads(ObjPtr<mirror::Object> thread_group, std::vector<JDWP::ObjectId>* thread_ids) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  std::list<Thread*> all_threads_list;
  {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    all_threads_list = Runtime::Current()->GetThreadList()->GetList();
  }
  for (Thread* t : all_threads_list) {
    if (t == Dbg::GetDebugThread()) {
      // Skip the JDWP thread. Some debuggers get bent out of shape when they can't suspend and
      // query all threads, so it's easier if we just don't tell them about this thread.
      continue;
    }
    if (t->IsStillStarting()) {
      // This thread is being started (and has been registered in the thread list). However, it is
      // not completely started yet so we must ignore it.
      continue;
    }
    ObjPtr<mirror::Object> peer = t->GetPeerFromOtherThread();
    if (peer == nullptr) {
      // peer might be null if the thread is still starting up. We can't tell the debugger about
      // this thread yet.
      // TODO: if we identified threads to the debugger by their Thread*
      // rather than their peer's mirror::Object*, we could fix this.
      // Doing so might help us report ZOMBIE threads too.
      continue;
    }
    if (IsInDesiredThreadGroup(thread_group, peer)) {
      thread_ids->push_back(gRegistry->Add(peer));
    }
  }
}

static int GetStackDepth(Thread* thread) REQUIRES_SHARED(Locks::mutator_lock_) {
  size_t depth = 0u;
  StackVisitor::WalkStack(
      [&depth](const StackVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        if (!visitor->GetMethod()->IsRuntimeMethod()) {
          ++depth;
        }
        return true;
      },
      thread,
      /* context= */ nullptr,
      StackVisitor::StackWalkKind::kIncludeInlinedFrames);
  return depth;
}

JDWP::JdwpError Dbg::GetThreadFrameCount(JDWP::ObjectId thread_id, size_t* result) {
  ScopedObjectAccess soa(Thread::Current());
  JDWP::JdwpError error;
  *result = 0;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }
  *result = GetStackDepth(thread);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetThreadFrames(JDWP::ObjectId thread_id,
                                     const size_t start_frame,
                                     const size_t frame_count,
                                     JDWP::ExpandBuf* buf) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }

  expandBufAdd4BE(buf, frame_count);

  size_t depth = 0u;
  StackVisitor::WalkStack(
      [&](StackVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        if (visitor->GetMethod()->IsRuntimeMethod()) {
          return true;  // The debugger can't do anything useful with a frame that has no Method*.
        }
        if (depth >= start_frame + frame_count) {
          return false;
        }
        if (depth >= start_frame) {
          JDWP::FrameId frame_id(visitor->GetFrameId());
          JDWP::JdwpLocation location;
          SetJdwpLocation(&location, visitor->GetMethod(), visitor->GetDexPc());
          VLOG(jdwp)
              << StringPrintf("    Frame %3zd: id=%3" PRIu64 " ", depth, frame_id) << location;
          expandBufAdd8BE(buf, frame_id);
          expandBufAddLocation(buf, location);
        }
        ++depth;
        return true;
      },
      thread,
      /* context= */ nullptr,
      StackVisitor::StackWalkKind::kIncludeInlinedFrames);

  return JDWP::ERR_NONE;
}

JDWP::ObjectId Dbg::GetThreadSelfId() {
  return GetThreadId(Thread::Current());
}

JDWP::ObjectId Dbg::GetThreadId(Thread* thread) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  return gRegistry->Add(thread->GetPeerFromOtherThread());
}

void Dbg::SuspendVM() {
  // Avoid a deadlock between GC and debugger where GC gets suspended during GC. b/25800335.
  gc::ScopedGCCriticalSection gcs(Thread::Current(),
                                  gc::kGcCauseDebugger,
                                  gc::kCollectorTypeDebugger);
  Runtime::Current()->GetThreadList()->SuspendAllForDebugger();
}

void Dbg::ResumeVM() {
  Runtime::Current()->GetThreadList()->ResumeAllForDebugger();
}

JDWP::JdwpError Dbg::SuspendThread(JDWP::ObjectId thread_id, bool request_suspension) {
  Thread* self = Thread::Current();
  ScopedLocalRef<jobject> peer(self->GetJniEnv(), nullptr);
  {
    ScopedObjectAccess soa(self);
    JDWP::JdwpError error;
    peer.reset(soa.AddLocalReference<jobject>(gRegistry->Get<mirror::Object>(thread_id, &error)));
  }
  if (peer.get() == nullptr) {
    return JDWP::ERR_THREAD_NOT_ALIVE;
  }
  // Suspend thread to build stack trace.
  bool timed_out;
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  Thread* thread = thread_list->SuspendThreadByPeer(peer.get(),
                                                    request_suspension,
                                                    SuspendReason::kForDebugger,
                                                    &timed_out);
  if (thread != nullptr) {
    return JDWP::ERR_NONE;
  } else if (timed_out) {
    return JDWP::ERR_INTERNAL;
  } else {
    return JDWP::ERR_THREAD_NOT_ALIVE;
  }
}

void Dbg::ResumeThread(JDWP::ObjectId thread_id) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  ObjPtr<mirror::Object> peer = gRegistry->Get<mirror::Object>(thread_id, &error);
  CHECK(peer != nullptr) << error;
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    thread = Thread::FromManagedThread(soa, peer);
  }
  if (thread == nullptr) {
    LOG(WARNING) << "No such thread for resume: " << peer;
    return;
  }
  bool needs_resume;
  {
    MutexLock mu2(soa.Self(), *Locks::thread_suspend_count_lock_);
    needs_resume = thread->GetDebugSuspendCount() > 0;
  }
  if (needs_resume) {
    bool resumed = Runtime::Current()->GetThreadList()->Resume(thread, SuspendReason::kForDebugger);
    DCHECK(resumed);
  }
}

void Dbg::SuspendSelf() {
  Runtime::Current()->GetThreadList()->SuspendSelfForDebugger();
}

JDWP::JdwpError Dbg::GetThisObject(JDWP::ObjectId thread_id, JDWP::FrameId frame_id,
                                   JDWP::ObjectId* result) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }
  std::unique_ptr<Context> context(Context::Create());
  ObjPtr<mirror::Object> this_object = nullptr;
  StackVisitor::WalkStack(
      [&](art::StackVisitor* stack_visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        if (frame_id != stack_visitor->GetFrameId()) {
          return true;  // continue
        } else {
          this_object = stack_visitor->GetThisObject();
          return false;
        }
      },
      thread,
      context.get(),
      art::StackVisitor::StackWalkKind::kIncludeInlinedFrames);
  *result = gRegistry->Add(this_object);
  return JDWP::ERR_NONE;
}

template <typename FrameHandler>
static JDWP::JdwpError FindAndHandleNonNativeFrame(Thread* thread,
                                                   JDWP::FrameId frame_id,
                                                   const FrameHandler& handler)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  JDWP::JdwpError result = JDWP::ERR_INVALID_FRAMEID;
  std::unique_ptr<Context> context(Context::Create());
  StackVisitor::WalkStack(
      [&](art::StackVisitor* stack_visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        if (stack_visitor->GetFrameId() != frame_id) {
          return true;  // Not our frame, carry on.
        }
        ArtMethod* m = stack_visitor->GetMethod();
        if (m->IsNative()) {
          // We can't read/write local value from/into native method.
          result = JDWP::ERR_OPAQUE_FRAME;
        } else {
          // We found our frame.
          result = handler(stack_visitor);
        }
        return false;
      },
      thread,
      context.get(),
      art::StackVisitor::StackWalkKind::kIncludeInlinedFrames);
  return result;
}

JDWP::JdwpError Dbg::GetLocalValues(JDWP::Request* request, JDWP::ExpandBuf* pReply) {
  JDWP::ObjectId thread_id = request->ReadThreadId();
  JDWP::FrameId frame_id = request->ReadFrameId();

  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }

  return FindAndHandleNonNativeFrame(
      thread,
      frame_id,
      [&](art::StackVisitor* stack_visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        // Read the values from visitor's context.
        int32_t slot_count = request->ReadSigned32("slot count");
        expandBufAdd4BE(pReply, slot_count);     /* "int values" */
        for (int32_t i = 0; i < slot_count; ++i) {
          uint32_t slot = request->ReadUnsigned32("slot");
          JDWP::JdwpTag reqSigByte = request->ReadTag();

          VLOG(jdwp) << "    --> slot " << slot << " " << reqSigByte;

          size_t width = Dbg::GetTagWidth(reqSigByte);
          uint8_t* ptr = expandBufAddSpace(pReply, width + 1);
          error = Dbg::GetLocalValue(*stack_visitor, soa, slot, reqSigByte, ptr, width);
          if (error != JDWP::ERR_NONE) {
            return error;
          }
        }
        return JDWP::ERR_NONE;
      });
}

constexpr JDWP::JdwpError kStackFrameLocalAccessError = JDWP::ERR_ABSENT_INFORMATION;

static std::string GetStackContextAsString(const StackVisitor& visitor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return StringPrintf(" at DEX pc 0x%08x in method %s", visitor.GetDexPc(false),
                      ArtMethod::PrettyMethod(visitor.GetMethod()).c_str());
}

static JDWP::JdwpError FailGetLocalValue(const StackVisitor& visitor, uint16_t vreg,
                                         JDWP::JdwpTag tag)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(ERROR) << "Failed to read " << tag << " local from register v" << vreg
             << GetStackContextAsString(visitor);
  return kStackFrameLocalAccessError;
}

JDWP::JdwpError Dbg::GetLocalValue(const StackVisitor& visitor, ScopedObjectAccessUnchecked& soa,
                                   int slot, JDWP::JdwpTag tag, uint8_t* buf, size_t width) {
  ArtMethod* m = visitor.GetMethod();
  JDWP::JdwpError error = JDWP::ERR_NONE;
  uint16_t vreg = DemangleSlot(slot, m, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  // TODO: check that the tag is compatible with the actual type of the slot!
  switch (tag) {
    case JDWP::JT_BOOLEAN: {
      CHECK_EQ(width, 1U);
      uint32_t intVal;
      if (!visitor.GetVReg(m, vreg, kIntVReg, &intVal)) {
        return FailGetLocalValue(visitor, vreg, tag);
      }
      VLOG(jdwp) << "get boolean local " << vreg << " = " << intVal;
      JDWP::Set1(buf + 1, intVal != 0);
      break;
    }
    case JDWP::JT_BYTE: {
      CHECK_EQ(width, 1U);
      uint32_t intVal;
      if (!visitor.GetVReg(m, vreg, kIntVReg, &intVal)) {
        return FailGetLocalValue(visitor, vreg, tag);
      }
      VLOG(jdwp) << "get byte local " << vreg << " = " << intVal;
      JDWP::Set1(buf + 1, intVal);
      break;
    }
    case JDWP::JT_SHORT:
    case JDWP::JT_CHAR: {
      CHECK_EQ(width, 2U);
      uint32_t intVal;
      if (!visitor.GetVReg(m, vreg, kIntVReg, &intVal)) {
        return FailGetLocalValue(visitor, vreg, tag);
      }
      VLOG(jdwp) << "get short/char local " << vreg << " = " << intVal;
      JDWP::Set2BE(buf + 1, intVal);
      break;
    }
    case JDWP::JT_INT: {
      CHECK_EQ(width, 4U);
      uint32_t intVal;
      if (!visitor.GetVReg(m, vreg, kIntVReg, &intVal)) {
        return FailGetLocalValue(visitor, vreg, tag);
      }
      VLOG(jdwp) << "get int local " << vreg << " = " << intVal;
      JDWP::Set4BE(buf + 1, intVal);
      break;
    }
    case JDWP::JT_FLOAT: {
      CHECK_EQ(width, 4U);
      uint32_t intVal;
      if (!visitor.GetVReg(m, vreg, kFloatVReg, &intVal)) {
        return FailGetLocalValue(visitor, vreg, tag);
      }
      VLOG(jdwp) << "get float local " << vreg << " = " << intVal;
      JDWP::Set4BE(buf + 1, intVal);
      break;
    }
    case JDWP::JT_ARRAY:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP: {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      uint32_t intVal;
      if (!visitor.GetVReg(m, vreg, kReferenceVReg, &intVal)) {
        return FailGetLocalValue(visitor, vreg, tag);
      }
      ObjPtr<mirror::Object> o = reinterpret_cast<mirror::Object*>(intVal);
      VLOG(jdwp) << "get " << tag << " object local " << vreg << " = " << o;
      if (!Runtime::Current()->GetHeap()->IsValidObjectAddress(o.Ptr())) {
        LOG(FATAL) << StringPrintf("Found invalid object %#" PRIxPTR " in register v%u",
                                   reinterpret_cast<uintptr_t>(o.Ptr()), vreg)
                                   << GetStackContextAsString(visitor);
        UNREACHABLE();
      }
      tag = TagFromObject(soa, o);
      JDWP::SetObjectId(buf + 1, gRegistry->Add(o));
      break;
    }
    case JDWP::JT_DOUBLE: {
      CHECK_EQ(width, 8U);
      uint64_t longVal;
      if (!visitor.GetVRegPair(m, vreg, kDoubleLoVReg, kDoubleHiVReg, &longVal)) {
        return FailGetLocalValue(visitor, vreg, tag);
      }
      VLOG(jdwp) << "get double local " << vreg << " = " << longVal;
      JDWP::Set8BE(buf + 1, longVal);
      break;
    }
    case JDWP::JT_LONG: {
      CHECK_EQ(width, 8U);
      uint64_t longVal;
      if (!visitor.GetVRegPair(m, vreg, kLongLoVReg, kLongHiVReg, &longVal)) {
        return FailGetLocalValue(visitor, vreg, tag);
      }
      VLOG(jdwp) << "get long local " << vreg << " = " << longVal;
      JDWP::Set8BE(buf + 1, longVal);
      break;
    }
    default:
      LOG(FATAL) << "Unknown tag " << tag;
      UNREACHABLE();
  }

  // Prepend tag, which may have been updated.
  JDWP::Set1(buf, tag);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::SetLocalValues(JDWP::Request* request) {
  JDWP::ObjectId thread_id = request->ReadThreadId();
  JDWP::FrameId frame_id = request->ReadFrameId();

  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }

  return FindAndHandleNonNativeFrame(
      thread,
      frame_id,
      [&](art::StackVisitor* stack_visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        // Writes the values into visitor's context.
        int32_t slot_count = request->ReadSigned32("slot count");
        for (int32_t i = 0; i < slot_count; ++i) {
          uint32_t slot = request->ReadUnsigned32("slot");
          JDWP::JdwpTag sigByte = request->ReadTag();
          size_t width = Dbg::GetTagWidth(sigByte);
          uint64_t value = request->ReadValue(width);

          VLOG(jdwp) << "    --> slot " << slot << " " << sigByte << " " << value;
          error = Dbg::SetLocalValue(thread, *stack_visitor, slot, sigByte, value, width);
          if (error != JDWP::ERR_NONE) {
            return error;
          }
        }
        return JDWP::ERR_NONE;
      });
}

template<typename T>
static JDWP::JdwpError FailSetLocalValue(const StackVisitor& visitor, uint16_t vreg,
                                         JDWP::JdwpTag tag, T value)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(ERROR) << "Failed to write " << tag << " local " << value
             << " (0x" << std::hex << value << ") into register v" << vreg
             << GetStackContextAsString(visitor);
  return kStackFrameLocalAccessError;
}

JDWP::JdwpError Dbg::SetLocalValue(Thread* thread, StackVisitor& visitor, int slot,
                                   JDWP::JdwpTag tag, uint64_t value, size_t width) {
  ArtMethod* m = visitor.GetMethod();
  JDWP::JdwpError error = JDWP::ERR_NONE;
  uint16_t vreg = DemangleSlot(slot, m, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  // TODO: check that the tag is compatible with the actual type of the slot!
  switch (tag) {
    case JDWP::JT_BOOLEAN:
    case JDWP::JT_BYTE:
      CHECK_EQ(width, 1U);
      if (!visitor.SetVReg(m, vreg, static_cast<uint32_t>(value), kIntVReg)) {
        return FailSetLocalValue(visitor, vreg, tag, static_cast<uint32_t>(value));
      }
      break;
    case JDWP::JT_SHORT:
    case JDWP::JT_CHAR:
      CHECK_EQ(width, 2U);
      if (!visitor.SetVReg(m, vreg, static_cast<uint32_t>(value), kIntVReg)) {
        return FailSetLocalValue(visitor, vreg, tag, static_cast<uint32_t>(value));
      }
      break;
    case JDWP::JT_INT:
      CHECK_EQ(width, 4U);
      if (!visitor.SetVReg(m, vreg, static_cast<uint32_t>(value), kIntVReg)) {
        return FailSetLocalValue(visitor, vreg, tag, static_cast<uint32_t>(value));
      }
      break;
    case JDWP::JT_FLOAT:
      CHECK_EQ(width, 4U);
      if (!visitor.SetVReg(m, vreg, static_cast<uint32_t>(value), kFloatVReg)) {
        return FailSetLocalValue(visitor, vreg, tag, static_cast<uint32_t>(value));
      }
      break;
    case JDWP::JT_ARRAY:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP: {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      ObjPtr<mirror::Object> o =
          gRegistry->Get<mirror::Object>(static_cast<JDWP::ObjectId>(value), &error);
      if (error != JDWP::ERR_NONE) {
        VLOG(jdwp) << tag << " object " << o << " is an invalid object";
        return JDWP::ERR_INVALID_OBJECT;
      }
      if (!visitor.SetVRegReference(m, vreg, o)) {
        return FailSetLocalValue(visitor, vreg, tag, reinterpret_cast<uintptr_t>(o.Ptr()));
      }
      break;
    }
    case JDWP::JT_DOUBLE: {
      CHECK_EQ(width, 8U);
      if (!visitor.SetVRegPair(m, vreg, value, kDoubleLoVReg, kDoubleHiVReg)) {
        return FailSetLocalValue(visitor, vreg, tag, value);
      }
      break;
    }
    case JDWP::JT_LONG: {
      CHECK_EQ(width, 8U);
      if (!visitor.SetVRegPair(m, vreg, value, kLongLoVReg, kLongHiVReg)) {
        return FailSetLocalValue(visitor, vreg, tag, value);
      }
      break;
    }
    default:
      LOG(FATAL) << "Unknown tag " << tag;
      UNREACHABLE();
  }

  // If we set the local variable in a compiled frame, we need to trigger a deoptimization of
  // the stack so we continue execution with the interpreter using the new value(s) of the updated
  // local variable(s). To achieve this, we install instrumentation exit stub on each method of the
  // thread's stack. The stub will cause the deoptimization to happen.
  if (!visitor.IsShadowFrame() && thread->HasDebuggerShadowFrames()) {
    Runtime::Current()->GetInstrumentation()->InstrumentThreadStack(thread);
  }

  return JDWP::ERR_NONE;
}

static void SetEventLocation(JDWP::EventLocation* location, ArtMethod* m, uint32_t dex_pc)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(location != nullptr);
  if (m == nullptr) {
    memset(location, 0, sizeof(*location));
  } else {
    location->method = m->GetCanonicalMethod(kRuntimePointerSize);
    location->dex_pc = (m->IsNative() || m->IsProxyMethod()) ? static_cast<uint32_t>(-1) : dex_pc;
  }
}

void Dbg::PostLocationEvent(ArtMethod* m,
                            int dex_pc,
                            ObjPtr<mirror::Object> this_object,
                            int event_flags,
                            const JValue* return_value) {
  if (!IsDebuggerActive()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK_EQ(m->IsStatic(), this_object == nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);

  // We need to be sure no exception is pending when calling JdwpState::PostLocationEvent.
  // This is required to be able to call JNI functions to create JDWP ids. To achieve this,
  // we temporarily clear the current thread's exception (if any) and will restore it after
  // the call.
  // Note: the only way to get a pending exception here is to suspend on a move-exception
  // instruction.
  Thread* const self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Throwable> pending_exception(hs.NewHandle(self->GetException()));
  self->ClearException();
  if (kIsDebugBuild && pending_exception != nullptr) {
    const Instruction& instr = location.method->DexInstructions().InstructionAt(location.dex_pc);
    CHECK_EQ(Instruction::MOVE_EXCEPTION, instr.Opcode());
  }

  gJdwpState->PostLocationEvent(&location, this_object, event_flags, return_value);

  if (pending_exception != nullptr) {
    self->SetException(pending_exception.Get());
  }
}

void Dbg::PostFieldAccessEvent(ArtMethod* m,
                               int dex_pc,
                               ObjPtr<mirror::Object> this_object,
                               ArtField* f) {
  // TODO We should send events for native methods.
  if (!IsDebuggerActive() || m->IsNative()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK(f != nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);

  gJdwpState->PostFieldEvent(&location, f, this_object, nullptr, false);
}

void Dbg::PostFieldModificationEvent(ArtMethod* m,
                                     int dex_pc,
                                     ObjPtr<mirror::Object> this_object,
                                     ArtField* f,
                                     const JValue* field_value) {
  // TODO We should send events for native methods.
  if (!IsDebuggerActive() || m->IsNative()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK(f != nullptr);
  DCHECK(field_value != nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);

  gJdwpState->PostFieldEvent(&location, f, this_object, field_value, true);
}

void Dbg::PostException(ObjPtr<mirror::Throwable> exception_object) {
  if (!IsDebuggerActive()) {
    return;
  }
  Thread* const self = Thread::Current();
  StackHandleScope<2> handle_scope(self);
  Handle<mirror::Throwable> h_exception(handle_scope.NewHandle(exception_object));
  MutableHandle<mirror::Object> this_at_throw = handle_scope.NewHandle<mirror::Object>(nullptr);
  std::unique_ptr<Context> context(Context::Create());

  ArtMethod* catch_method = nullptr;
  ArtMethod* throw_method = nullptr;
  uint32_t catch_dex_pc = dex::kDexNoIndex;
  uint32_t throw_dex_pc = dex::kDexNoIndex;
  StackVisitor::WalkStack(
      /**
       * Finds the location where this exception will be caught. We search until we reach the top
       * frame, in which case this exception is considered uncaught.
       */
      [&](const art::StackVisitor* stack_visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        ArtMethod* method = stack_visitor->GetMethod();
        DCHECK(method != nullptr);
        if (method->IsRuntimeMethod()) {
          // Ignore callee save method.
          DCHECK(method->IsCalleeSaveMethod());
          return true;
        }

        uint32_t dex_pc = stack_visitor->GetDexPc();
        if (throw_method == nullptr) {
          // First Java method found. It is either the method that threw the exception,
          // or the Java native method that is reporting an exception thrown by
          // native code.
          this_at_throw.Assign(stack_visitor->GetThisObject());
          throw_method = method;
          throw_dex_pc = dex_pc;
        }

        if (dex_pc != dex::kDexNoIndex) {
          StackHandleScope<1> hs(stack_visitor->GetThread());
          uint32_t found_dex_pc;
          Handle<mirror::Class> exception_class(hs.NewHandle(h_exception->GetClass()));
          bool unused_clear_exception;
          found_dex_pc = method->FindCatchBlock(exception_class, dex_pc, &unused_clear_exception);
          if (found_dex_pc != dex::kDexNoIndex) {
            catch_method = method;
            catch_dex_pc = found_dex_pc;
            return false;  // End stack walk.
          }
        }
        return true;  // Continue stack walk.
      },
      self,
      context.get(),
      art::StackVisitor::StackWalkKind::kIncludeInlinedFrames);

  JDWP::EventLocation exception_throw_location;
  SetEventLocation(&exception_throw_location, throw_method, throw_dex_pc);
  JDWP::EventLocation exception_catch_location;
  SetEventLocation(&exception_catch_location, catch_method, catch_dex_pc);

  gJdwpState->PostException(&exception_throw_location,
                            h_exception.Get(),
                            &exception_catch_location,
                            this_at_throw.Get());
}

void Dbg::PostClassPrepare(ObjPtr<mirror::Class> c) {
  if (!IsDebuggerActive()) {
    return;
  }
  gJdwpState->PostClassPrepare(c);
}

void Dbg::UpdateDebugger(Thread* thread,
                         ObjPtr<mirror::Object> this_object,
                         ArtMethod* m,
                         uint32_t dex_pc,
                         int event_flags,
                         const JValue* return_value) {
  if (!IsDebuggerActive() || dex_pc == static_cast<uint32_t>(-2) /* fake method exit */) {
    return;
  }

  if (IsBreakpoint(m, dex_pc)) {
    event_flags |= kBreakpoint;
  }

  // If the debugger is single-stepping one of our threads, check to
  // see if we're that thread and we've reached a step point.
  const SingleStepControl* single_step_control = thread->GetSingleStepControl();
  if (single_step_control != nullptr) {
    CHECK(!m->IsNative());
    if (single_step_control->GetStepDepth() == JDWP::SD_INTO) {
      // Step into method calls.  We break when the line number
      // or method pointer changes.  If we're in SS_MIN mode, we
      // always stop.
      if (single_step_control->GetMethod() != m) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new method";
      } else if (single_step_control->GetStepSize() == JDWP::SS_MIN) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new instruction";
      } else if (single_step_control->ContainsDexPc(dex_pc)) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new line";
      }
    } else if (single_step_control->GetStepDepth() == JDWP::SD_OVER) {
      // Step over method calls.  We break when the line number is
      // different and the frame depth is <= the original frame
      // depth.  (We can't just compare on the method, because we
      // might get unrolled past it by an exception, and it's tricky
      // to identify recursion.)

      int stack_depth = GetStackDepth(thread);

      if (stack_depth < single_step_control->GetStackDepth()) {
        // Popped up one or more frames, always trigger.
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS method pop";
      } else if (stack_depth == single_step_control->GetStackDepth()) {
        // Same depth, see if we moved.
        if (single_step_control->GetStepSize() == JDWP::SS_MIN) {
          event_flags |= kSingleStep;
          VLOG(jdwp) << "SS new instruction";
        } else if (single_step_control->ContainsDexPc(dex_pc)) {
          event_flags |= kSingleStep;
          VLOG(jdwp) << "SS new line";
        }
      }
    } else {
      CHECK_EQ(single_step_control->GetStepDepth(), JDWP::SD_OUT);
      // Return from the current method.  We break when the frame
      // depth pops up.

      // This differs from the "method exit" break in that it stops
      // with the PC at the next instruction in the returned-to
      // function, rather than the end of the returning function.

      int stack_depth = GetStackDepth(thread);
      if (stack_depth < single_step_control->GetStackDepth()) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS method pop";
      }
    }
  }

  // If there's something interesting going on, see if it matches one
  // of the debugger filters.
  if (event_flags != 0) {
    Dbg::PostLocationEvent(m, dex_pc, this_object, event_flags, return_value);
  }
}

size_t* Dbg::GetReferenceCounterForEvent(uint32_t instrumentation_event) {
  switch (instrumentation_event) {
    case instrumentation::Instrumentation::kMethodEntered:
      return &method_enter_event_ref_count_;
    case instrumentation::Instrumentation::kMethodExited:
      return &method_exit_event_ref_count_;
    case instrumentation::Instrumentation::kDexPcMoved:
      return &dex_pc_change_event_ref_count_;
    case instrumentation::Instrumentation::kFieldRead:
      return &field_read_event_ref_count_;
    case instrumentation::Instrumentation::kFieldWritten:
      return &field_write_event_ref_count_;
    case instrumentation::Instrumentation::kExceptionThrown:
      return &exception_catch_event_ref_count_;
    default:
      return nullptr;
  }
}

// Process request while all mutator threads are suspended.
void Dbg::ProcessDeoptimizationRequest(const DeoptimizationRequest& request) {
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  switch (request.GetKind()) {
    case DeoptimizationRequest::kNothing:
      LOG(WARNING) << "Ignoring empty deoptimization request.";
      break;
    case DeoptimizationRequest::kRegisterForEvent:
      VLOG(jdwp) << StringPrintf("Add debugger as listener for instrumentation event 0x%x",
                                 request.InstrumentationEvent());
      instrumentation->AddListener(&gDebugInstrumentationListener, request.InstrumentationEvent());
      instrumentation_events_ |= request.InstrumentationEvent();
      break;
    case DeoptimizationRequest::kUnregisterForEvent:
      VLOG(jdwp) << StringPrintf("Remove debugger as listener for instrumentation event 0x%x",
                                 request.InstrumentationEvent());
      instrumentation->RemoveListener(&gDebugInstrumentationListener,
                                      request.InstrumentationEvent());
      instrumentation_events_ &= ~request.InstrumentationEvent();
      break;
    case DeoptimizationRequest::kFullDeoptimization:
      VLOG(jdwp) << "Deoptimize the world ...";
      instrumentation->DeoptimizeEverything(kDbgInstrumentationKey);
      VLOG(jdwp) << "Deoptimize the world DONE";
      break;
    case DeoptimizationRequest::kFullUndeoptimization:
      VLOG(jdwp) << "Undeoptimize the world ...";
      instrumentation->UndeoptimizeEverything(kDbgInstrumentationKey);
      VLOG(jdwp) << "Undeoptimize the world DONE";
      break;
    case DeoptimizationRequest::kSelectiveDeoptimization:
      VLOG(jdwp) << "Deoptimize method " << ArtMethod::PrettyMethod(request.Method()) << " ...";
      instrumentation->Deoptimize(request.Method());
      VLOG(jdwp) << "Deoptimize method " << ArtMethod::PrettyMethod(request.Method()) << " DONE";
      break;
    case DeoptimizationRequest::kSelectiveUndeoptimization:
      VLOG(jdwp) << "Undeoptimize method " << ArtMethod::PrettyMethod(request.Method()) << " ...";
      instrumentation->Undeoptimize(request.Method());
      VLOG(jdwp) << "Undeoptimize method " << ArtMethod::PrettyMethod(request.Method()) << " DONE";
      break;
    default:
      LOG(FATAL) << "Unsupported deoptimization request kind " << request.GetKind();
      UNREACHABLE();
  }
}

void Dbg::RequestDeoptimization(const DeoptimizationRequest& req) {
  if (req.GetKind() == DeoptimizationRequest::kNothing) {
    // Nothing to do.
    return;
  }
  MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
  RequestDeoptimizationLocked(req);
}

void Dbg::RequestDeoptimizationLocked(const DeoptimizationRequest& req) {
  switch (req.GetKind()) {
    case DeoptimizationRequest::kRegisterForEvent: {
      DCHECK_NE(req.InstrumentationEvent(), 0u);
      size_t* counter = GetReferenceCounterForEvent(req.InstrumentationEvent());
      CHECK(counter != nullptr) << StringPrintf("No counter for instrumentation event 0x%x",
                                                req.InstrumentationEvent());
      if (*counter == 0) {
        VLOG(jdwp) << StringPrintf("Queue request #%zd to start listening to instrumentation event 0x%x",
                                   deoptimization_requests_.size(), req.InstrumentationEvent());
        deoptimization_requests_.push_back(req);
      }
      *counter = *counter + 1;
      break;
    }
    case DeoptimizationRequest::kUnregisterForEvent: {
      DCHECK_NE(req.InstrumentationEvent(), 0u);
      size_t* counter = GetReferenceCounterForEvent(req.InstrumentationEvent());
      CHECK(counter != nullptr) << StringPrintf("No counter for instrumentation event 0x%x",
                                                req.InstrumentationEvent());
      *counter = *counter - 1;
      if (*counter == 0) {
        VLOG(jdwp) << StringPrintf("Queue request #%zd to stop listening to instrumentation event 0x%x",
                                   deoptimization_requests_.size(), req.InstrumentationEvent());
        deoptimization_requests_.push_back(req);
      }
      break;
    }
    case DeoptimizationRequest::kFullDeoptimization: {
      DCHECK(req.Method() == nullptr);
      if (full_deoptimization_event_count_ == 0) {
        VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                   << " for full deoptimization";
        deoptimization_requests_.push_back(req);
      }
      ++full_deoptimization_event_count_;
      break;
    }
    case DeoptimizationRequest::kFullUndeoptimization: {
      DCHECK(req.Method() == nullptr);
      DCHECK_GT(full_deoptimization_event_count_, 0U);
      --full_deoptimization_event_count_;
      if (full_deoptimization_event_count_ == 0) {
        VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                   << " for full undeoptimization";
        deoptimization_requests_.push_back(req);
      }
      break;
    }
    case DeoptimizationRequest::kSelectiveDeoptimization: {
      DCHECK(req.Method() != nullptr);
      VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                 << " for deoptimization of " << req.Method()->PrettyMethod();
      deoptimization_requests_.push_back(req);
      break;
    }
    case DeoptimizationRequest::kSelectiveUndeoptimization: {
      DCHECK(req.Method() != nullptr);
      VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                 << " for undeoptimization of " << req.Method()->PrettyMethod();
      deoptimization_requests_.push_back(req);
      break;
    }
    default: {
      LOG(FATAL) << "Unknown deoptimization request kind " << req.GetKind();
      UNREACHABLE();
    }
  }
}

void Dbg::ManageDeoptimization() {
  Thread* const self = Thread::Current();
  {
    // Avoid suspend/resume if there is no pending request.
    MutexLock mu(self, *Locks::deoptimization_lock_);
    if (deoptimization_requests_.empty()) {
      return;
    }
  }
  CHECK_EQ(self->GetState(), kRunnable);
  ScopedThreadSuspension sts(self, kWaitingForDeoptimization);
  // Required for ProcessDeoptimizationRequest.
  gc::ScopedGCCriticalSection gcs(self,
                                  gc::kGcCauseInstrumentation,
                                  gc::kCollectorTypeInstrumentation);
  // We need to suspend mutator threads first.
  ScopedSuspendAll ssa(__FUNCTION__);
  const ThreadState old_state = self->SetStateUnsafe(kRunnable);
  {
    MutexLock mu(self, *Locks::deoptimization_lock_);
    size_t req_index = 0;
    for (DeoptimizationRequest& request : deoptimization_requests_) {
      VLOG(jdwp) << "Process deoptimization request #" << req_index++;
      ProcessDeoptimizationRequest(request);
    }
    deoptimization_requests_.clear();
  }
  CHECK_EQ(self->SetStateUnsafe(old_state), kRunnable);
}

static const Breakpoint* FindFirstBreakpointForMethod(ArtMethod* m)
    REQUIRES_SHARED(Locks::mutator_lock_, Locks::breakpoint_lock_) {
  for (Breakpoint& breakpoint : gBreakpoints) {
    if (breakpoint.IsInMethod(m)) {
      return &breakpoint;
    }
  }
  return nullptr;
}

bool Dbg::MethodHasAnyBreakpoints(ArtMethod* method) {
  ReaderMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
  return FindFirstBreakpointForMethod(method) != nullptr;
}

// Sanity checks all existing breakpoints on the same method.
static void SanityCheckExistingBreakpoints(ArtMethod* m,
                                           DeoptimizationRequest::Kind deoptimization_kind)
    REQUIRES_SHARED(Locks::mutator_lock_, Locks::breakpoint_lock_) {
  for (const Breakpoint& breakpoint : gBreakpoints) {
    if (breakpoint.IsInMethod(m)) {
      CHECK_EQ(deoptimization_kind, breakpoint.GetDeoptimizationKind());
    }
  }
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (deoptimization_kind == DeoptimizationRequest::kFullDeoptimization) {
    // We should have deoptimized everything but not "selectively" deoptimized this method.
    CHECK(instrumentation->AreAllMethodsDeoptimized());
    CHECK(!instrumentation->IsDeoptimized(m));
  } else if (deoptimization_kind == DeoptimizationRequest::kSelectiveDeoptimization) {
    // We should have "selectively" deoptimized this method.
    // Note: while we have not deoptimized everything for this method, we may have done it for
    // another event.
    CHECK(instrumentation->IsDeoptimized(m));
  } else {
    // This method does not require deoptimization.
    CHECK_EQ(deoptimization_kind, DeoptimizationRequest::kNothing);
    CHECK(!instrumentation->IsDeoptimized(m));
  }
}

// Returns the deoptimization kind required to set a breakpoint in a method.
// If a breakpoint has already been set, we also return the first breakpoint
// through the given 'existing_brkpt' pointer.
static DeoptimizationRequest::Kind GetRequiredDeoptimizationKind(Thread* self,
                                                                 ArtMethod* m,
                                                                 const Breakpoint** existing_brkpt)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!Dbg::RequiresDeoptimization()) {
    // We already run in interpreter-only mode so we don't need to deoptimize anything.
    VLOG(jdwp) << "No need for deoptimization when fully running with interpreter for method "
               << ArtMethod::PrettyMethod(m);
    return DeoptimizationRequest::kNothing;
  }
  const Breakpoint* first_breakpoint;
  {
    ReaderMutexLock mu(self, *Locks::breakpoint_lock_);
    first_breakpoint = FindFirstBreakpointForMethod(m);
    *existing_brkpt = first_breakpoint;
  }

  if (first_breakpoint == nullptr) {
    // There is no breakpoint on this method yet: we need to deoptimize. If this method is default,
    // we deoptimize everything; otherwise we deoptimize only this method. We
    // deoptimize with defaults because we do not know everywhere they are used. It is possible some
    // of the copies could be missed.
    // TODO Deoptimizing on default methods might not be necessary in all cases.
    bool need_full_deoptimization = m->IsDefault();
    if (need_full_deoptimization) {
      VLOG(jdwp) << "Need full deoptimization because of copying of method "
                 << ArtMethod::PrettyMethod(m);
      return DeoptimizationRequest::kFullDeoptimization;
    } else {
      // We don't need to deoptimize if the method has not been compiled.
      const bool is_compiled = m->HasAnyCompiledCode();
      if (is_compiled) {
        VLOG(jdwp) << "Need selective deoptimization for compiled method "
                   << ArtMethod::PrettyMethod(m);
        return DeoptimizationRequest::kSelectiveDeoptimization;
      } else {
        // Method is not compiled: we don't need to deoptimize.
        VLOG(jdwp) << "No need for deoptimization for non-compiled method "
                   << ArtMethod::PrettyMethod(m);
        return DeoptimizationRequest::kNothing;
      }
    }
  } else {
    // There is at least one breakpoint for this method: we don't need to deoptimize.
    // Let's check that all breakpoints are configured the same way for deoptimization.
    VLOG(jdwp) << "Breakpoint already set: no deoptimization is required";
    DeoptimizationRequest::Kind deoptimization_kind = first_breakpoint->GetDeoptimizationKind();
    if (kIsDebugBuild) {
      ReaderMutexLock mu(self, *Locks::breakpoint_lock_);
      SanityCheckExistingBreakpoints(m, deoptimization_kind);
    }
    return DeoptimizationRequest::kNothing;
  }
}

// Installs a breakpoint at the specified location. Also indicates through the deoptimization
// request if we need to deoptimize.
void Dbg::WatchLocation(const JDWP::JdwpLocation* location, DeoptimizationRequest* req) {
  Thread* const self = Thread::Current();
  ArtMethod* m = FromMethodId(location->method_id);
  DCHECK(m != nullptr) << "No method for method id " << location->method_id;

  const Breakpoint* existing_breakpoint = nullptr;
  const DeoptimizationRequest::Kind deoptimization_kind =
      GetRequiredDeoptimizationKind(self, m, &existing_breakpoint);
  req->SetKind(deoptimization_kind);
  if (deoptimization_kind == DeoptimizationRequest::kSelectiveDeoptimization) {
    req->SetMethod(m);
  } else {
    CHECK(deoptimization_kind == DeoptimizationRequest::kNothing ||
          deoptimization_kind == DeoptimizationRequest::kFullDeoptimization);
    req->SetMethod(nullptr);
  }

  {
    WriterMutexLock mu(self, *Locks::breakpoint_lock_);
    // If there is at least one existing breakpoint on the same method, the new breakpoint
    // must have the same deoptimization kind than the existing breakpoint(s).
    DeoptimizationRequest::Kind breakpoint_deoptimization_kind;
    if (existing_breakpoint != nullptr) {
      breakpoint_deoptimization_kind = existing_breakpoint->GetDeoptimizationKind();
    } else {
      breakpoint_deoptimization_kind = deoptimization_kind;
    }
    gBreakpoints.push_back(Breakpoint(m, location->dex_pc, breakpoint_deoptimization_kind));
    VLOG(jdwp) << "Set breakpoint #" << (gBreakpoints.size() - 1) << ": "
               << gBreakpoints[gBreakpoints.size() - 1];
  }
}

// Uninstalls a breakpoint at the specified location. Also indicates through the deoptimization
// request if we need to undeoptimize.
void Dbg::UnwatchLocation(const JDWP::JdwpLocation* location, DeoptimizationRequest* req) {
  WriterMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
  ArtMethod* m = FromMethodId(location->method_id);
  DCHECK(m != nullptr) << "No method for method id " << location->method_id;
  DeoptimizationRequest::Kind deoptimization_kind = DeoptimizationRequest::kNothing;
  for (size_t i = 0, e = gBreakpoints.size(); i < e; ++i) {
    if (gBreakpoints[i].DexPc() == location->dex_pc && gBreakpoints[i].IsInMethod(m)) {
      VLOG(jdwp) << "Removed breakpoint #" << i << ": " << gBreakpoints[i];
      deoptimization_kind = gBreakpoints[i].GetDeoptimizationKind();
      DCHECK_EQ(deoptimization_kind == DeoptimizationRequest::kSelectiveDeoptimization,
                Runtime::Current()->GetInstrumentation()->IsDeoptimized(m));
      gBreakpoints.erase(gBreakpoints.begin() + i);
      break;
    }
  }
  const Breakpoint* const existing_breakpoint = FindFirstBreakpointForMethod(m);
  if (existing_breakpoint == nullptr) {
    // There is no more breakpoint on this method: we need to undeoptimize.
    if (deoptimization_kind == DeoptimizationRequest::kFullDeoptimization) {
      // This method required full deoptimization: we need to undeoptimize everything.
      req->SetKind(DeoptimizationRequest::kFullUndeoptimization);
      req->SetMethod(nullptr);
    } else if (deoptimization_kind == DeoptimizationRequest::kSelectiveDeoptimization) {
      // This method required selective deoptimization: we need to undeoptimize only that method.
      req->SetKind(DeoptimizationRequest::kSelectiveUndeoptimization);
      req->SetMethod(m);
    } else {
      // This method had no need for deoptimization: do nothing.
      CHECK_EQ(deoptimization_kind, DeoptimizationRequest::kNothing);
      req->SetKind(DeoptimizationRequest::kNothing);
      req->SetMethod(nullptr);
    }
  } else {
    // There is at least one breakpoint for this method: we don't need to undeoptimize.
    req->SetKind(DeoptimizationRequest::kNothing);
    req->SetMethod(nullptr);
    if (kIsDebugBuild) {
      SanityCheckExistingBreakpoints(m, deoptimization_kind);
    }
  }
}

bool Dbg::IsForcedInterpreterNeededForCallingImpl(Thread* thread, ArtMethod* m) {
  const SingleStepControl* const ssc = thread->GetSingleStepControl();
  if (ssc == nullptr) {
    // If we are not single-stepping, then we don't have to force interpreter.
    return false;
  }
  if (Runtime::Current()->GetInstrumentation()->InterpretOnly()) {
    // If we are in interpreter only mode, then we don't have to force interpreter.
    return false;
  }

  if (!m->IsNative() && !m->IsProxyMethod()) {
    // If we want to step into a method, then we have to force interpreter on that call.
    if (ssc->GetStepDepth() == JDWP::SD_INTO) {
      return true;
    }
  }
  return false;
}

bool Dbg::IsForcedInterpreterNeededForResolutionImpl(Thread* thread, ArtMethod* m) {
  instrumentation::Instrumentation* const instrumentation =
      Runtime::Current()->GetInstrumentation();
  // If we are in interpreter only mode, then we don't have to force interpreter.
  if (instrumentation->InterpretOnly()) {
    return false;
  }
  // We can only interpret pure Java method.
  if (m->IsNative() || m->IsProxyMethod()) {
    return false;
  }
  const SingleStepControl* const ssc = thread->GetSingleStepControl();
  if (ssc != nullptr) {
    // If we want to step into a method, then we have to force interpreter on that call.
    if (ssc->GetStepDepth() == JDWP::SD_INTO) {
      return true;
    }
    // If we are stepping out from a static initializer, by issuing a step
    // in or step over, that was implicitly invoked by calling a static method,
    // then we need to step into that method. Having a lower stack depth than
    // the one the single step control has indicates that the step originates
    // from the static initializer.
    if (ssc->GetStepDepth() != JDWP::SD_OUT &&
        ssc->GetStackDepth() > GetStackDepth(thread)) {
      return true;
    }
  }
  // There are cases where we have to force interpreter on deoptimized methods,
  // because in some cases the call will not be performed by invoking an entry
  // point that has been replaced by the deoptimization, but instead by directly
  // invoking the compiled code of the method, for example.
  return instrumentation->IsDeoptimized(m);
}

bool Dbg::IsForcedInstrumentationNeededForResolutionImpl(Thread* thread, ArtMethod* m) {
  // The upcall can be null and in that case we don't need to do anything.
  if (m == nullptr) {
    return false;
  }
  instrumentation::Instrumentation* const instrumentation =
      Runtime::Current()->GetInstrumentation();
  // If we are in interpreter only mode, then we don't have to force interpreter.
  if (instrumentation->InterpretOnly()) {
    return false;
  }
  // We can only interpret pure Java method.
  if (m->IsNative() || m->IsProxyMethod()) {
    return false;
  }
  const SingleStepControl* const ssc = thread->GetSingleStepControl();
  if (ssc != nullptr) {
    // If we are stepping out from a static initializer, by issuing a step
    // out, that was implicitly invoked by calling a static method, then we
    // need to step into the caller of that method. Having a lower stack
    // depth than the one the single step control has indicates that the
    // step originates from the static initializer.
    if (ssc->GetStepDepth() == JDWP::SD_OUT &&
        ssc->GetStackDepth() > GetStackDepth(thread)) {
      return true;
    }
  }
  // If we are returning from a static intializer, that was implicitly
  // invoked by calling a static method and the caller is deoptimized,
  // then we have to deoptimize the stack without forcing interpreter
  // on the static method that was called originally. This problem can
  // be solved easily by forcing instrumentation on the called method,
  // because the instrumentation exit hook will recognise the need of
  // stack deoptimization by calling IsForcedInterpreterNeededForUpcall.
  return instrumentation->IsDeoptimized(m);
}

bool Dbg::IsForcedInterpreterNeededForUpcallImpl(Thread* thread, ArtMethod* m) {
  // The upcall can be null and in that case we don't need to do anything.
  if (m == nullptr) {
    return false;
  }
  instrumentation::Instrumentation* const instrumentation =
      Runtime::Current()->GetInstrumentation();
  // If we are in interpreter only mode, then we don't have to force interpreter.
  if (instrumentation->InterpretOnly()) {
    return false;
  }
  // We can only interpret pure Java method.
  if (m->IsNative() || m->IsProxyMethod()) {
    return false;
  }
  const SingleStepControl* const ssc = thread->GetSingleStepControl();
  if (ssc != nullptr) {
    // The debugger is not interested in what is happening under the level
    // of the step, thus we only force interpreter when we are not below of
    // the step.
    if (ssc->GetStackDepth() >= GetStackDepth(thread)) {
      return true;
    }
  }
  if (thread->HasDebuggerShadowFrames()) {
    // We need to deoptimize the stack for the exception handling flow so that
    // we don't miss any deoptimization that should be done when there are
    // debugger shadow frames.
    return true;
  }
  // We have to require stack deoptimization if the upcall is deoptimized.
  return instrumentation->IsDeoptimized(m);
}

// Do we need to deoptimize the stack to handle an exception?
bool Dbg::IsForcedInterpreterNeededForExceptionImpl(Thread* thread) {
  const SingleStepControl* const ssc = thread->GetSingleStepControl();
  if (ssc != nullptr) {
    // We deopt to step into the catch handler.
    return true;
  }
  // Deoptimization is required if at least one method in the stack needs it. However we
  // skip frames that will be unwound (thus not executed).
  bool needs_deoptimization = false;
  StackVisitor::WalkStack(
      [&](art::StackVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        // The visitor is meant to be used when handling exception from compiled code only.
        CHECK(!visitor->IsShadowFrame()) << "We only expect to visit compiled frame: "
                                         << ArtMethod::PrettyMethod(visitor->GetMethod());
        ArtMethod* method = visitor->GetMethod();
        if (method == nullptr) {
          // We reach an upcall and don't need to deoptimize this part of the stack (ManagedFragment)
          // so we can stop the visit.
          DCHECK(!needs_deoptimization);
          return false;
        }
        if (Runtime::Current()->GetInstrumentation()->InterpretOnly()) {
          // We found a compiled frame in the stack but instrumentation is set to interpret
          // everything: we need to deoptimize.
          needs_deoptimization = true;
          return false;
        }
        if (Runtime::Current()->GetInstrumentation()->IsDeoptimized(method)) {
          // We found a deoptimized method in the stack.
          needs_deoptimization = true;
          return false;
        }
        ShadowFrame* frame = visitor->GetThread()->FindDebuggerShadowFrame(visitor->GetFrameId());
        if (frame != nullptr) {
          // The debugger allocated a ShadowFrame to update a variable in the stack: we need to
          // deoptimize the stack to execute (and deallocate) this frame.
          needs_deoptimization = true;
          return false;
        }
        return true;
      },
      thread,
      /* context= */ nullptr,
      art::StackVisitor::StackWalkKind::kIncludeInlinedFrames,
      /* check_suspended */ true,
      /* include_transitions */ true);
  return needs_deoptimization;
}

// Scoped utility class to suspend a thread so that we may do tasks such as walk its stack. Doesn't
// cause suspension if the thread is the current thread.
class ScopedDebuggerThreadSuspension {
 public:
  ScopedDebuggerThreadSuspension(Thread* self, JDWP::ObjectId thread_id)
      REQUIRES(!Locks::thread_list_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_) :
      thread_(nullptr),
      error_(JDWP::ERR_NONE),
      self_suspend_(false),
      other_suspend_(false) {
    ScopedObjectAccessUnchecked soa(self);
    thread_ = DecodeThread(soa, thread_id, &error_);
    if (error_ == JDWP::ERR_NONE) {
      if (thread_ == soa.Self()) {
        self_suspend_ = true;
      } else {
        Thread* suspended_thread;
        {
          ScopedThreadSuspension sts(self, kWaitingForDebuggerSuspension);
          jobject thread_peer = Dbg::GetObjectRegistry()->GetJObject(thread_id);
          bool timed_out;
          ThreadList* const thread_list = Runtime::Current()->GetThreadList();
          suspended_thread = thread_list->SuspendThreadByPeer(thread_peer,
                                                              /* request_suspension= */ true,
                                                              SuspendReason::kForDebugger,
                                                              &timed_out);
        }
        if (suspended_thread == nullptr) {
          // Thread terminated from under us while suspending.
          error_ = JDWP::ERR_INVALID_THREAD;
        } else {
          CHECK_EQ(suspended_thread, thread_);
          other_suspend_ = true;
        }
      }
    }
  }

  Thread* GetThread() const {
    return thread_;
  }

  JDWP::JdwpError GetError() const {
    return error_;
  }

  ~ScopedDebuggerThreadSuspension() {
    if (other_suspend_) {
      bool resumed = Runtime::Current()->GetThreadList()->Resume(thread_,
                                                                 SuspendReason::kForDebugger);
      DCHECK(resumed);
    }
  }

 private:
  Thread* thread_;
  JDWP::JdwpError error_;
  bool self_suspend_;
  bool other_suspend_;
};

JDWP::JdwpError Dbg::ConfigureStep(JDWP::ObjectId thread_id, JDWP::JdwpStepSize step_size,
                                   JDWP::JdwpStepDepth step_depth) {
  Thread* self = Thread::Current();
  ScopedDebuggerThreadSuspension sts(self, thread_id);
  if (sts.GetError() != JDWP::ERR_NONE) {
    return sts.GetError();
  }

  // Work out what ArtMethod* we're in, the current line number, and how deep the stack currently
  // is for step-out.
  struct SingleStepStackVisitor : public StackVisitor {
    explicit SingleStepStackVisitor(Thread* thread) REQUIRES_SHARED(Locks::mutator_lock_)
        : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
          stack_depth(0),
          method(nullptr),
          line_number(-1) {}

    // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
    // annotalysis.
    bool VisitFrame() override NO_THREAD_SAFETY_ANALYSIS {
      ArtMethod* m = GetMethod();
      if (!m->IsRuntimeMethod()) {
        ++stack_depth;
        if (method == nullptr) {
          const DexFile* dex_file = m->GetDexFile();
          method = m;
          if (dex_file != nullptr) {
            line_number = annotations::GetLineNumFromPC(dex_file, m, GetDexPc());
          }
        }
      }
      return true;
    }

    int stack_depth;
    ArtMethod* method;
    int32_t line_number;
  };

  Thread* const thread = sts.GetThread();
  SingleStepStackVisitor visitor(thread);
  visitor.WalkStack();

  // Allocate single step.
  SingleStepControl* single_step_control =
      new (std::nothrow) SingleStepControl(step_size, step_depth,
                                           visitor.stack_depth, visitor.method);
  if (single_step_control == nullptr) {
    LOG(ERROR) << "Failed to allocate SingleStepControl";
    return JDWP::ERR_OUT_OF_MEMORY;
  }

  ArtMethod* m = single_step_control->GetMethod();
  const int32_t line_number = visitor.line_number;
  // Note: if the thread is not running Java code (pure native thread), there is no "current"
  // method on the stack (and no line number either).
  if (m != nullptr && !m->IsNative()) {
    CodeItemDebugInfoAccessor accessor(m->DexInstructionDebugInfo());
    bool last_pc_valid = false;
    uint32_t last_pc = 0u;
    // Find the dex_pc values that correspond to the current line, for line-based single-stepping.
    accessor.DecodeDebugPositionInfo([&](const DexFile::PositionInfo& entry) {
      if (static_cast<int32_t>(entry.line_) == line_number) {
        if (!last_pc_valid) {
          // Everything from this address until the next line change is ours.
          last_pc = entry.address_;
          last_pc_valid = true;
        }
        // Otherwise, if we're already in a valid range for this line,
        // just keep going (shouldn't really happen)...
      } else if (last_pc_valid) {  // and the line number is new
        // Add everything from the last entry up until here to the set
        for (uint32_t dex_pc = last_pc; dex_pc < entry.address_; ++dex_pc) {
          single_step_control->AddDexPc(dex_pc);
        }
        last_pc_valid = false;
      }
      return false;  // There may be multiple entries for any given line.
    });
    // If the line number was the last in the position table...
    if (last_pc_valid) {
      for (uint32_t dex_pc = last_pc; dex_pc < accessor.InsnsSizeInCodeUnits(); ++dex_pc) {
        single_step_control->AddDexPc(dex_pc);
      }
    }
  }

  // Activate single-step in the thread.
  thread->ActivateSingleStepControl(single_step_control);

  if (VLOG_IS_ON(jdwp)) {
    VLOG(jdwp) << "Single-step thread: " << *thread;
    VLOG(jdwp) << "Single-step step size: " << single_step_control->GetStepSize();
    VLOG(jdwp) << "Single-step step depth: " << single_step_control->GetStepDepth();
    VLOG(jdwp) << "Single-step current method: "
               << ArtMethod::PrettyMethod(single_step_control->GetMethod());
    VLOG(jdwp) << "Single-step current line: " << line_number;
    VLOG(jdwp) << "Single-step current stack depth: " << single_step_control->GetStackDepth();
    VLOG(jdwp) << "Single-step dex_pc values:";
    for (uint32_t dex_pc : single_step_control->GetDexPcs()) {
      VLOG(jdwp) << StringPrintf(" %#x", dex_pc);
    }
  }

  return JDWP::ERR_NONE;
}

void Dbg::UnconfigureStep(JDWP::ObjectId thread_id) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error == JDWP::ERR_NONE) {
    thread->DeactivateSingleStepControl();
  }
}

static char JdwpTagToShortyChar(JDWP::JdwpTag tag) {
  switch (tag) {
    default:
      LOG(FATAL) << "unknown JDWP tag: " << PrintableChar(tag);
      UNREACHABLE();

    // Primitives.
    case JDWP::JT_BYTE:    return 'B';
    case JDWP::JT_CHAR:    return 'C';
    case JDWP::JT_FLOAT:   return 'F';
    case JDWP::JT_DOUBLE:  return 'D';
    case JDWP::JT_INT:     return 'I';
    case JDWP::JT_LONG:    return 'J';
    case JDWP::JT_SHORT:   return 'S';
    case JDWP::JT_VOID:    return 'V';
    case JDWP::JT_BOOLEAN: return 'Z';

    // Reference types.
    case JDWP::JT_ARRAY:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
      return 'L';
  }
}

JDWP::JdwpError Dbg::PrepareInvokeMethod(uint32_t request_id, JDWP::ObjectId thread_id,
                                         JDWP::ObjectId object_id, JDWP::RefTypeId class_id,
                                         JDWP::MethodId method_id, uint32_t arg_count,
                                         uint64_t arg_values[], JDWP::JdwpTag* arg_types,
                                         uint32_t options) {
  Thread* const self = Thread::Current();
  CHECK_EQ(self, GetDebugThread()) << "This must be called by the JDWP thread";
  const bool resume_all_threads = ((options & JDWP::INVOKE_SINGLE_THREADED) == 0);

  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  Thread* targetThread = nullptr;
  {
    ScopedObjectAccessUnchecked soa(self);
    JDWP::JdwpError error;
    targetThread = DecodeThread(soa, thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      LOG(ERROR) << "InvokeMethod request for invalid thread id " << thread_id;
      return error;
    }
    if (targetThread->GetInvokeReq() != nullptr) {
      // Thread is already invoking a method on behalf of the debugger.
      LOG(ERROR) << "InvokeMethod request for thread already invoking a method: " << *targetThread;
      return JDWP::ERR_ALREADY_INVOKING;
    }
    if (!targetThread->IsReadyForDebugInvoke()) {
      // Thread is not suspended by an event so it cannot invoke a method.
      LOG(ERROR) << "InvokeMethod request for thread not stopped by event: " << *targetThread;
      return JDWP::ERR_INVALID_THREAD;
    }

    /*
     * According to the JDWP specs, we are expected to resume all threads (or only the
     * target thread) once. So if a thread has been suspended more than once (either by
     * the debugger for an event or by the runtime for GC), it will remain suspended before
     * the invoke is executed. This means the debugger is responsible to properly resume all
     * the threads it has suspended so the target thread can execute the method.
     *
     * However, for compatibility reason with older versions of debuggers (like Eclipse), we
     * fully resume all threads (by canceling *all* debugger suspensions) when the debugger
     * wants us to resume all threads. This is to avoid ending up in deadlock situation.
     *
     * On the other hand, if we are asked to only resume the target thread, then we follow the
     * JDWP specs by resuming that thread only once. This means the thread will remain suspended
     * if it has been suspended more than once before the invoke (and again, this is the
     * responsibility of the debugger to properly resume that thread before invoking a method).
     */
    int suspend_count;
    {
      MutexLock mu2(soa.Self(), *Locks::thread_suspend_count_lock_);
      suspend_count = targetThread->GetSuspendCount();
    }
    if (suspend_count > 1 && resume_all_threads) {
      // The target thread will remain suspended even after we resume it. Let's emit a warning
      // to indicate the invoke won't be executed until the thread is resumed.
      LOG(WARNING) << *targetThread << " suspended more than once (suspend count == "
                   << suspend_count << "). This thread will invoke the method only once "
                   << "it is fully resumed.";
    }

    ObjPtr<mirror::Object> receiver = gRegistry->Get<mirror::Object>(object_id, &error);
    if (error != JDWP::ERR_NONE) {
      return JDWP::ERR_INVALID_OBJECT;
    }

    gRegistry->Get<mirror::Object>(thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      return JDWP::ERR_INVALID_OBJECT;
    }

    ObjPtr<mirror::Class> c = DecodeClass(class_id, &error);
    if (c == nullptr) {
      return error;
    }

    ArtMethod* m = FromMethodId(method_id);
    if (m->IsStatic() != (receiver == nullptr)) {
      return JDWP::ERR_INVALID_METHODID;
    }
    if (m->IsStatic()) {
      if (m->GetDeclaringClass() != c) {
        return JDWP::ERR_INVALID_METHODID;
      }
    } else {
      if (!m->GetDeclaringClass()->IsAssignableFrom(c)) {
        return JDWP::ERR_INVALID_METHODID;
      }
    }

    // Check the argument list matches the method.
    uint32_t shorty_len = 0;
    const char* shorty = m->GetShorty(&shorty_len);
    if (shorty_len - 1 != arg_count) {
      return JDWP::ERR_ILLEGAL_ARGUMENT;
    }

    {
      StackHandleScope<2> hs(soa.Self());
      HandleWrapperObjPtr<mirror::Object> h_obj(hs.NewHandleWrapper(&receiver));
      HandleWrapperObjPtr<mirror::Class> h_klass(hs.NewHandleWrapper(&c));
      const dex::TypeList* types = m->GetParameterTypeList();
      for (size_t i = 0; i < arg_count; ++i) {
        if (shorty[i + 1] != JdwpTagToShortyChar(arg_types[i])) {
          return JDWP::ERR_ILLEGAL_ARGUMENT;
        }

        if (shorty[i + 1] == 'L') {
          // Did we really get an argument of an appropriate reference type?
          ObjPtr<mirror::Class> parameter_type =
              m->ResolveClassFromTypeIndex(types->GetTypeItem(i).type_idx_);
          ObjPtr<mirror::Object> argument = gRegistry->Get<mirror::Object>(arg_values[i], &error);
          if (error != JDWP::ERR_NONE) {
            return JDWP::ERR_INVALID_OBJECT;
          }
          if (argument != nullptr && !argument->InstanceOf(parameter_type)) {
            return JDWP::ERR_ILLEGAL_ARGUMENT;
          }

          // Turn the on-the-wire ObjectId into a jobject.
          jvalue& v = reinterpret_cast<jvalue&>(arg_values[i]);
          v.l = gRegistry->GetJObject(arg_values[i]);
        }
      }
    }

    // Allocates a DebugInvokeReq.
    DebugInvokeReq* req = new (std::nothrow) DebugInvokeReq(
        request_id, thread_id, receiver, c, m, options, arg_values, arg_count);
    if (req == nullptr) {
      LOG(ERROR) << "Failed to allocate DebugInvokeReq";
      return JDWP::ERR_OUT_OF_MEMORY;
    }

    // Attaches the DebugInvokeReq to the target thread so it executes the method when
    // it is resumed. Once the invocation completes, the target thread will delete it before
    // suspending itself (see ThreadList::SuspendSelfForDebugger).
    targetThread->SetDebugInvokeReq(req);
  }

  // The fact that we've released the thread list lock is a bit risky --- if the thread goes
  // away we're sitting high and dry -- but we must release this before the UndoDebuggerSuspensions
  // call.
  if (resume_all_threads) {
    VLOG(jdwp) << "      Resuming all threads";
    thread_list->UndoDebuggerSuspensions();
  } else {
    VLOG(jdwp) << "      Resuming event thread only";
    bool resumed = thread_list->Resume(targetThread, SuspendReason::kForDebugger);
    DCHECK(resumed);
  }

  return JDWP::ERR_NONE;
}

void Dbg::ExecuteMethod(DebugInvokeReq* pReq) {
  Thread* const self = Thread::Current();
  CHECK_NE(self, GetDebugThread()) << "This must be called by the event thread";

  ScopedObjectAccess soa(self);

  // We can be called while an exception is pending. We need
  // to preserve that across the method invocation.
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Throwable> old_exception = hs.NewHandle(soa.Self()->GetException());
  soa.Self()->ClearException();

  // Execute the method then sends reply to the debugger.
  ExecuteMethodWithoutPendingException(soa, pReq);

  // If an exception was pending before the invoke, restore it now.
  if (old_exception != nullptr) {
    soa.Self()->SetException(old_exception.Get());
  }
}

// Helper function: write a variable-width value into the output input buffer.
static void WriteValue(JDWP::ExpandBuf* pReply, int width, uint64_t value) {
  switch (width) {
    case 1:
      expandBufAdd1(pReply, value);
      break;
    case 2:
      expandBufAdd2BE(pReply, value);
      break;
    case 4:
      expandBufAdd4BE(pReply, value);
      break;
    case 8:
      expandBufAdd8BE(pReply, value);
      break;
    default:
      LOG(FATAL) << width;
      UNREACHABLE();
  }
}

void Dbg::ExecuteMethodWithoutPendingException(ScopedObjectAccess& soa, DebugInvokeReq* pReq) {
  soa.Self()->AssertNoPendingException();

  // Translate the method through the vtable, unless the debugger wants to suppress it.
  ArtMethod* m = pReq->method;
  PointerSize image_pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  if ((pReq->options & JDWP::INVOKE_NONVIRTUAL) == 0 && pReq->receiver.Read() != nullptr) {
    ArtMethod* actual_method =
        pReq->klass.Read()->FindVirtualMethodForVirtualOrInterface(m, image_pointer_size);
    if (actual_method != m) {
      VLOG(jdwp) << "ExecuteMethod translated " << ArtMethod::PrettyMethod(m)
                 << " to " << ArtMethod::PrettyMethod(actual_method);
      m = actual_method;
    }
  }
  VLOG(jdwp) << "ExecuteMethod " << ArtMethod::PrettyMethod(m)
             << " receiver=" << pReq->receiver.Read()
             << " arg_count=" << pReq->arg_count;
  CHECK(m != nullptr);

  static_assert(sizeof(jvalue) == sizeof(uint64_t), "jvalue and uint64_t have different sizes.");

  // Invoke the method.
  ScopedLocalRef<jobject> ref(soa.Env(), soa.AddLocalReference<jobject>(pReq->receiver.Read()));
  JValue result = InvokeWithJValues(soa, ref.get(), jni::EncodeArtMethod(m),
                                    reinterpret_cast<jvalue*>(pReq->arg_values.get()));

  // Prepare JDWP ids for the reply.
  JDWP::JdwpTag result_tag = BasicTagFromDescriptor(m->GetShorty());
  const bool is_object_result = (result_tag == JDWP::JT_OBJECT);
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::Object> object_result = hs.NewHandle(is_object_result ? result.GetL() : nullptr);
  Handle<mirror::Throwable> exception = hs.NewHandle(soa.Self()->GetException());
  soa.Self()->ClearException();

  if (!IsDebuggerActive()) {
    // The debugger detached: we must not re-suspend threads. We also don't need to fill the reply
    // because it won't be sent either.
    return;
  }

  JDWP::ObjectId exceptionObjectId = gRegistry->Add(exception);
  uint64_t result_value = 0;
  if (exceptionObjectId != 0) {
    VLOG(jdwp) << "  JDWP invocation returning with exception=" << exception.Get()
               << " " << exception->Dump();
    result_value = 0;
  } else if (is_object_result) {
    /* if no exception was thrown, examine object result more closely */
    JDWP::JdwpTag new_tag = TagFromObject(soa, object_result.Get());
    if (new_tag != result_tag) {
      VLOG(jdwp) << "  JDWP promoted result from " << result_tag << " to " << new_tag;
      result_tag = new_tag;
    }

    // Register the object in the registry and reference its ObjectId. This ensures
    // GC safety and prevents from accessing stale reference if the object is moved.
    result_value = gRegistry->Add(object_result.Get());
  } else {
    // Primitive result.
    DCHECK(IsPrimitiveTag(result_tag));
    result_value = result.GetJ();
  }
  const bool is_constructor = m->IsConstructor() && !m->IsStatic();
  if (is_constructor) {
    // If we invoked a constructor (which actually returns void), return the receiver,
    // unless we threw, in which case we return null.
    DCHECK_EQ(JDWP::JT_VOID, result_tag);
    if (exceptionObjectId == 0) {
      if (m->GetDeclaringClass()->IsStringClass()) {
        // For string constructors, the new string is remapped to the receiver (stored in ref).
        Handle<mirror::Object> decoded_ref = hs.NewHandle(soa.Self()->DecodeJObject(ref.get()));
        result_value = gRegistry->Add(decoded_ref);
        result_tag = TagFromObject(soa, decoded_ref.Get());
      } else {
        // TODO we could keep the receiver ObjectId in the DebugInvokeReq to avoid looking into the
        // object registry.
        result_value = GetObjectRegistry()->Add(pReq->receiver.Read());
        result_tag = TagFromObject(soa, pReq->receiver.Read());
      }
    } else {
      result_value = 0;
      result_tag = JDWP::JT_OBJECT;
    }
  }

  // Suspend other threads if the invoke is not single-threaded.
  if ((pReq->options & JDWP::INVOKE_SINGLE_THREADED) == 0) {
    ScopedThreadSuspension sts(soa.Self(), kWaitingForDebuggerSuspension);
    // Avoid a deadlock between GC and debugger where GC gets suspended during GC. b/25800335.
    gc::ScopedGCCriticalSection gcs(soa.Self(), gc::kGcCauseDebugger, gc::kCollectorTypeDebugger);
    VLOG(jdwp) << "      Suspending all threads";
    Runtime::Current()->GetThreadList()->SuspendAllForDebugger();
  }

  VLOG(jdwp) << "  --> returned " << result_tag
             << StringPrintf(" %#" PRIx64 " (except=%#" PRIx64 ")", result_value,
                             exceptionObjectId);

  // Show detailed debug output.
  if (result_tag == JDWP::JT_STRING && exceptionObjectId == 0) {
    if (result_value != 0) {
      if (VLOG_IS_ON(jdwp)) {
        std::string result_string;
        JDWP::JdwpError error = Dbg::StringToUtf8(result_value, &result_string);
        CHECK_EQ(error, JDWP::ERR_NONE);
        VLOG(jdwp) << "      string '" << result_string << "'";
      }
    } else {
      VLOG(jdwp) << "      string (null)";
    }
  }

  // Attach the reply to DebugInvokeReq so it can be sent to the debugger when the event thread
  // is ready to suspend.
  BuildInvokeReply(pReq->reply, pReq->request_id, result_tag, result_value, exceptionObjectId);
}

void Dbg::BuildInvokeReply(JDWP::ExpandBuf* pReply, uint32_t request_id, JDWP::JdwpTag result_tag,
                           uint64_t result_value, JDWP::ObjectId exception) {
  // Make room for the JDWP header since we do not know the size of the reply yet.
  JDWP::expandBufAddSpace(pReply, kJDWPHeaderLen);

  size_t width = GetTagWidth(result_tag);
  JDWP::expandBufAdd1(pReply, result_tag);
  if (width != 0) {
    WriteValue(pReply, width, result_value);
  }
  JDWP::expandBufAdd1(pReply, JDWP::JT_OBJECT);
  JDWP::expandBufAddObjectId(pReply, exception);

  // Now we know the size, we can complete the JDWP header.
  uint8_t* buf = expandBufGetBuffer(pReply);
  JDWP::Set4BE(buf + kJDWPHeaderSizeOffset, expandBufGetLength(pReply));
  JDWP::Set4BE(buf + kJDWPHeaderIdOffset, request_id);
  JDWP::Set1(buf + kJDWPHeaderFlagsOffset, kJDWPFlagReply);  // flags
  JDWP::Set2BE(buf + kJDWPHeaderErrorCodeOffset, JDWP::ERR_NONE);
}

void Dbg::FinishInvokeMethod(DebugInvokeReq* pReq) {
  CHECK_NE(Thread::Current(), GetDebugThread()) << "This must be called by the event thread";

  JDWP::ExpandBuf* const pReply = pReq->reply;
  CHECK(pReply != nullptr) << "No reply attached to DebugInvokeReq";

  // We need to prevent other threads (including JDWP thread) from interacting with the debugger
  // while we send the reply but are not yet suspended. The JDWP token will be released just before
  // we suspend ourself again (see ThreadList::SuspendSelfForDebugger).
  gJdwpState->AcquireJdwpTokenForEvent(pReq->thread_id);

  // Send the reply unless the debugger detached before the completion of the method.
  if (IsDebuggerActive()) {
    const size_t replyDataLength = expandBufGetLength(pReply) - kJDWPHeaderLen;
    VLOG(jdwp) << StringPrintf("REPLY INVOKE id=0x%06x (length=%zu)",
                               pReq->request_id, replyDataLength);

    gJdwpState->SendRequest(pReply);
  } else {
    VLOG(jdwp) << "Not sending invoke reply because debugger detached";
  }
}

bool Dbg::DdmHandleChunk(JNIEnv* env,
                         uint32_t type,
                         const ArrayRef<const jbyte>& data,
                         /*out*/uint32_t* out_type,
                         /*out*/std::vector<uint8_t>* out_data) {
  ScopedLocalRef<jbyteArray> dataArray(env, env->NewByteArray(data.size()));
  if (dataArray.get() == nullptr) {
    LOG(WARNING) << "byte[] allocation failed: " << data.size();
    env->ExceptionClear();
    return false;
  }
  env->SetByteArrayRegion(dataArray.get(),
                          0,
                          data.size(),
                          reinterpret_cast<const jbyte*>(data.data()));
  // Call "private static Chunk dispatch(int type, byte[] data, int offset, int length)".
  ScopedLocalRef<jobject> chunk(
      env,
      env->CallStaticObjectMethod(
          WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer,
          WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer_dispatch,
          type, dataArray.get(), 0, data.size()));
  if (env->ExceptionCheck()) {
    Thread* self = Thread::Current();
    ScopedObjectAccess soa(self);
    LOG(INFO) << StringPrintf("Exception thrown by dispatcher for 0x%08x", type) << std::endl
              << self->GetException()->Dump();
    self->ClearException();
    return false;
  }

  if (chunk.get() == nullptr) {
    return false;
  }

  /*
   * Pull the pieces out of the chunk.  We copy the results into a
   * newly-allocated buffer that the caller can free.  We don't want to
   * continue using the Chunk object because nothing has a reference to it.
   *
   * We could avoid this by returning type/data/offset/length and having
   * the caller be aware of the object lifetime issues, but that
   * integrates the JDWP code more tightly into the rest of the runtime, and doesn't work
   * if we have responses for multiple chunks.
   *
   * So we're pretty much stuck with copying data around multiple times.
   */
  ScopedLocalRef<jbyteArray> replyData(
      env,
      reinterpret_cast<jbyteArray>(
          env->GetObjectField(
              chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_data)));
  jint offset = env->GetIntField(chunk.get(),
                                 WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_offset);
  jint length = env->GetIntField(chunk.get(),
                                 WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_length);
  *out_type = env->GetIntField(chunk.get(),
                               WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_type);

  VLOG(jdwp) << StringPrintf("DDM reply: type=0x%08x data=%p offset=%d length=%d",
                             type,
                             replyData.get(),
                             offset,
                             length);
  out_data->resize(length);
  env->GetByteArrayRegion(replyData.get(),
                          offset,
                          length,
                          reinterpret_cast<jbyte*>(out_data->data()));

  if (env->ExceptionCheck()) {
    Thread* self = Thread::Current();
    ScopedObjectAccess soa(self);
    LOG(INFO) << StringPrintf("Exception thrown when reading response data from dispatcher 0x%08x",
                              type) << std::endl << self->GetException()->Dump();
    self->ClearException();
    return false;
  }

  return true;
}

/*
 * "request" contains a full JDWP packet, possibly with multiple chunks.  We
 * need to process each, accumulate the replies, and ship the whole thing
 * back.
 *
 * Returns "true" if we have a reply.  The reply buffer is newly allocated,
 * and includes the chunk type/length, followed by the data.
 *
 * OLD-TODO: we currently assume that the request and reply include a single
 * chunk.  If this becomes inconvenient we will need to adapt.
 */
bool Dbg::DdmHandlePacket(JDWP::Request* request, uint8_t** pReplyBuf, int* pReplyLen) {
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  uint32_t type = request->ReadUnsigned32("type");
  uint32_t length = request->ReadUnsigned32("length");

  // Create a byte[] corresponding to 'request'.
  size_t request_length = request->size();
  // Run through and find all chunks.  [Currently just find the first.]
  if (length != request_length) {
    LOG(WARNING) << StringPrintf("bad chunk found (len=%u pktLen=%zd)", length, request_length);
    return false;
  }

  ArrayRef<const jbyte> data(reinterpret_cast<const jbyte*>(request->data()), request_length);
  std::vector<uint8_t> out_data;
  uint32_t out_type = 0;
  request->Skip(request_length);
  if (!DdmHandleChunk(env, type, data, &out_type, &out_data) || out_data.empty()) {
    return false;
  }
  const uint32_t kDdmHeaderSize = 8;
  *pReplyLen = out_data.size() + kDdmHeaderSize;
  *pReplyBuf = new uint8_t[out_data.size() + kDdmHeaderSize];
  memcpy((*pReplyBuf) + kDdmHeaderSize, out_data.data(), out_data.size());
  JDWP::Set4BE(*pReplyBuf, out_type);
  JDWP::Set4BE((*pReplyBuf) + 4, static_cast<uint32_t>(out_data.size()));
  VLOG(jdwp)
      << StringPrintf("dvmHandleDdm returning type=%.4s", reinterpret_cast<char*>(*pReplyBuf))
      << "0x" << std::hex << reinterpret_cast<uintptr_t>(*pReplyBuf) << std::dec
      << " len= " << out_data.size();
  return true;
}

void Dbg::DdmBroadcast(bool connect) {
  VLOG(jdwp) << "Broadcasting DDM " << (connect ? "connect" : "disconnect") << "...";

  Thread* self = Thread::Current();
  if (self->GetState() != kRunnable) {
    LOG(ERROR) << "DDM broadcast in thread state " << self->GetState();
    /* try anyway? */
  }

  JNIEnv* env = self->GetJniEnv();
  jint event = connect ? 1 /*DdmServer.CONNECTED*/ : 2 /*DdmServer.DISCONNECTED*/;
  env->CallStaticVoidMethod(WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer,
                            WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer_broadcast,
                            event);
  if (env->ExceptionCheck()) {
    LOG(ERROR) << "DdmServer.broadcast " << event << " failed";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

void Dbg::DdmConnected() {
  Dbg::DdmBroadcast(true);
}

void Dbg::DdmDisconnected() {
  Dbg::DdmBroadcast(false);
  gDdmThreadNotification = false;
}

/*
 * Send a notification when a thread starts, stops, or changes its name.
 *
 * Because we broadcast the full set of threads when the notifications are
 * first enabled, it's possible for "thread" to be actively executing.
 */
void Dbg::DdmSendThreadNotification(Thread* t, uint32_t type) {
  if (!gDdmThreadNotification) {
    return;
  }

  RuntimeCallbacks* cb = Runtime::Current()->GetRuntimeCallbacks();
  if (type == CHUNK_TYPE("THDE")) {
    uint8_t buf[4];
    JDWP::Set4BE(&buf[0], t->GetThreadId());
    cb->DdmPublishChunk(CHUNK_TYPE("THDE"), ArrayRef<const uint8_t>(buf));
  } else {
    CHECK(type == CHUNK_TYPE("THCR") || type == CHUNK_TYPE("THNM")) << type;
    ScopedObjectAccessUnchecked soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::String> name(hs.NewHandle(t->GetThreadName()));
    size_t char_count = (name != nullptr) ? name->GetLength() : 0;
    const jchar* chars = (name != nullptr) ? name->GetValue() : nullptr;
    bool is_compressed = (name != nullptr) ? name->IsCompressed() : false;

    std::vector<uint8_t> bytes;
    JDWP::Append4BE(bytes, t->GetThreadId());
    if (is_compressed) {
      const uint8_t* chars_compressed = name->GetValueCompressed();
      JDWP::AppendUtf16CompressedBE(bytes, chars_compressed, char_count);
    } else {
      JDWP::AppendUtf16BE(bytes, chars, char_count);
    }
    CHECK_EQ(bytes.size(), char_count*2 + sizeof(uint32_t)*2);
    cb->DdmPublishChunk(type, ArrayRef<const uint8_t>(bytes));
  }
}

void Dbg::DdmSetThreadNotification(bool enable) {
  // Enable/disable thread notifications.
  gDdmThreadNotification = enable;
  if (enable) {
    // Suspend the VM then post thread start notifications for all threads. Threads attaching will
    // see a suspension in progress and block until that ends. They then post their own start
    // notification.
    SuspendVM();
    std::list<Thread*> threads;
    Thread* self = Thread::Current();
    {
      MutexLock mu(self, *Locks::thread_list_lock_);
      threads = Runtime::Current()->GetThreadList()->GetList();
    }
    {
      ScopedObjectAccess soa(self);
      for (Thread* thread : threads) {
        Dbg::DdmSendThreadNotification(thread, CHUNK_TYPE("THCR"));
      }
    }
    ResumeVM();
  }
}

void Dbg::PostThreadStartOrStop(Thread* t, uint32_t type) {
  if (IsDebuggerActive()) {
    gJdwpState->PostThreadChange(t, type == CHUNK_TYPE("THCR"));
  }
  Dbg::DdmSendThreadNotification(t, type);
}

void Dbg::PostThreadStart(Thread* t) {
  Dbg::PostThreadStartOrStop(t, CHUNK_TYPE("THCR"));
}

void Dbg::PostThreadDeath(Thread* t) {
  Dbg::PostThreadStartOrStop(t, CHUNK_TYPE("THDE"));
}

JDWP::JdwpState* Dbg::GetJdwpState() {
  return gJdwpState;
}

int Dbg::DdmHandleHpifChunk(HpifWhen when) {
  if (when == HPIF_WHEN_NOW) {
    DdmSendHeapInfo(when);
    return 1;
  }

  if (when != HPIF_WHEN_NEVER && when != HPIF_WHEN_NEXT_GC && when != HPIF_WHEN_EVERY_GC) {
    LOG(ERROR) << "invalid HpifWhen value: " << static_cast<int>(when);
    return 0;
  }

  gDdmHpifWhen = when;
  return 1;
}

bool Dbg::DdmHandleHpsgNhsgChunk(Dbg::HpsgWhen when, Dbg::HpsgWhat what, bool native) {
  if (when != HPSG_WHEN_NEVER && when != HPSG_WHEN_EVERY_GC) {
    LOG(ERROR) << "invalid HpsgWhen value: " << static_cast<int>(when);
    return false;
  }

  if (what != HPSG_WHAT_MERGED_OBJECTS && what != HPSG_WHAT_DISTINCT_OBJECTS) {
    LOG(ERROR) << "invalid HpsgWhat value: " << static_cast<int>(what);
    return false;
  }

  if (native) {
    gDdmNhsgWhen = when;
    gDdmNhsgWhat = what;
  } else {
    gDdmHpsgWhen = when;
    gDdmHpsgWhat = what;
  }
  return true;
}

void Dbg::DdmSendHeapInfo(HpifWhen reason) {
  // If there's a one-shot 'when', reset it.
  if (reason == gDdmHpifWhen) {
    if (gDdmHpifWhen == HPIF_WHEN_NEXT_GC) {
      gDdmHpifWhen = HPIF_WHEN_NEVER;
    }
  }

  /*
   * Chunk HPIF (client --> server)
   *
   * Heap Info. General information about the heap,
   * suitable for a summary display.
   *
   *   [u4]: number of heaps
   *
   *   For each heap:
   *     [u4]: heap ID
   *     [u8]: timestamp in ms since Unix epoch
   *     [u1]: capture reason (same as 'when' value from server)
   *     [u4]: max heap size in bytes (-Xmx)
   *     [u4]: current heap size in bytes
   *     [u4]: current number of bytes allocated
   *     [u4]: current number of objects allocated
   */
  uint8_t heap_count = 1;
  gc::Heap* heap = Runtime::Current()->GetHeap();
  std::vector<uint8_t> bytes;
  JDWP::Append4BE(bytes, heap_count);
  JDWP::Append4BE(bytes, 1);  // Heap id (bogus; we only have one heap).
  JDWP::Append8BE(bytes, MilliTime());
  JDWP::Append1BE(bytes, reason);
  JDWP::Append4BE(bytes, heap->GetMaxMemory());  // Max allowed heap size in bytes.
  JDWP::Append4BE(bytes, heap->GetTotalMemory());  // Current heap size in bytes.
  JDWP::Append4BE(bytes, heap->GetBytesAllocated());
  JDWP::Append4BE(bytes, heap->GetObjectsAllocated());
  CHECK_EQ(bytes.size(), 4U + (heap_count * (4 + 8 + 1 + 4 + 4 + 4 + 4)));
  Runtime::Current()->GetRuntimeCallbacks()->DdmPublishChunk(CHUNK_TYPE("HPIF"),
                                                             ArrayRef<const uint8_t>(bytes));
}

enum HpsgSolidity {
  SOLIDITY_FREE = 0,
  SOLIDITY_HARD = 1,
  SOLIDITY_SOFT = 2,
  SOLIDITY_WEAK = 3,
  SOLIDITY_PHANTOM = 4,
  SOLIDITY_FINALIZABLE = 5,
  SOLIDITY_SWEEP = 6,
};

enum HpsgKind {
  KIND_OBJECT = 0,
  KIND_CLASS_OBJECT = 1,
  KIND_ARRAY_1 = 2,
  KIND_ARRAY_2 = 3,
  KIND_ARRAY_4 = 4,
  KIND_ARRAY_8 = 5,
  KIND_UNKNOWN = 6,
  KIND_NATIVE = 7,
};

#define HPSG_PARTIAL (1<<7)
#define HPSG_STATE(solidity, kind) ((uint8_t)((((kind) & 0x7) << 3) | ((solidity) & 0x7)))

class HeapChunkContext {
 public:
  // Maximum chunk size.  Obtain this from the formula:
  // (((maximum_heap_size / ALLOCATION_UNIT_SIZE) + 255) / 256) * 2
  HeapChunkContext(bool merge, bool native)
      : buf_(16384 - 16),
        type_(0),
        chunk_overhead_(0) {
    Reset();
    if (native) {
      type_ = CHUNK_TYPE("NHSG");
    } else {
      type_ = merge ? CHUNK_TYPE("HPSG") : CHUNK_TYPE("HPSO");
    }
  }

  ~HeapChunkContext() {
    if (p_ > &buf_[0]) {
      Flush();
    }
  }

  void SetChunkOverhead(size_t chunk_overhead) {
    chunk_overhead_ = chunk_overhead;
  }

  void ResetStartOfNextChunk() {
    startOfNextMemoryChunk_ = nullptr;
  }

  void EnsureHeader(const void* chunk_ptr) {
    if (!needHeader_) {
      return;
    }

    // Start a new HPSx chunk.
    JDWP::Write4BE(&p_, 1);  // Heap id (bogus; we only have one heap).
    JDWP::Write1BE(&p_, 8);  // Size of allocation unit, in bytes.

    JDWP::Write4BE(&p_, reinterpret_cast<uintptr_t>(chunk_ptr));  // virtual address of segment start.
    JDWP::Write4BE(&p_, 0);  // offset of this piece (relative to the virtual address).
    // [u4]: length of piece, in allocation units
    // We won't know this until we're done, so save the offset and stuff in a dummy value.
    pieceLenField_ = p_;
    JDWP::Write4BE(&p_, 0x55555555);
    needHeader_ = false;
  }

  void Flush() REQUIRES_SHARED(Locks::mutator_lock_) {
    if (pieceLenField_ == nullptr) {
      // Flush immediately post Reset (maybe back-to-back Flush). Ignore.
      CHECK(needHeader_);
      return;
    }
    // Patch the "length of piece" field.
    CHECK_LE(&buf_[0], pieceLenField_);
    CHECK_LE(pieceLenField_, p_);
    JDWP::Set4BE(pieceLenField_, totalAllocationUnits_);

    ArrayRef<const uint8_t> out(&buf_[0], p_ - &buf_[0]);
    Runtime::Current()->GetRuntimeCallbacks()->DdmPublishChunk(type_, out);
    Reset();
  }

  static void HeapChunkJavaCallback(void* start, void* end, size_t used_bytes, void* arg)
      REQUIRES_SHARED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
    reinterpret_cast<HeapChunkContext*>(arg)->HeapChunkJavaCallback(start, end, used_bytes);
  }

  static void HeapChunkNativeCallback(void* start, void* end, size_t used_bytes, void* arg)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    reinterpret_cast<HeapChunkContext*>(arg)->HeapChunkNativeCallback(start, end, used_bytes);
  }

 private:
  enum { ALLOCATION_UNIT_SIZE = 8 };

  void Reset() {
    p_ = &buf_[0];
    ResetStartOfNextChunk();
    totalAllocationUnits_ = 0;
    needHeader_ = true;
    pieceLenField_ = nullptr;
  }

  bool IsNative() const {
    return type_ == CHUNK_TYPE("NHSG");
  }

  // Returns true if the object is not an empty chunk.
  bool ProcessRecord(void* start, size_t used_bytes) REQUIRES_SHARED(Locks::mutator_lock_) {
    // Note: heap call backs cannot manipulate the heap upon which they are crawling, care is taken
    // in the following code not to allocate memory, by ensuring buf_ is of the correct size
    if (used_bytes == 0) {
      if (start == nullptr) {
        // Reset for start of new heap.
        startOfNextMemoryChunk_ = nullptr;
        Flush();
      }
      // Only process in use memory so that free region information
      // also includes dlmalloc book keeping.
      return false;
    }
    if (startOfNextMemoryChunk_ != nullptr) {
      // Transmit any pending free memory. Native free memory of over kMaxFreeLen could be because
      // of the use of mmaps, so don't report. If not free memory then start a new segment.
      bool flush = true;
      if (start > startOfNextMemoryChunk_) {
        const size_t kMaxFreeLen = 2 * kPageSize;
        void* free_start = startOfNextMemoryChunk_;
        void* free_end = start;
        const size_t free_len =
            reinterpret_cast<uintptr_t>(free_end) - reinterpret_cast<uintptr_t>(free_start);
        if (!IsNative() || free_len < kMaxFreeLen) {
          AppendChunk(HPSG_STATE(SOLIDITY_FREE, 0), free_start, free_len, IsNative());
          flush = false;
        }
      }
      if (flush) {
        startOfNextMemoryChunk_ = nullptr;
        Flush();
      }
    }
    return true;
  }

  void HeapChunkNativeCallback(void* start, void* /*end*/, size_t used_bytes)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (ProcessRecord(start, used_bytes)) {
      uint8_t state = ExamineNativeObject(start);
      AppendChunk(state, start, used_bytes + chunk_overhead_, /*is_native=*/ true);
      startOfNextMemoryChunk_ = reinterpret_cast<char*>(start) + used_bytes + chunk_overhead_;
    }
  }

  void HeapChunkJavaCallback(void* start, void* /*end*/, size_t used_bytes)
      REQUIRES_SHARED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    if (ProcessRecord(start, used_bytes)) {
      // Determine the type of this chunk.
      // OLD-TODO: if context.merge, see if this chunk is different from the last chunk.
      // If it's the same, we should combine them.
      uint8_t state = ExamineJavaObject(reinterpret_cast<mirror::Object*>(start));
      AppendChunk(state, start, used_bytes + chunk_overhead_, /*is_native=*/ false);
      startOfNextMemoryChunk_ = reinterpret_cast<char*>(start) + used_bytes + chunk_overhead_;
    }
  }

  void AppendChunk(uint8_t state, void* ptr, size_t length, bool is_native)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // Make sure there's enough room left in the buffer.
    // We need to use two bytes for every fractional 256 allocation units used by the chunk plus
    // 17 bytes for any header.
    const size_t needed = ((RoundUp(length / ALLOCATION_UNIT_SIZE, 256) / 256) * 2) + 17;
    size_t byte_left = &buf_.back() - p_;
    if (byte_left < needed) {
      if (is_native) {
      // Cannot trigger memory allocation while walking native heap.
        return;
      }
      Flush();
    }

    byte_left = &buf_.back() - p_;
    if (byte_left < needed) {
      LOG(WARNING) << "Chunk is too big to transmit (chunk_len=" << length << ", "
          << needed << " bytes)";
      return;
    }
    EnsureHeader(ptr);
    // Write out the chunk description.
    length /= ALLOCATION_UNIT_SIZE;   // Convert to allocation units.
    totalAllocationUnits_ += length;
    while (length > 256) {
      *p_++ = state | HPSG_PARTIAL;
      *p_++ = 255;     // length - 1
      length -= 256;
    }
    *p_++ = state;
    *p_++ = length - 1;
  }

  uint8_t ExamineNativeObject(const void* p) REQUIRES_SHARED(Locks::mutator_lock_) {
    return p == nullptr ? HPSG_STATE(SOLIDITY_FREE, 0) : HPSG_STATE(SOLIDITY_HARD, KIND_NATIVE);
  }

  uint8_t ExamineJavaObject(ObjPtr<mirror::Object> o)
      REQUIRES_SHARED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    if (o == nullptr) {
      return HPSG_STATE(SOLIDITY_FREE, 0);
    }
    // It's an allocated chunk. Figure out what it is.
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (!heap->IsLiveObjectLocked(o)) {
      LOG(ERROR) << "Invalid object in managed heap: " << o;
      return HPSG_STATE(SOLIDITY_HARD, KIND_NATIVE);
    }
    ObjPtr<mirror::Class> c = o->GetClass();
    if (c == nullptr) {
      // The object was probably just created but hasn't been initialized yet.
      return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
    }
    if (!heap->IsValidObjectAddress(c.Ptr())) {
      LOG(ERROR) << "Invalid class for managed heap object: " << o << " " << c;
      return HPSG_STATE(SOLIDITY_HARD, KIND_UNKNOWN);
    }
    if (c->GetClass() == nullptr) {
      LOG(ERROR) << "Null class of class " << c << " for object " << o;
      return HPSG_STATE(SOLIDITY_HARD, KIND_UNKNOWN);
    }
    if (c->IsClassClass()) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_CLASS_OBJECT);
    }
    if (c->IsArrayClass()) {
      switch (c->GetComponentSize()) {
      case 1: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_1);
      case 2: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_2);
      case 4: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_4);
      case 8: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_8);
      }
    }
    return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
  }

  std::vector<uint8_t> buf_;
  uint8_t* p_;
  uint8_t* pieceLenField_;
  void* startOfNextMemoryChunk_;
  size_t totalAllocationUnits_;
  uint32_t type_;
  bool needHeader_;
  size_t chunk_overhead_;

  DISALLOW_COPY_AND_ASSIGN(HeapChunkContext);
};

void Dbg::DdmSendHeapSegments(bool native) {
  Dbg::HpsgWhen when = native ? gDdmNhsgWhen : gDdmHpsgWhen;
  Dbg::HpsgWhat what = native ? gDdmNhsgWhat : gDdmHpsgWhat;
  if (when == HPSG_WHEN_NEVER) {
    return;
  }
  RuntimeCallbacks* cb = Runtime::Current()->GetRuntimeCallbacks();
  // Figure out what kind of chunks we'll be sending.
  CHECK(what == HPSG_WHAT_MERGED_OBJECTS || what == HPSG_WHAT_DISTINCT_OBJECTS)
      << static_cast<int>(what);

  // First, send a heap start chunk.
  uint8_t heap_id[4];
  JDWP::Set4BE(&heap_id[0], 1);  // Heap id (bogus; we only have one heap).
  cb->DdmPublishChunk(native ? CHUNK_TYPE("NHST") : CHUNK_TYPE("HPST"),
                      ArrayRef<const uint8_t>(heap_id));
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertSharedHeld(self);

  // Send a series of heap segment chunks.
  HeapChunkContext context(what == HPSG_WHAT_MERGED_OBJECTS, native);
  auto bump_pointer_space_visitor = [&](mirror::Object* obj)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(Locks::heap_bitmap_lock_) {
    const size_t size = RoundUp(obj->SizeOf(), kObjectAlignment);
    HeapChunkContext::HeapChunkJavaCallback(
        obj, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(obj) + size), size, &context);
  };
  if (native) {
    UNIMPLEMENTED(WARNING) << "Native heap inspection is not supported";
  } else {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    for (const auto& space : heap->GetContinuousSpaces()) {
      if (space->IsDlMallocSpace()) {
        ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
        // dlmalloc's chunk header is 2 * sizeof(size_t), but if the previous chunk is in use for an
        // allocation then the first sizeof(size_t) may belong to it.
        context.SetChunkOverhead(sizeof(size_t));
        space->AsDlMallocSpace()->Walk(HeapChunkContext::HeapChunkJavaCallback, &context);
      } else if (space->IsRosAllocSpace()) {
        context.SetChunkOverhead(0);
        // Need to acquire the mutator lock before the heap bitmap lock with exclusive access since
        // RosAlloc's internal logic doesn't know to release and reacquire the heap bitmap lock.
        ScopedThreadSuspension sts(self, kSuspended);
        ScopedSuspendAll ssa(__FUNCTION__);
        ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
        space->AsRosAllocSpace()->Walk(HeapChunkContext::HeapChunkJavaCallback, &context);
      } else if (space->IsBumpPointerSpace()) {
        ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
        context.SetChunkOverhead(0);
        space->AsBumpPointerSpace()->Walk(bump_pointer_space_visitor);
        HeapChunkContext::HeapChunkJavaCallback(nullptr, nullptr, 0, &context);
      } else if (space->IsRegionSpace()) {
        heap->IncrementDisableMovingGC(self);
        {
          ScopedThreadSuspension sts(self, kSuspended);
          ScopedSuspendAll ssa(__FUNCTION__);
          ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
          context.SetChunkOverhead(0);
          space->AsRegionSpace()->Walk(bump_pointer_space_visitor);
          HeapChunkContext::HeapChunkJavaCallback(nullptr, nullptr, 0, &context);
        }
        heap->DecrementDisableMovingGC(self);
      } else {
        UNIMPLEMENTED(WARNING) << "Not counting objects in space " << *space;
      }
      context.ResetStartOfNextChunk();
    }
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // Walk the large objects, these are not in the AllocSpace.
    context.SetChunkOverhead(0);
    heap->GetLargeObjectsSpace()->Walk(HeapChunkContext::HeapChunkJavaCallback, &context);
  }

  // Finally, send a heap end chunk.
  cb->DdmPublishChunk(native ? CHUNK_TYPE("NHEN") : CHUNK_TYPE("HPEN"),
                      ArrayRef<const uint8_t>(heap_id));
}

void Dbg::SetAllocTrackingEnabled(bool enable) {
  gc::AllocRecordObjectMap::SetAllocTrackingEnabled(enable);
}

void Dbg::DumpRecentAllocations() {
  ScopedObjectAccess soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::alloc_tracker_lock_);
  if (!Runtime::Current()->GetHeap()->IsAllocTrackingEnabled()) {
    LOG(INFO) << "Not recording tracked allocations";
    return;
  }
  gc::AllocRecordObjectMap* records = Runtime::Current()->GetHeap()->GetAllocationRecords();
  CHECK(records != nullptr);

  const uint16_t capped_count = CappedAllocRecordCount(records->GetRecentAllocationSize());
  uint16_t count = capped_count;

  LOG(INFO) << "Tracked allocations, (count=" << count << ")";
  for (auto it = records->RBegin(), end = records->REnd();
      count > 0 && it != end; count--, it++) {
    const gc::AllocRecord* record = &it->second;

    LOG(INFO) << StringPrintf(" Thread %-2d %6zd bytes ", record->GetTid(), record->ByteCount())
              << mirror::Class::PrettyClass(record->GetClass());

    for (size_t stack_frame = 0, depth = record->GetDepth(); stack_frame < depth; ++stack_frame) {
      const gc::AllocRecordStackTraceElement& stack_element = record->StackElement(stack_frame);
      ArtMethod* m = stack_element.GetMethod();
      LOG(INFO) << "    " << ArtMethod::PrettyMethod(m) << " line "
                << stack_element.ComputeLineNumber();
    }

    // pause periodically to help logcat catch up
    if ((count % 5) == 0) {
      usleep(40000);
    }
  }
}

class StringTable {
 private:
  struct Entry {
    explicit Entry(const char* data_in)
        : data(data_in), hash(ComputeModifiedUtf8Hash(data_in)), index(0) {
    }
    Entry(const Entry& entry) = default;
    Entry(Entry&& entry) = default;

    // Pointer to the actual string data.
    const char* data;

    // The hash of the data.
    const uint32_t hash;

    // The index. This will be filled in on Finish and is not part of the ordering, so mark it
    // mutable.
    mutable uint32_t index;

    bool operator==(const Entry& other) const {
      return strcmp(data, other.data) == 0;
    }
  };
  struct EntryHash {
    size_t operator()(const Entry& entry) const {
      return entry.hash;
    }
  };

 public:
  StringTable() : finished_(false) {
  }

  void Add(const char* str, bool copy_string) {
    DCHECK(!finished_);
    if (UNLIKELY(copy_string)) {
      // Check whether it's already there.
      Entry entry(str);
      if (table_.find(entry) != table_.end()) {
        return;
      }

      // Make a copy.
      size_t str_len = strlen(str);
      char* copy = new char[str_len + 1];
      strlcpy(copy, str, str_len + 1);
      string_backup_.emplace_back(copy);
      str = copy;
    }
    Entry entry(str);
    table_.insert(entry);
  }

  // Update all entries and give them an index. Note that this is likely not the insertion order,
  // as the set will with high likelihood reorder elements. Thus, Add must not be called after
  // Finish, and Finish must be called before IndexOf. In that case, WriteTo will walk in
  // the same order as Finish, and indices will agree. The order invariant, as well as indices,
  // are enforced through debug checks.
  void Finish() {
    DCHECK(!finished_);
    finished_ = true;
    uint32_t index = 0;
    for (auto& entry : table_) {
      entry.index = index;
      ++index;
    }
  }

  size_t IndexOf(const char* s) const {
    DCHECK(finished_);
    Entry entry(s);
    auto it = table_.find(entry);
    if (it == table_.end()) {
      LOG(FATAL) << "IndexOf(\"" << s << "\") failed";
    }
    return it->index;
  }

  size_t Size() const {
    return table_.size();
  }

  void WriteTo(std::vector<uint8_t>& bytes) const {
    DCHECK(finished_);
    uint32_t cur_index = 0;
    for (const auto& entry : table_) {
      DCHECK_EQ(cur_index++, entry.index);

      size_t s_len = CountModifiedUtf8Chars(entry.data);
      std::unique_ptr<uint16_t[]> s_utf16(new uint16_t[s_len]);
      ConvertModifiedUtf8ToUtf16(s_utf16.get(), entry.data);
      JDWP::AppendUtf16BE(bytes, s_utf16.get(), s_len);
    }
  }

 private:
  std::unordered_set<Entry, EntryHash> table_;
  std::vector<std::unique_ptr<char[]>> string_backup_;

  bool finished_;

  DISALLOW_COPY_AND_ASSIGN(StringTable);
};

static const char* GetMethodSourceFile(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(method != nullptr);
  const char* source_file = method->GetDeclaringClassSourceFile();
  return (source_file != nullptr) ? source_file : "";
}

/*
 * The data we send to DDMS contains everything we have recorded.
 *
 * Message header (all values big-endian):
 * (1b) message header len (to allow future expansion); includes itself
 * (1b) entry header len
 * (1b) stack frame len
 * (2b) number of entries
 * (4b) offset to string table from start of message
 * (2b) number of class name strings
 * (2b) number of method name strings
 * (2b) number of source file name strings
 * For each entry:
 *   (4b) total allocation size
 *   (2b) thread id
 *   (2b) allocated object's class name index
 *   (1b) stack depth
 *   For each stack frame:
 *     (2b) method's class name
 *     (2b) method name
 *     (2b) method source file
 *     (2b) line number, clipped to 32767; -2 if native; -1 if no source
 * (xb) class name strings
 * (xb) method name strings
 * (xb) source file strings
 *
 * As with other DDM traffic, strings are sent as a 4-byte length
 * followed by UTF-16 data.
 *
 * We send up 16-bit unsigned indexes into string tables.  In theory there
 * can be (kMaxAllocRecordStackDepth * alloc_record_max_) unique strings in
 * each table, but in practice there should be far fewer.
 *
 * The chief reason for using a string table here is to keep the size of
 * the DDMS message to a minimum.  This is partly to make the protocol
 * efficient, but also because we have to form the whole thing up all at
 * once in a memory buffer.
 *
 * We use separate string tables for class names, method names, and source
 * files to keep the indexes small.  There will generally be no overlap
 * between the contents of these tables.
 */
jbyteArray Dbg::GetRecentAllocations() {
  if ((false)) {
    DumpRecentAllocations();
  }

  Thread* self = Thread::Current();
  std::vector<uint8_t> bytes;
  {
    MutexLock mu(self, *Locks::alloc_tracker_lock_);
    gc::AllocRecordObjectMap* records = Runtime::Current()->GetHeap()->GetAllocationRecords();
    // In case this method is called when allocation tracker is disabled,
    // we should still send some data back.
    gc::AllocRecordObjectMap dummy;
    if (records == nullptr) {
      CHECK(!Runtime::Current()->GetHeap()->IsAllocTrackingEnabled());
      records = &dummy;
    }
    // We don't need to wait on the condition variable records->new_record_condition_, because this
    // function only reads the class objects, which are already marked so it doesn't change their
    // reachability.

    //
    // Part 1: generate string tables.
    //
    StringTable class_names;
    StringTable method_names;
    StringTable filenames;

    VLOG(jdwp) << "Collecting StringTables.";

    const uint16_t capped_count = CappedAllocRecordCount(records->GetRecentAllocationSize());
    uint16_t count = capped_count;
    size_t alloc_byte_count = 0;
    for (auto it = records->RBegin(), end = records->REnd();
         count > 0 && it != end; count--, it++) {
      const gc::AllocRecord* record = &it->second;
      std::string temp;
      const char* class_descr = record->GetClassDescriptor(&temp);
      class_names.Add(class_descr, !temp.empty());

      // Size + tid + class name index + stack depth.
      alloc_byte_count += 4u + 2u + 2u + 1u;

      for (size_t i = 0, depth = record->GetDepth(); i < depth; i++) {
        ArtMethod* m = record->StackElement(i).GetMethod();
        class_names.Add(m->GetDeclaringClassDescriptor(), false);
        method_names.Add(m->GetName(), false);
        filenames.Add(GetMethodSourceFile(m), false);
      }

      // Depth * (class index + method name index + file name index + line number).
      alloc_byte_count += record->GetDepth() * (2u + 2u + 2u + 2u);
    }

    class_names.Finish();
    method_names.Finish();
    filenames.Finish();
    VLOG(jdwp) << "Done collecting StringTables:" << std::endl
               << "  ClassNames: " << class_names.Size() << std::endl
               << "  MethodNames: " << method_names.Size() << std::endl
               << "  Filenames: " << filenames.Size();

    LOG(INFO) << "recent allocation records: " << capped_count;
    LOG(INFO) << "allocation records all objects: " << records->Size();

    //
    // Part 2: Generate the output and store it in the buffer.
    //

    // (1b) message header len (to allow future expansion); includes itself
    // (1b) entry header len
    // (1b) stack frame len
    const int kMessageHeaderLen = 15;
    const int kEntryHeaderLen = 9;
    const int kStackFrameLen = 8;
    JDWP::Append1BE(bytes, kMessageHeaderLen);
    JDWP::Append1BE(bytes, kEntryHeaderLen);
    JDWP::Append1BE(bytes, kStackFrameLen);

    // (2b) number of entries
    // (4b) offset to string table from start of message
    // (2b) number of class name strings
    // (2b) number of method name strings
    // (2b) number of source file name strings
    JDWP::Append2BE(bytes, capped_count);
    size_t string_table_offset = bytes.size();
    JDWP::Append4BE(bytes, 0);  // We'll patch this later...
    JDWP::Append2BE(bytes, class_names.Size());
    JDWP::Append2BE(bytes, method_names.Size());
    JDWP::Append2BE(bytes, filenames.Size());

    VLOG(jdwp) << "Dumping allocations with stacks";

    // Enlarge the vector for the allocation data.
    size_t reserve_size = bytes.size() + alloc_byte_count;
    bytes.reserve(reserve_size);

    std::string temp;
    count = capped_count;
    // The last "count" number of allocation records in "records" are the most recent "count" number
    // of allocations. Reverse iterate to get them. The most recent allocation is sent first.
    for (auto it = records->RBegin(), end = records->REnd();
         count > 0 && it != end; count--, it++) {
      // For each entry:
      // (4b) total allocation size
      // (2b) thread id
      // (2b) allocated object's class name index
      // (1b) stack depth
      const gc::AllocRecord* record = &it->second;
      size_t stack_depth = record->GetDepth();
      size_t allocated_object_class_name_index =
          class_names.IndexOf(record->GetClassDescriptor(&temp));
      JDWP::Append4BE(bytes, record->ByteCount());
      JDWP::Append2BE(bytes, static_cast<uint16_t>(record->GetTid()));
      JDWP::Append2BE(bytes, allocated_object_class_name_index);
      JDWP::Append1BE(bytes, stack_depth);

      for (size_t stack_frame = 0; stack_frame < stack_depth; ++stack_frame) {
        // For each stack frame:
        // (2b) method's class name
        // (2b) method name
        // (2b) method source file
        // (2b) line number, clipped to 32767; -2 if native; -1 if no source
        ArtMethod* m = record->StackElement(stack_frame).GetMethod();
        size_t class_name_index = class_names.IndexOf(m->GetDeclaringClassDescriptor());
        size_t method_name_index = method_names.IndexOf(m->GetName());
        size_t file_name_index = filenames.IndexOf(GetMethodSourceFile(m));
        JDWP::Append2BE(bytes, class_name_index);
        JDWP::Append2BE(bytes, method_name_index);
        JDWP::Append2BE(bytes, file_name_index);
        JDWP::Append2BE(bytes, record->StackElement(stack_frame).ComputeLineNumber());
      }
    }

    CHECK_EQ(bytes.size(), reserve_size);
    VLOG(jdwp) << "Dumping tables.";

    // (xb) class name strings
    // (xb) method name strings
    // (xb) source file strings
    JDWP::Set4BE(&bytes[string_table_offset], bytes.size());
    class_names.WriteTo(bytes);
    method_names.WriteTo(bytes);
    filenames.WriteTo(bytes);

    VLOG(jdwp) << "GetRecentAllocations: data created. " << bytes.size();
  }
  JNIEnv* env = self->GetJniEnv();
  jbyteArray result = env->NewByteArray(bytes.size());
  if (result != nullptr) {
    env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(&bytes[0]));
  }
  return result;
}

ArtMethod* DeoptimizationRequest::Method() const {
  return jni::DecodeArtMethod(method_);
}

void DeoptimizationRequest::SetMethod(ArtMethod* m) {
  method_ = jni::EncodeArtMethod(m);
}

void Dbg::VisitRoots(RootVisitor* visitor) {
  // Visit breakpoint roots, used to prevent unloading of methods with breakpoints.
  ReaderMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
  BufferedRootVisitor<128> root_visitor(visitor, RootInfo(kRootVMInternal));
  for (Breakpoint& breakpoint : gBreakpoints) {
    breakpoint.Method()->VisitRoots(root_visitor, kRuntimePointerSize);
  }
}

void Dbg::DbgThreadLifecycleCallback::ThreadStart(Thread* self) {
  Dbg::PostThreadStart(self);
}

void Dbg::DbgThreadLifecycleCallback::ThreadDeath(Thread* self) {
  Dbg::PostThreadDeath(self);
}

void Dbg::DbgClassLoadCallback::ClassLoad(Handle<mirror::Class> klass ATTRIBUTE_UNUSED) {
  // Ignore ClassLoad;
}
void Dbg::DbgClassLoadCallback::ClassPrepare(Handle<mirror::Class> temp_klass ATTRIBUTE_UNUSED,
                                             Handle<mirror::Class> klass) {
  Dbg::PostClassPrepare(klass.Get());
}

}  // namespace art

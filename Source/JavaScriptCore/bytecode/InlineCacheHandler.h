/*
 * Copyright (C) 2014-2022, 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

// An InlineCacheHandler is one node of a property IC's handler chain. The chain and its per-node data
// are shared with LLInt, so they are available without the JIT; the compiled-thunk entries and the
// stub routine that owns generated code are gated below.

#include "AccessCase.h"
#include "PropertyInlineCacheClearingWatchpoint.h"
#include <wtf/RefCounted.h>

#if ENABLE(JIT)
#include "CallLinkInfo.h"
#include "JITStubRoutine.h"
#endif

namespace JSC {

class CodeBlock;
class InlineCacheCompiler;
class InlineCacheHandlerWithJSCall;
class PolymorphicAccessJITStubRoutine;
class PropertyInlineCache;

enum class CacheType : int8_t {
    Unset,
    GetByIdSelf,
    GetByIdPrototype,
    PutByIdReplace,
    InByIdSelf,
    Stub,
    ArrayLength,
    StringLength,
};

// Arms the replacement watchpoint for a get-by-id Load/GetGetter whose conditions are all
// watchpoint-backed, and returns the CacheType the resulting node carries. Shared by the
// precompiled-thunk path and the codegen-free LLInt path so the two cannot disagree. The sibling
// branches in compileOneAccessCaseHandler arm the same watchpoint but need no CacheType.
CacheType prepareGetByIdLoadNode(VM&, const AccessCase&);

// Size of the DataIC prologue a handler's call entry runs. Needed without the JIT too: LLInt's
// per-node jump target is callTarget + this, so the prologue runs once per chain traversal.
#if CPU(X86_64)
static constexpr size_t prologueSizeInBytesDataIC = 1;
#elif CPU(ARM64E)
static constexpr size_t prologueSizeInBytesDataIC = 4;
#elif CPU(ARM64)
static constexpr size_t prologueSizeInBytesDataIC = 0;
#elif CPU(ARM_THUMB2)
static constexpr size_t prologueSizeInBytesDataIC = 0;
#elif CPU(RISCV64)
static constexpr size_t prologueSizeInBytesDataIC = 0;
#else
// The CPUs that get C_LOOP have no entry above, and C_LOOP emits no DataIC prologue: backends are
// mutually exclusive offlineasm settings, so getByIdLLIntHandlerPrologue's X86_64/ARM64 tests are
// both false under the C_LOOP backend whatever the host CPU is.
static constexpr size_t prologueSizeInBytesDataIC = 0;
#endif

// The outcome of an attempt to add a case to a cache. Pure data -- it names a Kind and carries the
// resulting handler -- so it lives here rather than in the JIT-only InlineCacheCompiler.h; the
// get_by_id caching decision inspects it without the JIT.
class AccessGenerationResult {
public:
    enum Kind {
        MadeNoChanges,
        GaveUp,
        Buffered,
        GeneratedNewCode,
        GeneratedFinalCode, // Generated so much code that we never want to generate code again.
        GeneratedMegamorphicCode, // Generated so much code that we never want to generate code again. And this is megamorphic code.
        ResetStubAndFireWatchpoints // We found out some data that makes us want to start over fresh with this stub. Currently, this happens when we detect poly proto.
    };

    AccessGenerationResult() = default;

    AccessGenerationResult(Kind kind)
        : m_kind(kind)
    {
        RELEASE_ASSERT(kind != GeneratedNewCode);
        RELEASE_ASSERT(kind != GeneratedFinalCode);
        RELEASE_ASSERT(kind != GeneratedMegamorphicCode);
    }

    AccessGenerationResult(Kind, Ref<InlineCacheHandler>&&);

    Kind kind() const { return m_kind; }

    bool madeNoChanges() const { return m_kind == MadeNoChanges; }
    bool gaveUp() const { return m_kind == GaveUp; }
    bool buffered() const { return m_kind == Buffered; }
    bool generatedNewCode() const { return m_kind == GeneratedNewCode; }
    bool generatedFinalCode() const { return m_kind == GeneratedFinalCode; }
    bool generatedMegamorphicCode() const { return m_kind == GeneratedMegamorphicCode; }
    bool shouldResetStubAndFireWatchpoints() const { return m_kind == ResetStubAndFireWatchpoints; }

    // If we gave up on this attempt to generate code, or if we generated the "final" code, then we
    // should give up after this.
    bool shouldGiveUpNow() const { return gaveUp() || generatedFinalCode(); }

    bool generatedSomeCode() const { return generatedNewCode() || generatedFinalCode() || generatedMegamorphicCode(); }

    void dump(PrintStream&) const;

    void addWatchpointToFire(InlineWatchpointSet& set, StringFireDetail detail)
    {
        m_watchpointsToFire.append(std::pair<InlineWatchpointSet&, StringFireDetail>(set, detail));
    }
    void fireWatchpoints(VM& vm)
    {
        ASSERT(m_kind == ResetStubAndFireWatchpoints);
        for (auto& pair : m_watchpointsToFire)
            pair.first.invalidate(vm, pair.second);
    }

    InlineCacheHandler* handler() const { return m_handler.get(); }

private:
    Kind m_kind { MadeNoChanges };
    RefPtr<InlineCacheHandler> m_handler;
    Vector<std::pair<InlineWatchpointSet&, StringFireDetail>> m_watchpointsToFire;
};

class JSC_CACHE_LINE_ALIGNED InlineCacheHandler : public RefCounted<InlineCacheHandler> {
    WTF_MAKE_NONCOPYABLE(InlineCacheHandler);
    WTF_MAKE_TZONE_ALLOCATED(InlineCacheHandler);
    friend class InlineCacheCompiler;
    friend class InlineCacheHandlerWithJSCall;
public:
#if ENABLE(JIT)
    static Ref<InlineCacheHandler> create(Ref<InlineCacheHandler>&&, CodeBlock*, PropertyInlineCache&, Ref<PolymorphicAccessJITStubRoutine>&&, std::unique_ptr<PropertyInlineCacheClearingWatchpoint>&&, unsigned callLinkInfoCount);
    static Ref<InlineCacheHandler> createPreCompiled(Ref<InlineCacheHandler>&&, CodeBlock*, PropertyInlineCache&, Ref<PolymorphicAccessJITStubRoutine>&&, std::unique_ptr<PropertyInlineCacheClearingWatchpoint>&&, AccessCase&, CacheType);
#endif

    void operator delete(InlineCacheHandler*, std::destroying_delete_t);

#if ENABLE(JIT)
    CodePtr<JITStubRoutinePtrTag> callTarget() const { return m_callTarget; }
    CodePtr<JITStubRoutinePtrTag> jumpTarget() const { return m_jumpTarget; }
#endif

    void aboutToDie();
    bool containsPC(void* pc) const
    {
#if ENABLE(JIT)
        if (!m_stubRoutine)
            return false;

        uintptr_t pcAsInt = std::bit_cast<uintptr_t>(pc);
        return m_stubRoutine->startAddress() <= pcAsInt && pcAsInt <= m_stubRoutine->endAddress();
#else
        UNUSED_PARAM(pc);
        return false;
#endif
    }

    // If this returns false then we are requesting a reset of the owning PropertyInlineCache.
    bool visitWeak(VM&);

    void dump(PrintStream&) const;

    static Ref<InlineCacheHandler> createNonHandlerSlowPath(CodePtr<JITStubRoutinePtrTag>);

    // The VM-shared terminal node for an access type: the last link of every handler chain, which calls
    // back into C++. Lives here rather than on InlineCacheCompiler because it builds a handler rather
    // than generating code, and a non-JIT build needs it too.
    static Ref<InlineCacheHandler> sharedSlowPathHandler(VM&, AccessType);

    void addOwner(CodeBlock*);
    void removeOwner(CodeBlock*);

#if ENABLE(JIT)
    PolymorphicAccessJITStubRoutine* stubRoutine() { return m_stubRoutine.get(); }
#endif

    InlineCacheHandler* next() const { return m_next.get(); }
    void setNext(RefPtr<InlineCacheHandler>&& next)
    {
        m_next = WTF::move(next);
    }

    AccessCase* accessCase() const { return m_accessCase.get(); }
    void setAccessCase(RefPtr<AccessCase>&& accessCase)
    {
        m_accessCase = WTF::move(accessCase);
    }

    bool makesJSCalls() const { return m_makesJSCalls; }

#if ENABLE(JIT)
    static constexpr ptrdiff_t offsetOfCallTarget() { return OBJECT_OFFSETOF(InlineCacheHandler, m_callTarget); }
    static constexpr ptrdiff_t offsetOfJumpTarget() { return OBJECT_OFFSETOF(InlineCacheHandler, m_jumpTarget); }
#endif
    static constexpr ptrdiff_t offsetOfLLIntCallTarget() { return OBJECT_OFFSETOF(InlineCacheHandler, m_llintCallTarget); }
    static constexpr ptrdiff_t offsetOfLLIntJumpTarget() { return OBJECT_OFFSETOF(InlineCacheHandler, m_llintJumpTarget); }
    static constexpr ptrdiff_t offsetOfNext() { return OBJECT_OFFSETOF(InlineCacheHandler, m_next); }
    static constexpr ptrdiff_t offsetOfUid() { return OBJECT_OFFSETOF(InlineCacheHandler, m_uid); }
    static constexpr ptrdiff_t offsetOfStructureID() { return OBJECT_OFFSETOF(InlineCacheHandler, m_structureID); }
    static constexpr ptrdiff_t offsetOfOffset() { return OBJECT_OFFSETOF(InlineCacheHandler, m_offset); }
    static constexpr ptrdiff_t offsetOfNewStructureID() { return OBJECT_OFFSETOF(InlineCacheHandler, u.s2.m_newStructureID); }
    static constexpr ptrdiff_t offsetOfNewSize() { return OBJECT_OFFSETOF(InlineCacheHandler, u.s2.m_newSize); }
    static constexpr ptrdiff_t offsetOfOldSize() { return OBJECT_OFFSETOF(InlineCacheHandler, u.s2.m_oldSize); }
    static constexpr ptrdiff_t offsetOfHolder() { return OBJECT_OFFSETOF(InlineCacheHandler, u.s1.m_holder); }
    static constexpr ptrdiff_t offsetOfGlobalObject() { return OBJECT_OFFSETOF(InlineCacheHandler, u.s1.m_globalObject); }
    static constexpr ptrdiff_t offsetOfCustomAccessor() { return OBJECT_OFFSETOF(InlineCacheHandler, u.s1.m_customAccessor); }
    static constexpr ptrdiff_t offsetOfModuleNamespaceObject() { return OBJECT_OFFSETOF(InlineCacheHandler, u.s3.m_moduleNamespaceObject); }
    static constexpr ptrdiff_t offsetOfModuleVariableSlot() { return OBJECT_OFFSETOF(InlineCacheHandler, u.s3.m_moduleVariableSlot); }

    StructureID structureID() const { return m_structureID; }
    PropertyOffset offset() const { return m_offset; }
    JSCell* holder() const { return u.s1.m_holder; }
    size_t newSize() const { return u.s2.m_newSize; }
    size_t oldSize() const { return u.s2.m_oldSize; }
    StructureID newStructureID() const { return u.s2.m_newStructureID; }

    CacheType cacheType() const { return m_cacheType; }

    DECLARE_VISIT_AGGREGATE;

    // This returns true if it has marked everything it will ever marked. This can be used as an
    // optimization to then avoid calling this method again during the fixpoint.
    template<typename Visitor> void propagateTransitions(Visitor&) const;

protected:
    InlineCacheHandler();
#if ENABLE(JIT)
    InlineCacheHandler(bool makesJSCalls, Ref<InlineCacheHandler>&&, Ref<PolymorphicAccessJITStubRoutine>&&, std::unique_ptr<PropertyInlineCacheClearingWatchpoint>&&, CacheType);
#endif

    static Ref<InlineCacheHandler> createSlowPath(VM&, AccessType);

    // The selection and ordering of the fields through m_uid is deliberate.
    // They are are either hot with high affinity, or placed where they are to minimize padding.
    StructureID m_structureID { };
    RefPtr<InlineCacheHandler> m_next;
#if ENABLE(JIT)
    // Entries into this node's compiled thunk. A build without the JIT has no compiled chain, so these
    // are omitted rather than left null -- they sit inside the hot prefix, hence the two-valued
    // offsetOfUid() assert below.
    CodePtr<JITStubRoutinePtrTag> m_callTarget;
    CodePtr<JITStubRoutinePtrTag> m_jumpTarget;
#endif
    PropertyOffset m_offset { invalidOffset };
    CacheType m_cacheType { CacheType::Unset };
    bool m_makesJSCalls { false };
    UniquedStringImpl* m_uid { nullptr };

    union {
        struct {
            StructureID m_newStructureID { };
            unsigned m_newSize { };
            unsigned m_oldSize { };
        } s2 { };
        struct {
            JSCell* m_holder;
            JSGlobalObject* m_globalObject;
            void* m_customAccessor;
        } s1;
        struct {
            JSObject* m_moduleNamespaceObject;
            WriteBarrierBase<Unknown>* m_moduleVariableSlot;
        } s3;
    } u;
#if ENABLE(JIT)
    RefPtr<PolymorphicAccessJITStubRoutine> m_stubRoutine;
#endif
    // Unconditional: InlineCacheHandler::visitWeak reads only m_accessCase and m_stubRoutine, so a node
    // without an access case is invisible to GC and its cached StructureID would dangle.
    RefPtr<AccessCase> m_accessCase;
    std::unique_ptr<PropertyInlineCacheClearingWatchpoint> m_watchpoint;
    // LLInt Handler-IC entries: runtime addresses of a static offlineasm handler chosen from
    // m_cacheType / makesJSCalls (see llintCallTargetForHandler in InlineCacheHandler.cpp).
    // m_llintCallTarget is the call entry (runs the DataIC prologue); m_llintJumpTarget is the
    // chain-walk entry (== callTarget + prologueSizeInBytesDataIC, skipping the prologue) so the
    // prologue runs exactly once per chain traversal, exactly like m_callTarget/m_jumpTarget.
    // Tagged JITStubRoutinePtrTag with no address diversity, like m_callTarget. Set at node creation.
    // Placed after m_watchpoint so the hot region ending at m_uid is undisturbed.
    CodePtr<JITStubRoutinePtrTag> m_llintCallTarget;
    CodePtr<JITStubRoutinePtrTag> m_llintJumpTarget;
};

#if !ASSERT_ENABLED && !ASAN_ENABLED && CPU(ARM64) && CPU(ADDRESS64)
#if ENABLE(JIT)
static_assert(InlineCacheHandler::offsetOfUid() == 40, "InlineCacheHandler hot field layout drifted.");
#else
// m_callTarget/m_jumpTarget are absent without the JIT, so the hot prefix is 16 bytes shorter.
static_assert(InlineCacheHandler::offsetOfUid() == 24, "InlineCacheHandler hot field layout drifted.");
#endif
#endif

#if ENABLE(JIT)

class InlineCacheHandlerWithJSCall final : public InlineCacheHandler {
    WTF_MAKE_TZONE_ALLOCATED(InlineCacheHandlerWithJSCall);
    friend class InlineCacheHandler;
    friend class InlineCacheCompiler;
public:
    CallLinkInfo* callLinkInfo(const ConcurrentJSLocker&) { return &m_callLinkInfo; }

    static constexpr ptrdiff_t offsetOfCallLinkInfo() { return OBJECT_OFFSETOF(InlineCacheHandlerWithJSCall, m_callLinkInfo); }

private:
    InlineCacheHandlerWithJSCall(Ref<InlineCacheHandler>&&, Ref<PolymorphicAccessJITStubRoutine>&&, std::unique_ptr<PropertyInlineCacheClearingWatchpoint>&&, CacheType);

    DataOnlyCallLinkInfo m_callLinkInfo;
};

#endif // ENABLE(JIT)

// Out of line because it needs InlineCacheHandler complete.
inline AccessGenerationResult::AccessGenerationResult(Kind kind, Ref<InlineCacheHandler>&& handler)
    : m_kind(kind)
    , m_handler(WTF::move(handler))
{
    RELEASE_ASSERT(kind == GeneratedNewCode || kind == GeneratedFinalCode || kind == GeneratedMegamorphicCode);
}

} // namespace JSC

#if ENABLE(JIT)
SPECIALIZE_TYPE_TRAITS_BEGIN(JSC::InlineCacheHandlerWithJSCall)
    static bool isType(const JSC::InlineCacheHandler& handler) { return handler.makesJSCalls(); }
SPECIALIZE_TYPE_TRAITS_END()
#endif

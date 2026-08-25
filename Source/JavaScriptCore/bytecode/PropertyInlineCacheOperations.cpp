/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PropertyInlineCacheOperations.h"

#include "CacheableIdentifierInlines.h"
#include "CodeBlock.h"
#include "ICStats.h"
#include "JSCJSValueInlines.h"
#include "PropertyInlineCacheOperationsInlines.h"
#include "PropertyNameInlines.h"
#include "Repatch.h"
#include "SuperSampler.h"

// DECLARE_CALL_FRAME uses __builtin_frame_address(1).
IGNORE_WARNINGS_BEGIN("frame-address")

namespace JSC {

JSC_DEFINE_JIT_OPERATION(operationGetByIdOptimize, EncodedJSValue, (EncodedJSValue base, PropertyInlineCache* propertyCache))
{
    SuperSamplerScope superSamplerScope(false);

    JSGlobalObject* globalObject = propertyCache->globalObject();
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    ICSlowPathCallFrameTracer tracer(vm, callFrame, propertyCache);
    auto scope = DECLARE_THROW_SCOPE(vm);

    CacheableIdentifier identifier = propertyCache->identifier();

    JSValue baseValue = JSValue::decode(base);

    OPERATION_RETURN(scope, JSValue::encode(baseValue.getPropertySlot(globalObject, identifier, [&] (bool found, PropertySlot& slot) -> JSValue {

        LOG_IC((ICEvent::OperationGetByIdOptimize, baseValue.classInfoOrNull(), baseValue == slot.slotBase()));

#if ENABLE(JIT)
        CodeBlock* codeBlock = callFrame->codeBlock();
        if (propertyCache->considerRepatchingCacheBy(vm, codeBlock, baseValue.structureOrNull(), identifier))
            repatchGetBy(globalObject, codeBlock, baseValue, identifier, slot, *propertyCache, GetByKind::ById, /* isNonStringPrimitiveKey */ false);
#endif
        // Without the JIT the chain has no node factory yet, so this does the lookup and leaves the
        // cache at its terminal: correct, and slow until the codegen-free factory lands.
        return found ? slot.getValue(globalObject, identifier) : jsUndefined();
    })));
}

} // namespace JSC

IGNORE_WARNINGS_END


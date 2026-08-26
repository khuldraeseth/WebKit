#!/usr/bin/env ruby
# Copyright (C) 2026 Apple Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Lowers LowLevelInterpreter.asm for a backend without needing a target toolchain.
#
# asm.rb reads struct offsets out of a compiled LLIntOffsetsExtractor, so normally you
# cannot run offlineasm for a configuration you cannot build. Lowering only needs a
# node->value map, so this substitutes synthetic values and discards the output. That
# checks parsing, macro expansion, register names, instruction selection, and -- given
# InitBytecodes.asm via --entry-labels -- that every setEntryAddress target is really emitted.
# It cannot check anything that depends on an offset's actual value, and it cannot run code.

$LOAD_PATH.unshift(File.join(File.dirname(File.dirname(File.dirname(File.realpath(__FILE__)))), "Source", "JavaScriptCore", "offlineasm"))

require "backends"
require "config"
require "instructions"
require "offsets"
require "parser"
require "self_hash"
require "settings"
require "transform"
require "optparse"
require "set"
require "stringio"

# The backends emit through an Assembler, which lives in asm.rb -- a script whose main body
# runs on load. Take just the class definition.
ASM_RB = File.join(File.dirname(File.dirname(File.dirname(File.realpath(__FILE__)))),
                   "Source", "JavaScriptCore", "offlineasm", "asm.rb")
asmSource = File.readlines(ASM_RB)
classFirst = asmSource.index { | line | line.start_with?("class Assembler") }
raise "asm.rb: no 'class Assembler'" unless classFirst
classLast = (classFirst...asmSource.size).find { | i | asmSource[i] == "end\n" }
raise "asm.rb: unterminated 'class Assembler'" unless classLast
eval(asmSource[classFirst..classLast].join, TOPLEVEL_BINDING, ASM_RB, classFirst + 1)

$options = {}
backends = nil
listOnly = false
includes = []
overrides = {}
entryLabelsFile = nil
dumpFile = nil
OptionParser.new do |opts|
    opts.banner = "Usage: offlineasm-lower-check.rb --include=DERIVED_SOURCES_DIR --backend=NAME [--on=A,B] [--list]"
    opts.on("--backend=NAMES", "Comma-separated backends to lower for (default: all working ones).") do |names|
        backends = names.split(/[,\s]+/)
    end
    opts.on("--on=SETTINGS", "Comma-separated non-backend settings to enable; all others are off.") do |names|
        names.split(/[,\s]+/).each { | name | overrides[name] = true }
    end
    opts.on("--include=PATH", "Directory holding generated .asm includes (InitBytecodes.asm etc).") do |path|
        includes << path
    end
    opts.on("--list", "List the settings that would be used, then exit.") do
        listOnly = true
    end
    opts.on("--entry-labels=INIT_BYTECODES_ASM", "Also check every setEntryAddress target is defined.") do |path|
        entryLabelsFile = path
    end
    opts.on("--dump=FILE", "Write the lowered output for the last configuration here.") do |path|
        dumpFile = path
    end
    opts.on("--webkit-additions-path=PATH", "WebKitAdditions path.") do |path|
        $options[:webkit_additions_path] = path
    end
end.parse!

# processIncludeOptions reads -I flags straight off ARGV and walks off the end unless
# something non-'-I' follows, so keep a terminator in place.
ARGV.replace(includes.collect { | path | "-I#{path}" } + ["--"])
IncludeFile.processIncludeOptions

asmFile = File.join(File.dirname(File.dirname(File.dirname(File.realpath(__FILE__)))),
                    "Source", "JavaScriptCore", "llint", "LowLevelInterpreter.asm")

# Values are not arbitrary. Sizes and constants reach Address scale fields (PtrSize is
# `constexpr (sizeof(void*))`, and scaleValue rejects anything but 1/2/4/8), so those get a
# pointer size. Struct offsets are only ever immediates, so vary them to exercise both the
# fits-in-a-field and the materialize-into-a-temp paths.
def syntheticMapping(ast, pointerSize, offsetScale)
    map = {}
    offsetsList(ast).each_with_index { | node, i | map[node] = offsetScale * (i % 32) }
    sizesList(ast).each { | node | map[node] = pointerSize }
    constsList(ast).each { | node | map[node] = pointerSize }
    map
end

ast = parse(asmFile, $options, Set.new)

# Every 'op :foo' in BytecodeList.rb becomes a setEntryAddress reference in the generated
# InitBytecodes.asm, and offlineasm does not check those resolve -- a label declared there but
# defined only under, say, 'if JSVALUE64' fails at link instead, in a configuration nobody builds.
entryLabels = []
if entryLabelsFile
    File.readlines(entryLabelsFile).each {
        | line |
        match = line.match(/^setEntryAddress(?:Wide16|Wide32)?\(\d+,\s*_(\w+)\)/)
        entryLabels << match[1] if match
    }
    entryLabels.uniq!
    raise "No setEntryAddress lines in #{entryLabelsFile}" if entryLabels.empty?
end

# computeSettingsCombinations enumerates the full cross product -- ~2^14 per backend, far
# more than is useful here. Build the one map we want instead: requested settings on,
# every other non-backend setting off.
allSettingNames = ast.filter(Setting).uniq.collect { | v | v.name }
nonBackendSettings = allSettingNames.reject { | name | isBackend? name }
unknown = overrides.keys - nonBackendSettings
raise "Not a non-backend setting: #{unknown.join(", ")}" unless unknown.empty?

failures = []
lowered = 0
# 8 keeps every offset inside a load/store immediate field; 4096 pushes most of them out of
# it, so both address-mode lowerings get exercised.
offsetScales = [8, 4096]

(backends || WORKING_BACKENDS).each {
    | backend |
    raise "Unknown backend: #{backend}" unless BACKENDS.include? backend

    concreteSettings = {}
    nonBackendSettings.each { | name | concreteSettings[name] = overrides.fetch(name, false) }
    BACKENDS.each { | name | concreteSettings[name] = (name == backend) }

    described = ([backend] + overrides.keys.sort).join(" ")

    if listOnly
        puts described
        next
    end

    begin
        forSettings(concreteSettings, ast) {
            | settings, lowLevelAST, selectedBackend |
            if isASTErroneous(lowLevelAST)
                failures << [described, RuntimeError.new("AST has an error node at #{lowLevelAST.filter(Error)[0].codeOrigin}")]
                next
            end

            lowLevelAST = lowLevelAST.demacroify({})

            pointerSize = (backend == "ARMv7") ? 4 : 8
            offsetScales.each {
                | offsetScale |
                scaleDescription = "#{described} offsetScale=#{offsetScale}"
                resolved = lowLevelAST.resolve(syntheticMapping(lowLevelAST, pointerSize, offsetScale))
                resolved.validate

                # Lowering writes through the global $asm/$output; throw the text away.
                $output = StringIO.new
                $asm = Assembler.new($output)
                $currentSettings = settings
                Label.resetReferenced
                $enableDebugAnnotations = false
                begin
                    $asm.inAsm {
                        resolved.lower(selectedBackend)
                    }
                rescue => e
                    failures << [scaleDescription, e]
                    next
                end
                File.write(dumpFile, $output.string) if dumpFile

                # Checking the emitted text rather than the AST is deliberate: label definitions are
                # registered globally at parse time, before resolveSettings prunes the branch they came
                # from, so a Label can look defined in a configuration that never emits it. Which offsets
                # we invented cannot affect this, so only check once per configuration.
                if !entryLabels.empty? && offsetScale == offsetScales.first
                    emitted = Set.new($output.string.scan(/OFFLINE_ASM_[A-Z0-9_]*LABEL\((\w+)\)/).flatten)
                    # Opcode labels lose their llint_ prefix on the way out: setEntryAddress references
                    # _llint_op_foo, but the definition is emitted as OFFLINE_ASM_OPCODE_LABEL(op_foo).
                    missing = entryLabels.reject { | name |
                        emitted.include?(name) || emitted.include?(name.sub(/^llint_/, ""))
                    }
                    unless missing.empty?
                        failures << [described, RuntimeError.new("#{missing.size} setEntryAddress target(s) referenced but never emitted, e.g. #{missing.first(4).join(", ")}")]
                    end
                end

                lowered += 1
            }
        }
    rescue => e
        failures << [described, e]
    end
}

exit 0 if listOnly

failures.each {
    | described, e |
    $stderr.puts "FAIL [#{described}]"
    $stderr.puts "  #{e.class}: #{e.message}"
    $stderr.puts e.backtrace.take(6).collect { | l | "    #{l}" }.join("\n") if e.backtrace
}

puts "lowered #{lowered} configuration(s), #{failures.size} failure(s)"
exit failures.empty? ? 0 : 1

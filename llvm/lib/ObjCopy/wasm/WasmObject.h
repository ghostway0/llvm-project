//===- WasmObject.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_OBJCOPY_WASM_WASMOBJECT_H
#define LLVM_LIB_OBJCOPY_WASM_WASMOBJECT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/Wasm.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/Wasm.h"
#include "llvm/ObjectYAML/WasmYAML.h"
#include "llvm/Support/MemoryBuffer.h"
#include <_types/_uint8_t.h>
#include <vector>

namespace llvm {
namespace objcopy {
namespace wasm {

struct Section {
  // For now, each section is only an opaque binary blob with no distinction
  // between custom and known sections.
  uint8_t SectionType;
  std::optional<uint8_t> HeaderSecSizeEncodingLen;
  StringRef Name;
  ArrayRef<uint8_t> Contents;
  std::optional<uint32_t> RelocationSectionIdx;
};

struct SegmentInfo {
  uint32_t Index;
  StringRef Name;
  uint32_t Alignment;
  uint32_t Flags;
};

struct Signature {
  uint32_t Index;
  uint32_t Form = llvm::wasm::WASM_TYPE_FUNC;
  uint32_t ParamTypes;
  std::vector<uint32_t> ReturnTypes;
};

struct InitFunction {
  uint32_t Priority;
  uint32_t Symbol;
};

struct ComdatEntry {
  uint32_t Kind;
  uint32_t Index;
};

struct Comdat {
  StringRef Name;
  std::vector<ComdatEntry> Entries;
};

struct Object {
  llvm::wasm::WasmObjectHeader Header;
  std::vector<object::WasmSymbol> Symbols;
  std::vector<Section> OpaqueSections;
  llvm::wasm::WasmLinkingData LinkingData;

  Section *LinkingSection = nullptr;

  void addSectionWithOwnedContents(Section NewSection,
                                   std::unique_ptr<MemoryBuffer> &&Content);
  void removeSections(function_ref<bool(const Section &)> ToRemove);
  std::vector<uint8_t> finalizeLinking();

private:
  std::vector<std::unique_ptr<MemoryBuffer>> OwnedContents;
};

} // end namespace wasm
} // end namespace objcopy
} // end namespace llvm

#endif // LLVM_LIB_OBJCOPY_WASM_WASMOBJECT_H
